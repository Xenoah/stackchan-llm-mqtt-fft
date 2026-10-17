"""ブラウザUI（ログイン画面・メイン画面）の HTML/CSS/JS。

app.py から LOGIN_HTML / MAIN_HTML を import して使う。
UI は単一画面。fetch で各エンドポイントを叩き、JSON 応答で DOM を更新する。
"""

_STYLE = """
<style>
:root{--bg:#0f1115;--card:#1a1d24;--line:#2a2e38;--fg:#e8eaed;--mut:#8a909c;
--ok:#5bd277;--warn:#ffce6b;--err:#ff6b6b;--accent:#7fb4ff;--accent2:#76d275}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:var(--bg);
color:var(--fg);line-height:1.5}
.wrap{max-width:760px;margin:0 auto;padding:16px}
h1{font-size:1.25rem;margin:6px 0}
h3{margin:0 0 10px;font-size:1rem}
.topbar{display:flex;justify-content:space-between;align-items:center}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;
padding:14px 16px;margin:12px 0}
label{display:block;margin-top:10px;font-size:.9rem;color:var(--mut)}
input,select,textarea{width:100%;padding:10px;margin-top:4px;border-radius:9px;
border:1px solid var(--line);background:#0c0e12;color:var(--fg);font-size:1rem}
textarea{resize:vertical}
.row{display:flex;gap:10px;flex-wrap:wrap}
.row>div{flex:1;min-width:120px}
button{padding:11px 16px;border:0;border-radius:10px;background:var(--accent2);
color:#0c0e12;font-weight:700;cursor:pointer;font-size:.95rem;margin-top:12px}
button.sec{background:#2c313c;color:var(--fg)}
button.mini{padding:7px 11px;margin-top:0;font-size:.82rem}
.pills{display:flex;gap:8px;flex-wrap:wrap;font-size:.85rem}
.pill{padding:4px 10px;border-radius:999px;background:#0c0e12;border:1px solid var(--line)}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:5px}
.dot.ok{background:var(--ok)}.dot.ng{background:var(--err)}.dot.q{background:var(--mut)}
.muted{color:var(--mut);font-size:.85rem}
.ans{white-space:pre-wrap;background:#0c0e12;border:1px solid var(--line);
border-radius:9px;padding:10px;margin-top:6px}
.tag{font-size:.72rem;color:var(--mut);text-transform:uppercase;letter-spacing:.04em}
.hist{border-top:1px solid var(--line);padding:12px 0}
.hist:first-child{border-top:0}
.histq{font-weight:600}
.btns{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}
audio{width:100%;margin-top:8px}
.toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);
background:#2c313c;color:#fff;padding:10px 18px;border-radius:10px;opacity:0;
transition:opacity .25s;pointer-events:none}
.toast.show{opacity:1}
.err{color:var(--err)}.ok{color:var(--ok)}
</style>
"""

LOGIN_HTML = (
    """<!doctype html><html lang='ja'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>StackChan Gateway ログイン</title>"""
    + _STYLE
    + """</head><body><div class='wrap'>
<h1>StackChan Gateway</h1>
<form class='card' method='post' action='/login'>
<h3>ログイン</h3>
<p class='err'><!--ERR--></p>
<label>パスワード
<input type='password' name='password' autofocus placeholder='初期パスワード: stackchan'></label>
<button type='submit'>ログイン</button>
<p class='muted'>初期パスワードは <b>stackchan</b> です。ログイン後、設定画面の
「パスワード変更」から変更できます。</p>
</form></div></body></html>"""
)


