"""Capture release Web UI with sample data, without connecting to a printer.

Uses each Git tag's HTML/CSS and an isolated headless Chrome profile.
Requires Node.js 22+; animations are disabled only while capturing.
python tools/screenshots/capture_web.py --version v2.4.1
python tools/screenshots/capture_web.py                 # all published versions
"""

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[2]
VERSIONS = ["v2.0.0", "v2.1.0", "v2.2.0", "v2.3.0", "v2.3.1", "v2.3.2", "v2.4.0", "v2.4.1", "v2.5.0", "v2.5.1"]
SAMPLE = {
    "enabled": True, "voice": True, "mode": "mqtt", "auto_mode": True,
    "link": "online", "synced": True, "age": 1,
    "phase": "RUNNING", "phase_ja": "印刷中", "job": "StackChan スタンド",
    "percent": 62, "remaining_min": 42, "eta": "14:42", "layer": 155, "total_layers": 250,
    "stage": 0, "stage_ja": "印刷中", "speed": "スタンダード",
    "nozzle": 220, "nozzle_target": 220, "bed": 55, "bed_target": 55, "chamber": 37,
    "fans": {"part": 80, "aux": 0, "chamber": 60}, "light": 1, "hms": [],
    "ams": {"now": 0, "units": [{"id": 0, "humidity": 1, "trays": [
        {"present": True, "type": "PLA", "color": "#60a5fa", "remain": 68},
        {"present": True, "type": "PETG", "color": "#f8fafc", "remain": 82},
        {"present": True, "type": "PLA", "color": "#f472b6", "remain": 45},
        {"present": False, "type": "", "color": "#000000", "remain": -1},
    ]}]},
    "log": [
        {"text": "いま62%、155層目。全部で250層だよ。", "mood": "neutral", "ago": 5},
        {"text": "半分まで来たよ！順調にできているね。", "mood": "happy", "ago": 120},
        {"text": "印刷スタート！完成するのが楽しみだね。", "mood": "happy", "ago": 3600},
    ],
}


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT).decode("utf-8")


def resource(source, symbol, delimiter):
    return re.search(r"\b" + symbol + r'\[\].*?R"' + delimiter + r'\((.*?)\)' + delimiter + '"',
                     source, re.S).group(1).encode("utf-8")


def capture(version, browser):
    source = git("show", f"{version}:src/WebPages.h")
    html = resource(source, "kDashboardHtml", "HTML")
    css = resource(source, "kAppCss", "CSS")
    sample = json.dumps(SAMPLE, ensure_ascii=False).encode("utf-8")
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            paths = {"/": ("text/html; charset=utf-8", html),
                     "/app.css": ("text/css; charset=utf-8", css),
                     "/api/printer": ("application/json; charset=utf-8", sample)}
            resource = paths.get(self.path.split("?", 1)[0])
            if not resource:
                self.send_error(404)
                return
            requests.append(self.path)
            content_type, body = resource
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            self.send_error(405)  # No real printer/device actions in screenshots.

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    out = ROOT / "docs/screenshots" / version
    out.mkdir(parents=True, exist_ok=True)
    profile = ROOT / ".pio/screenshots/chrome-profile"
    profile.mkdir(parents=True, exist_ok=True)
    images = []
    try:
        result = subprocess.run([
            "node", str(ROOT / "tools/screenshots/capture_chrome.mjs"), str(browser),
            f"http://127.0.0.1:{server.server_port}/", str(out), str(profile),
        ], capture_output=True, timeout=60)
        if result.returncode or "/api/printer" not in requests:
            raise RuntimeError(result.stderr.decode("utf-8", "replace")[-2000:])
        images = json.loads(result.stdout)
        for entry in images:
            destination = out / entry["file"]
            entry["sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
            print(f"Captured {version}/{destination.name}", flush=True)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
    (out / "web-capture.json").write_text(json.dumps({
        "version": version, "commit": git("rev-parse", f"{version}^{{commit}}").strip(),
        "source": "src/WebPages.h", "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "capture": "Tagged Web UI rendered by Chrome with synthetic sample data; animations disabled; no live printer",
        "sample": SAMPLE, "images": images,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", choices=VERSIONS, action="append")
    parser.add_argument("--browser", type=pathlib.Path)
    args = parser.parse_args()
    browser = args.browser
    if browser is None:
        for candidate in [shutil.which("google-chrome"), shutil.which("chromium"),
                          "C:/Program Files/Google/Chrome/Application/chrome.exe",
                          "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"]:
            if candidate and pathlib.Path(candidate).is_file():
                browser = pathlib.Path(candidate)
                break
    if browser is None:
        raise SystemExit("Chrome/Chromium not found; pass --browser PATH")
    for version in args.version or VERSIONS:
        capture(version, browser)


if __name__ == "__main__":
    main()
