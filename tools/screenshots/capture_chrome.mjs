// Capture a local fixture in an isolated Chrome profile. Node.js 22+ required.
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';

const [executable, url, output, profile] = process.argv.slice(2);
if (!executable || !url || !output || !profile || new URL(url).hostname !== '127.0.0.1') {
  throw new Error('Usage: capture_chrome.mjs CHROME LOCAL_URL OUTPUT PROFILE');
}
const chrome = spawn(executable, [
  '--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run',
  '--no-default-browser-check', '--disable-background-networking',
  '--disable-component-update', '--disable-sync', '--disable-extensions',
  '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0',
  `--user-data-dir=${profile}`, 'about:blank',
], { windowsHide: true, stdio: ['ignore', 'ignore', 'pipe'] });

let socket;
let client;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
try {
  const endpoint = await new Promise((resolve, reject) => {
    let stderr = '';
    const timer = setTimeout(() => reject(new Error('Chrome startup timed out')), 15000);
    chrome.once('error', err => { clearTimeout(timer); reject(err); });
    chrome.once('exit', code => {
      clearTimeout(timer);
      reject(new Error(`Chrome exited early (${code}): ${stderr.slice(-1000)}`));
    });
    chrome.stderr.on('data', chunk => {
      stderr += chunk.toString();
      const match = stderr.match(/DevTools listening on (ws:\/\/[^\s]+)/);
      if (match) { clearTimeout(timer); resolve(match[1]); }
    });
  });
  socket = new WebSocket(endpoint);
  await once(socket, 'open');
  let nextId = 0;
  const pending = new Map();
  socket.addEventListener('message', event => {
    const reply = JSON.parse(event.data);
    const waiter = pending.get(reply.id);
    if (!waiter) return;
    pending.delete(reply.id);
    clearTimeout(waiter.timer);
    if (reply.error) waiter.reject(new Error(JSON.stringify(reply.error)));
    else waiter.resolve(reply.result);
  });
  client = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
    const id = ++nextId;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`${method} timed out`));
    }, 15000);
    pending.set(id, { resolve, reject, timer });
    socket.send(JSON.stringify({ id, method, params, sessionId }));
  });
  const { targetId } = await client('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await client('Target.attachToTarget', { targetId, flatten: true });
  const page = (method, params) => client(method, params, sessionId);
  await page('Page.enable');
  // Only screenshot timing changes; all tagged HTML/CSS, text and layout are retained.
  await page('Page.addScriptToEvaluateOnNewDocument', { source: `
    document.addEventListener('DOMContentLoaded', () => {
      const style = document.createElement('style');
      style.textContent = '* { transition: none !important; animation: none !important; }';
      document.head.appendChild(style);
    });
  ` });
  const images = [];
  for (const [kind, width, viewportHeight] of [['desktop', 1040, 900], ['mobile', 430, 900]]) {
    await page('Emulation.setDeviceMetricsOverride', {
      width, height: viewportHeight, deviceScaleFactor: 1, mobile: kind === 'mobile',
    });
    await page('Page.navigate', { url });
    let ready = false;
    for (let attempt = 0; attempt < 60; attempt++) {
      const { result } = await page('Runtime.evaluate', {
        expression: "document.getElementById('pct')?.textContent === '62%'",
        returnByValue: true,
      });
      if (result.value) { ready = true; break; }
      await pause(100);
    }
    if (!ready) throw new Error(`${kind}: sample state did not render`);
    await page('Runtime.evaluate', { expression: 'document.fonts.ready', awaitPromise: true });
    const { result, exceptionDetails } = await page('Runtime.evaluate', {
      expression: `new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(() => {
        const bar = document.getElementById('bar');
        resolve({
          width: innerWidth, scrollWidth: document.documentElement.scrollWidth,
          height: Math.max(document.body.scrollHeight, document.documentElement.scrollHeight),
          progress: parseFloat(getComputedStyle(document.getElementById('arc')).strokeDashoffset),
          barFraction: bar.getBoundingClientRect().width / bar.parentElement.getBoundingClientRect().width
        });
      })))`,
      awaitPromise: true, returnByValue: true,
    });
    if (exceptionDetails) throw new Error(JSON.stringify(exceptionDetails));
    const layout = result.value;
    if (layout.width !== width || layout.scrollWidth > width ||
        Math.abs(layout.progress - 138.472) > 0.01 || Math.abs(layout.barFraction - 0.62) > 0.001) {
      throw new Error(`${kind}: clipped layout or unfinished progress animation: ${JSON.stringify(layout)}`);
    }
    const height = Math.ceil(layout.height);
    const { data } = await page('Page.captureScreenshot', {
      format: 'png', captureBeyondViewport: true,
      clip: { x: 0, y: 0, width, height, scale: 1 },
    });
    const file = `web-${kind}.png`;
    await writeFile(join(output, file), Buffer.from(data, 'base64'));
    images.push({ file, width, height });
  }
  console.log(JSON.stringify(images));
} finally {
  if (client && socket?.readyState === WebSocket.OPEN) {
    await client('Browser.close').catch(() => {});
  }
  socket?.close();
  if (chrome.exitCode === null) {
    await Promise.race([once(chrome, 'exit'), pause(2000)]);
    if (chrome.exitCode === null) chrome.kill();
  }
}