MAIN_HTML = (
    """<!doctype html><html lang='ja'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>StackChan Gateway</title>"""
    + _STYLE
    + """</head><body><div class='wrap'>

<div class='topbar'>
  <h1>StackChan Gateway</h1>
  <form method='post' action='/logout'><button class='sec mini' type='submit'>ログアウト</button></form>
</div>

<!-- ステータス -->
<div class='card'>
  <h3>ステータス <button class='sec mini' onclick='refreshStatus()'>更新</button></h3>
  <div class='pills' id='statusPills'><span class='pill'>読み込み中…</span></div>
  <div class='muted' id='ipInfo' style='margin-top:8px'></div>
</div>

<!-- 質問 -->
<div class='card'>
  <h3>質問する</h3>
  <textarea id='question' rows='3' placeholder='StackChan に聞きたいことを入力…'></textarea>
  <div class='row'>
    <div><label>回答/読み上げ言語
      <select id='voice_lang'>
        <option value='ja'>日本語</option>
        <option value='en'>英語</option>
      </select></label></div>
    <div><label>回答長
      <select id='answer_mode'>
        <option value='hitokoto'>ひとこと</option>
        <option value='short'>短め</option>
        <option value='normal'>普通</option>
      </select></label></div>
  </div>
  <button id='askBtn' onclick='ask()'>送信して喋らせる</button>
</div>

<!-- 音声設定 -->
<div class='card'>
  <h3>音声設定</h3>
  <div class='row'>
    <div><label>TTS音声
      <select id='tts_voice'>
        <option value='default'>標準</option>
        <option value='f1'>女性 1</option>
        <option value='f2'>女性 2</option>
        <option value='m1'>男性 1</option>
        <option value='m2'>男性 2</option>
        <option value='m3'>男性 3</option>
      </select></label></div>
    <div><label>ピッチ (0-99) <input type='number' id='pitch' min='0' max='99'></label></div>
    <div><label>音量 (0-200) <input type='number' id='volume' min='0' max='200'></label></div>
    <div><label>速度 (80-260) <input type='number' id='speed' min='80' max='260'></label></div>
  </div>
  <label style='margin-top:12px'><input type='checkbox' id='kanji_to_kana' style='width:auto'> 漢字読み変換（日本語）</label>
  <label><input type='checkbox' id='auto_speak' style='width:auto'> 送信時に自動で喋らせる</label>
  <div class='btns'>
    <button onclick='saveSettings()'>設定保存</button>
    <button class='sec' onclick='rebuildVoice()'>音声だけ再生成</button>
  </div>
</div>

<!-- 接続先・プロンプト設定 -->
<div class='card'>
  <h3>接続先 / プロンプト</h3>
  <div class='row'>
    <div><label>StackChan Host <input id='stackchan_host' placeholder='192.168.0.50'></label></div>
    <div><label>StackChan Port <input type='number' id='stackchan_port' min='1' max='65535'></label></div>
  </div>
  <label>システムプロンプト<textarea id='system_prompt' rows='3'></textarea></label>
  <label>パスワード変更（変更時のみ入力）<input type='password' id='new_password' placeholder='新しいパスワード'></label>
  <button onclick='saveSettings()'>保存</button>
</div>

<!-- 回答カード -->
<div class='card' id='answerCard' style='display:none'>
  <h3>回答</h3>
  <div class='tag'>質問</div><div class='ans' id='ansQuestion'></div>
  <div class='tag' style='margin-top:8px'>表示回答</div><div class='ans' id='ansAnswer'></div>
  <div class='tag' style='margin-top:8px'>読み上げ用テキスト</div><div class='ans' id='ansSpeech'></div>
  <audio id='ansAudio' controls></audio>
  <div class='btns'>
    <button onclick='speakCurrent()'>StackChanでもう一度喋る</button>
    <button class='sec' onclick='rebuildVoice()'>音声だけ再生成</button>
  </div>
</div>

<!-- 履歴 -->
<div class='card'>
  <h3>履歴 <button class='sec mini' onclick='loadHistory()'>更新</button></h3>
  <div id='historyList'><span class='muted'>読み込み中…</span></div>
</div>

</div>
<div class='toast' id='toast'></div>

<script>
let CURRENT_ID = null;
let SETTINGS_APPLIED = false;
const TTS_VOICE_LABELS = {
  default:'標準', f1:'女性 1', f2:'女性 2', m1:'男性 1', m2:'男性 2', m3:'男性 3'
};

function toast(msg, isErr){
  const t=document.getElementById('toast');
  t.textContent=msg; t.className='toast show'+(isErr?' err':'');
  setTimeout(()=>{t.className='toast';}, 2600);
}
function esc(s){return (s||'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}

async function api(url, opts){
  const r = await fetch(url, opts);
  let data={}; try{data=await r.json();}catch(e){}
  if(r.status===401){ location.href='/'; throw new Error('unauthorized'); }
  return {ok:r.ok, status:r.status, data};
}
function form(obj){
  const f=new FormData(); for(const k in obj) if(obj[k]!==undefined&&obj[k]!==null) f.append(k,obj[k]); return f;
}

// --- ステータス ---
async function refreshStatus(){
  try{
    const {data}=await api('/status');
    if(!data.ok) return;
    const pills=[];
    pills.push(pill('Gateway', true));
    pills.push(pill('LLM', data.llm));
    pills.push(pill('StackChan', data.stackchan));
    pills.push(`<span class='pill'>履歴 ${data.history_count}</span>`);
    pills.push(`<span class='pill'>current ${data.current?'あり':'なし'}</span>`);
    document.getElementById('statusPills').innerHTML=pills.join('');
    const ips=(data.android_ips||[]).join(', ')||'不明';
    document.getElementById('ipInfo').innerHTML=
      `Android IP候補: <b>${esc(ips)}</b> &nbsp;|&nbsp; StackChan: ${esc(data.stackchan_host||'未設定')}:${data.stackchan_port}`;
    // 設定の反映は初回のみ（ポーリングで編集中の入力を上書きしないため）
    if(data.settings && !SETTINGS_APPLIED){ applySettings(data.settings); SETTINGS_APPLIED=true; }
  }catch(e){}
}
function pill(name, ok){
  const cls = ok?'ok':'ng';
  return `<span class='pill'><span class='dot ${cls}'></span>${name}</span>`;
}

function applySettings(s){
  setVal('voice_lang', s.voice_lang); setVal('answer_mode', s.answer_mode);
  setVal('tts_voice', s.tts_voice || 'default');
  setVal('pitch', s.pitch); setVal('volume', s.volume); setVal('speed', s.speed);
  setVal('stackchan_host', s.stackchan_host); setVal('stackchan_port', s.stackchan_port);
  setVal('system_prompt', s.system_prompt);
  document.getElementById('kanji_to_kana').checked=!!s.kanji_to_kana;
  document.getElementById('auto_speak').checked=!!s.auto_speak;
}
function setVal(id,v){ const el=document.getElementById(id); if(el&&v!==undefined&&v!==null) el.value=v; }
function gv(id){ return document.getElementById(id).value; }

// --- 設定保存 ---
async function saveSettings(){
  const body=form({
    stackchan_host:gv('stackchan_host'), stackchan_port:gv('stackchan_port'),
    voice_lang:gv('voice_lang'), answer_mode:gv('answer_mode'),
    tts_voice:gv('tts_voice'), pitch:gv('pitch'), volume:gv('volume'), speed:gv('speed'),
    kanji_to_kana:document.getElementById('kanji_to_kana').checked,
    auto_speak:document.getElementById('auto_speak').checked,
    system_prompt:gv('system_prompt'),
    new_password:gv('new_password')||undefined,
  });
  const {data}=await api('/settings',{method:'POST',body});
  if(data.ok){ toast('設定を保存しました'); document.getElementById('new_password').value=''; }
  else toast('保存に失敗: '+(data.error||''), true);
}

// --- 質問 ---
async function ask(){
  const q=gv('question').trim();
  if(!q){ toast('質問を入力してください', true); return; }
  const btn=document.getElementById('askBtn');
  btn.disabled=true; btn.textContent='生成中…';
  try{
    const {data}=await api('/ask',{method:'POST',body:form({
      question:q, voice_lang:gv('voice_lang'), answer_mode:gv('answer_mode'),
      tts_voice:gv('tts_voice'), pitch:gv('pitch'), volume:gv('volume'), speed:gv('speed'),
      kanji_to_kana:document.getElementById('kanji_to_kana').checked,
    })});
    if(!data.ok){ toast(data.error||'失敗しました', true); return; }
    showAnswer(data.item);
    if(!data.tts_ok) toast('音声生成に失敗: '+(data.tts_error||''), true);
    else if(data.speak && data.speak.ok===false) toast('StackChan発話: '+(data.speak.error||'busy'), true);
    else toast('送信しました');
    loadHistory(); refreshStatus();
  }finally{ btn.disabled=false; btn.textContent='送信して喋らせる'; }
}

function showAnswer(item){
  CURRENT_ID=item.id;
  document.getElementById('answerCard').style.display='block';
  document.getElementById('ansQuestion').textContent=item.question;
  document.getElementById('ansAnswer').textContent=item.answer;
  document.getElementById('ansSpeech').textContent=item.speech_text;
  const a=document.getElementById('ansAudio');
  a.src='/history/'+item.id+'.wav?t='+Date.now();
}

// --- current 操作 ---
async function speakCurrent(){
  const {data}=await api('/stackchan/speak',{method:'POST'});
  if(data.ok===false) toast('発話失敗: '+(data.error||'busy'), true);
  else toast('StackChanに送信しました');
}
async function rebuildVoice(){
  const body=form({
    id:CURRENT_ID||undefined, voice_lang:gv('voice_lang'),
    tts_voice:gv('tts_voice'), pitch:gv('pitch'), volume:gv('volume'), speed:gv('speed'),
    kanji_to_kana:document.getElementById('kanji_to_kana').checked,
  });
  const {data}=await api('/voice/rebuild',{method:'POST',body});
  if(data.ok){ toast('音声を再生成しました'); if(data.item) showAnswer(data.item); loadHistory(); }
  else toast('再生成失敗: '+(data.error||''), true);
}

// --- 履歴 ---
async function loadHistory(){
  const {data}=await api('/history');
  const box=document.getElementById('historyList');
  if(!data.ok||!data.items.length){ box.innerHTML="<span class='muted'>履歴はまだありません。</span>"; return; }
  box.innerHTML=data.items.map(renderHist).join('');
}
function renderHist(it){
  const when=(it.created_at||'').replace('T',' ').slice(0,19);
  const ttsVoice=TTS_VOICE_LABELS[it.tts_voice||'default']||it.tts_voice||'標準';
  const langInfo=(it.speech_lang&&it.speech_lang!==it.voice_lang)?`${it.voice_lang}/${it.speech_lang}`:it.voice_lang;
  return `<div class='hist'>
    <div class='histq'>${esc(it.question)}</div>
    <div class='muted'>${esc(it.answer)}</div>
    <div class='muted' style='font-size:.78rem'>読み: ${esc(it.speech_text)}</div>
    <div class='muted' style='font-size:.74rem'>${when} · ${langInfo} · ${ttsVoice} · p${it.pitch}/v${it.volume}/s${it.speed}</div>
    <audio controls preload='none' src='/history/${it.id}.wav?t=${Date.now()}'></audio>
    <div class='btns'>
      <button class='mini' onclick="histSpeak('${it.id}')">もう一度喋る</button>
      <button class='mini sec' onclick="histRevoice('${it.id}')">声だけ変える</button>
      <button class='mini sec' onclick="histDelete('${it.id}')">削除</button>
    </div></div>`;
}
async function histSpeak(id){
  const {data}=await api('/history/'+id+'/speak',{method:'POST'});
  if(data.ok){ toast('StackChanに送信しました'); CURRENT_ID=id; refreshStatus(); }
  else toast('失敗: '+(data.error||''), true);
}
async function histRevoice(id){
  const body=form({voice_lang:gv('voice_lang'),tts_voice:gv('tts_voice'),pitch:gv('pitch'),volume:gv('volume'),
    speed:gv('speed'),kanji_to_kana:document.getElementById('kanji_to_kana').checked});
  const {data}=await api('/history/'+id+'/revoice',{method:'POST',body});
  if(data.ok){ toast('声を変更しました'); loadHistory(); }
  else toast('失敗: '+(data.error||''), true);
}
async function histDelete(id){
  if(!confirm('この履歴を削除しますか？')) return;
  const {data}=await api('/history/'+id+'/delete',{method:'POST'});
  if(data.ok){ toast('削除しました'); loadHistory(); refreshStatus(); }
}

// --- 初期化 ---
refreshStatus(); loadHistory();
setInterval(refreshStatus, 15000);
</script>
</body></html>"""
)
