#pragma once

#include <Arduino.h>

// Web UI の静的リソース（フラッシュに置き、RAM へは展開しない）。
// 動的ページ（設定・ステータス）も /app.css を共有して見た目を揃える。

static const char kAppCss[] PROGMEM = R"CSS(
:root{--bg:#0e1014;--card:#171a21;--card2:#11141a;--line:#262b35;--text:#e9ebf1;--sub:#8b92a3;--acc:#4ade80;--warn:#fbbf24;--err:#f87171;--blue:#60a5fa;--pur:#c084fc}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;font-family:system-ui,-apple-system,"Hiragino Sans","Noto Sans JP","Yu Gothic UI",sans-serif;background:var(--bg);color:var(--text);line-height:1.5}
header{position:sticky;top:0;z-index:5;background:rgba(14,16,20,.9);backdrop-filter:blur(8px);border-bottom:1px solid var(--line)}
.bar{max-width:760px;margin:0 auto;padding:10px 16px;display:flex;align-items:center;gap:10px;flex-wrap:wrap}
.brand{font-weight:800;letter-spacing:.02em;white-space:nowrap}
nav{margin-left:auto;display:flex;gap:2px}
nav a{color:var(--sub);text-decoration:none;padding:6px 11px;border-radius:9px;font-size:14px;white-space:nowrap}
nav a.on{color:var(--text);background:var(--line)}
main{max-width:760px;margin:0 auto;padding:6px 16px 48px}
h1{font-size:22px;margin:14px 0 4px}
h3{margin:0 0 12px;font-size:15px;color:var(--sub);font-weight:700;letter-spacing:.03em}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin:12px 0}
.chip{display:inline-flex;align-items:center;gap:6px;padding:3px 11px;border-radius:999px;font-size:13px;font-weight:700;background:var(--line);white-space:nowrap}
.chip i{width:8px;height:8px;border-radius:50%;background:var(--sub);display:inline-block}
.ok{color:var(--acc)}.warn{color:var(--warn)}.err{color:var(--err)}
.chip.ok i{background:var(--acc)}.chip.warn i{background:var(--warn)}.chip.err i{background:var(--err)}
.hint{font-size:13px;color:var(--sub);margin:4px 0 0}
label{display:block;margin-top:14px;font-size:14px}
input,select,textarea{box-sizing:border-box;width:100%;padding:11px 12px;margin-top:6px;border-radius:10px;border:1px solid var(--line);background:var(--card2);color:var(--text);font-size:15px;font-family:inherit}
input:focus,select:focus,textarea:focus{outline:2px solid var(--blue);outline-offset:-1px}
input[type=range]{padding:0;border:0;background:none;accent-color:var(--acc);height:28px}
label.check{display:flex;align-items:center;gap:10px;margin-top:14px}
label.check input{width:22px;height:22px;margin:0;flex:none;accent-color:var(--acc)}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:0 12px}
@media(max-width:520px){.grid2{grid-template-columns:1fr}}
button,.btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:11px 16px;border:0;border-radius:11px;background:var(--acc);color:#0b1a10;font-weight:800;font-size:15px;cursor:pointer;font-family:inherit;text-decoration:none}
button.ghost{background:var(--line);color:var(--text)}
button:disabled{opacity:.5}
.save{position:sticky;bottom:0;padding:12px 0;background:linear-gradient(transparent,var(--bg) 35%)}
.save button{width:100%;padding:14px}
table{width:100%;border-collapse:collapse}
td{padding:7px 2px;vertical-align:top;border-bottom:1px solid var(--line)}
tr:last-child td{border-bottom:0}
td:first-child{color:var(--sub);width:42%;white-space:nowrap}
pre{white-space:pre-wrap;word-break:break-word;background:var(--card2);border-radius:10px;padding:12px;margin:10px 0 0}
a{color:var(--blue)}
)CSS";

static const char kDashboardHtml[] PROGMEM = R"HTML(<!doctype html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>StackChan Printer</title><link rel="stylesheet" href="/app.css">
<style>
.hero{display:flex;gap:20px;align-items:center}
.ring{position:relative;width:136px;height:136px;flex:none}
.ring svg{transform:rotate(-90deg)}
.ring circle{transition:stroke-dashoffset .8s ease,stroke .4s}
.pct{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center}
.pct b{font-size:34px;line-height:1;font-variant-numeric:tabular-nums}
.pct small{color:var(--sub);font-size:12px;margin-top:4px}
.info{min-width:0;flex:1}
.job{font-size:18px;font-weight:800;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.meta{display:grid;grid-template-columns:auto 1fr;gap:3px 12px;margin-top:8px;font-size:14px}
.meta dt{color:var(--sub)}.meta dd{margin:0;font-variant-numeric:tabular-nums}
.bar2{height:8px;background:var(--line);border-radius:6px;overflow:hidden;margin-top:12px}
.bar2 i{display:block;height:100%;width:0;background:var(--acc);transition:width .8s ease}
.kv{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}
.kv div{background:var(--card2);border-radius:12px;padding:10px 12px}
.kv small{display:block;color:var(--sub);font-size:12px}
.kv span{font-size:20px;font-weight:700;font-variant-numeric:tabular-nums}
.kv em{font-style:normal;color:var(--sub);font-size:13px}
.tbar{height:4px;background:var(--line);border-radius:3px;margin-top:6px;overflow:hidden}
.tbar i{display:block;height:100%;background:var(--warn)}
.trays{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.tray{border-radius:12px;padding:8px 6px;background:var(--card2);text-align:center;border:2px solid transparent;font-size:13px}
.tray.on{border-color:var(--acc)}
.sw{height:30px;border-radius:8px;margin-bottom:6px;border:1px solid #0008;box-shadow:inset 0 0 0 1px #fff2}
.tray small{color:var(--sub)}
.actions{display:flex;flex-wrap:wrap;gap:8px}
.say{display:flex;gap:8px;margin-top:12px}
.say input{margin:0}
.say button{flex:none}
.log{list-style:none;padding:0;margin:14px 0 0}
.log li{display:flex;gap:10px;padding:10px 0;border-top:1px solid var(--line)}
.log .face{font-size:20px;flex:none;width:28px;text-align:center}
.log small{color:var(--sub);display:block;font-size:12px}
.hms li{font-family:ui-monospace,Menlo,Consolas,monospace}
.empty{color:var(--sub);text-align:center;padding:10px 0}
@media(max-width:480px){.hero{flex-direction:column;align-items:stretch}.ring{margin:0 auto}}
</style></head><body>
<header><div class="bar"><span class="brand">StackChan × Bambu</span><span id="link" class="chip"><i></i>…</span><span id="mode" class="chip" hidden></span>
<nav><a class="on" href="/">プリンター</a><a href="/settings">設定</a><a href="/status">状態</a></nav></div></header>
<main>
<section class="card" id="off" hidden><h3>プリンター未設定</h3>
<p>設定画面で Bambu Lab プリンターの IP アドレス・シリアル番号・アクセスコードを入力すると、ここに状態が表示され、スタックチャンが実況します。</p>
<a class="btn" href="/settings#printer">プリンターを設定する</a></section>
<section class="card"><div class="hero">
<div class="ring"><svg width="136" height="136" viewBox="0 0 136 136"><circle cx="68" cy="68" r="58" stroke="#262b35" stroke-width="13" fill="none"/>
<circle id="arc" cx="68" cy="68" r="58" stroke="#4ade80" stroke-width="13" fill="none" stroke-linecap="round" stroke-dasharray="364.4" stroke-dashoffset="364.4"/></svg>
<div class="pct"><b id="pct">--</b><small id="phase">--</small></div></div>
<div class="info"><div class="job" id="job">--</div>
<dl class="meta"><dt>残り</dt><dd id="rem">--</dd><dt>完成予定</dt><dd id="eta">--</dd><dt>レイヤー</dt><dd id="layer">--</dd><dt>工程</dt><dd id="stage">--</dd><dt>速度</dt><dd id="speed">--</dd></dl>
</div></div><div class="bar2"><i id="bar"></i></div></section>
<section class="card"><h3>温度・ファン</h3><div class="kv">
<div><small>ノズル</small><span id="tN">--</span> <em id="tNt"></em><div class="tbar"><i id="bN"></i></div></div>
<div><small>ベッド</small><span id="tB">--</span> <em id="tBt"></em><div class="tbar"><i id="bB"></i></div></div>
<div><small>チャンバー</small><span id="tC">--</span></div>
<div><small>ファン（部品/補助/庫内）</small><span id="fans">--</span></div>
</div></section>
<section class="card" id="amsCard" hidden><h3>AMS</h3><div class="trays" id="trays"></div></section>
<section class="card" id="hmsCard" hidden><h3>お知らせ（HMS）</h3><ul class="hms" id="hms"></ul><p class="hint">コードは Bambu Lab Wiki の HMS 一覧で検索できます。</p></section>
<section class="card"><h3>スタックチャンの実況</h3>
<div class="actions"><button id="bReport">🗣 今の状況を話して</button><button id="bMode" class="ghost" hidden>モード --</button><button id="bVoice" class="ghost">実況 --</button><button id="bLight" class="ghost">💡 ライト</button><button id="bRefresh" class="ghost">↻ 再取得</button></div>
<div class="say"><input id="sayText" maxlength="120" placeholder="スタックチャンに言わせる（テスト）"><button id="bSay" class="ghost">話す</button></div>
<ul class="log" id="log"></ul></section>
</main>
<script>
const $=i=>document.getElementById(i);
const C=364.4;let st={};
const moods={happy:'😆',sad:'😢',doubt:'🤔',angry:'😠',sleepy:'😪',neutral:'🙂'};
const post=(u,b)=>fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b||''}).then(r=>r.json()).catch(()=>({}));
const esc=s=>String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const rem=m=>m==null||m<0?'--':(m>=60?Math.floor(m/60)+'時間'+(m%60)+'分':m+'分');
const t=v=>v==null?'--':Math.round(v)+'℃';
const ago=s=>s<60?s+'秒前':s<3600?Math.floor(s/60)+'分前':Math.floor(s/3600)+'時間前';
const LINK={online:['接続中','ok'],connecting:['接続試行中','warn'],waiting_wifi:['Wi-Fi待ち','warn'],error:['接続エラー','err'],disabled:['未設定','']};
const MODE={mqtt:'MQTT モード',llm:'LOCAL LLM',level:'LEVEL HOLD'};
const PC={RUNNING:'#4ade80',PREPARE:'#60a5fa',SLICING:'#60a5fa',PAUSE:'#fbbf24',FINISH:'#c084fc',FAILED:'#f87171'};
function render(d){
 st=d;const l=LINK[d.link]||['?',''];const lk=$('link');
 lk.className='chip '+l[1];lk.innerHTML='<i></i>'+l[0]+(d.link=='error'&&d.mqtt_error!=null?' ('+d.mqtt_error+')':'');
 $('off').hidden=d.enabled;
 const act=['RUNNING','PREPARE','PAUSE','SLICING'].includes(d.phase);
 const p=d.percent==null?null:(d.phase=='FINISH'?100:d.percent);
 $('pct').textContent=p==null?'--':p+'%';$('phase').textContent=d.phase_ja||'--';
 const col=PC[d.phase]||'#8b92a3';$('arc').style.stroke=col;$('bar').style.background=col;
 $('arc').style.strokeDashoffset=C*(1-(p||0)/100);$('bar').style.width=(p||0)+'%';
 $('job').textContent=d.job||(act?'（名前なし）':'ジョブなし');
 $('rem').textContent=act?rem(d.remaining_min):'--';$('eta').textContent=act&&d.eta?d.eta:'--';
 $('layer').textContent=d.layer!=null&&d.total_layers>0?d.layer+' / '+d.total_layers:'--';
 $('stage').textContent=d.stage_ja||'--';$('speed').textContent=d.speed||'--';
 $('tN').textContent=t(d.nozzle);$('tNt').textContent=d.nozzle_target>0?'→ '+t(d.nozzle_target):'';
 $('tB').textContent=t(d.bed);$('tBt').textContent=d.bed_target>0?'→ '+t(d.bed_target):'';
 $('bN').style.width=Math.min(100,(d.nozzle||0)/300*100)+'%';$('bB').style.width=Math.min(100,(d.bed||0)/120*100)+'%';
 $('tC').textContent=t(d.chamber);
 const f=d.fans||{};const fv=v=>v==null?'-':v+'%';$('fans').textContent=fv(f.part)+' / '+fv(f.aux)+' / '+fv(f.chamber);
 const units=(d.ams&&d.ams.units)||[];$('amsCard').hidden=!units.length;
 let h='';units.forEach(u=>u.trays.forEach((tr,i)=>{const idx=u.id*4+i;const on=d.ams.now===idx;
  h+='<div class="tray'+(on?' on':'')+'"><div class="sw" style="background:'+(tr.present?tr.color:'repeating-linear-gradient(45deg,#222 0 6px,#2c2c2c 6px 12px)')+'"></div><b>'+(tr.present?esc(tr.type):'空')+'</b><br><small>'+String.fromCharCode(65+u.id)+(i+1)+(tr.present&&tr.remain>=0?' · '+tr.remain+'%':'')+'</small></div>';}));
 $('trays').innerHTML=h;
 const hms=d.hms||[];$('hmsCard').hidden=!hms.length;$('hms').innerHTML=hms.map(c=>'<li>'+esc(c)+'</li>').join('');
 const md=$('mode');md.hidden=!d.enabled;md.textContent=(MODE[d.mode]||'--')+(d.auto_mode?'（自動）':'');
 $('bMode').hidden=!d.enabled;$('bMode').textContent=d.mode=='mqtt'?'🤖 LOCAL LLM に戻す':'🖨 MQTT モードにする';
 $('bVoice').textContent=d.voice?'🔊 実況 ON':'🔇 実況 OFF';
 $('bLight').textContent=d.light===1?'💡 ライト ON':'💡 ライト OFF';
 const lg=d.log||[];$('log').innerHTML=lg.length?lg.map(e=>'<li><span class="face">'+(moods[e.mood]||'🙂')+'</span><div>'+esc(e.text)+'<small>'+ago(e.ago)+'</small></div></li>').join(''):'<li class="empty">まだ実況はありません</li>';
}
async function load(){try{const r=await fetch('/api/printer',{cache:'no-store'});render(await r.json());}catch(e){const lk=$('link');lk.className='chip err';lk.innerHTML='<i></i>StackChan 応答なし';}}
async function loop(){await load();setTimeout(loop,document.hidden?8000:2000);}
$('bReport').onclick=()=>post('/api/printer/report');
$('bRefresh').onclick=()=>post('/api/printer/refresh');
$('bMode').onclick=()=>post('/api/mode','mode='+(st.mode=='mqtt'?'llm':'mqtt')).then(()=>setTimeout(load,500));
$('bVoice').onclick=()=>post('/api/printer/voice','on='+(st.voice?0:1)).then(load);
$('bLight').onclick=()=>post('/api/printer/light','on='+(st.light===1?0:1));
$('bSay').onclick=()=>{const v=$('sayText').value.trim();if(v)post('/api/printer/say','text='+encodeURIComponent(v)).then(()=>{$('sayText').value='';});};
loop();
</script></body></html>)HTML";
