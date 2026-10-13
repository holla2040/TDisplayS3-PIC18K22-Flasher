#pragma once

// Served at "/". Status comes from /status (JSON), files go through /files and
// /file, scripts run via /run, and /update takes the flasher's own .ino.bin.
// Look: the jig's own PCB — soldermask green, silkscreen white, gold pads for
// anything you act on.
static const char PAGE[] = R"rawliteral(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PIC18Flasher</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Barlow+Semi+Condensed:wght@400;500;600&display=swap" rel="stylesheet">
<style>
:root{
  --mask:#0E3B2E; --mask-hi:#185240; --silk:#F2F1EC; --silk-dim:#A8C3B7;
  --enig:#D6B25E; --epoxy:#26282B; --laser:#A7ABB0;
  --good:#8FE3AE; --bad:#FF9B8C;
}
*{box-sizing:border-box}
body{margin:0;background:var(--mask);color:var(--silk);
  font:18px/1.5 "Barlow Semi Condensed","Arial Narrow",system-ui,sans-serif}
main{max-width:660px;margin:0 auto;padding:36px 16px 56px}
h1{margin:0;font-size:2.3rem;line-height:1.1;font-weight:600;letter-spacing:.01em}
h2{margin:0 0 4px;font-size:1.35rem;font-weight:600}
p{margin:0}

.jig{display:grid;grid-template-columns:minmax(0,1.15fr) minmax(0,1fr);gap:28px;align-items:center;
  margin:28px 0 40px}
.jig svg{width:100%;height:auto;display:block}
#pads rect{fill:var(--enig)}
.outline{fill:none;stroke:var(--silk);stroke-width:2}
.pin1{fill:var(--silk)}
#body{opacity:0;transform:translateY(-10px);transition:opacity .35s,transform .35s cubic-bezier(.2,.8,.3,1)}
.on #body{opacity:1;transform:none}
#mark{fill:var(--laser);font:600 17px "Barlow Semi Condensed","Arial Narrow",sans-serif;letter-spacing:.04em}
.det{font-size:1.7rem;font-weight:600;line-height:1.15;margin-bottom:8px}
.jig p:not(.det){color:var(--silk-dim)}
.counts b{color:var(--silk);font-weight:600}

.up{border-top:1.5px solid rgba(242,241,236,.28);padding:26px 0 30px}
.up>p{color:var(--silk-dim);max-width:34em;margin-bottom:16px}
.row{display:flex;gap:12px;flex-wrap:wrap}
.drop{flex:1 1 260px;display:flex;align-items:center;min-height:52px;padding:10px 16px;
  border:1.5px solid var(--enig);border-radius:7px;color:var(--enig);cursor:pointer;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.drop.over,.drop:hover{background:rgba(214,178,94,.12)}
.drop.has{color:var(--silk)}
.drop input{position:absolute;opacity:0;width:1px;height:1px}
.drop:focus-within,button:focus-visible{outline:2px solid var(--silk);outline-offset:3px}
button{flex:0 0 auto;min-height:52px;padding:0 22px;border:0;border-radius:7px;
  background:var(--enig);color:var(--mask);font:600 1.05rem "Barlow Semi Condensed","Arial Narrow",sans-serif;cursor:pointer}
button:disabled{opacity:.35;cursor:default}
.trace{height:3px;margin-top:16px;background:var(--mask-hi);border-radius:2px;overflow:hidden}
.trace i{display:block;height:100%;width:0;background:var(--enig);transition:width .15s}
.msg{min-height:1.5em;margin-top:8px}
.msg.good{color:var(--good)} .msg.bad{color:var(--bad)}

.sm{min-height:36px;padding:0 14px;font-size:.95rem}
.ghost{background:transparent;border:1.5px solid var(--enig);color:var(--enig)}
.ghost:hover{background:rgba(214,178,94,.12)}
table{width:100%;border-collapse:collapse;margin-bottom:6px}
td{padding:6px 0;border-bottom:1px solid rgba(242,241,236,.14);vertical-align:middle}
td.n{word-break:break-all}
td.s{color:var(--silk-dim);text-align:right;padding:6px 12px;white-space:nowrap}
td.a{text-align:right;white-space:nowrap}
td.a>*{margin-left:6px}
td.a a{display:inline-flex;align-items:center;min-height:36px;padding:0 14px;border:1.5px solid var(--enig);
  border-radius:7px;color:var(--enig);text-decoration:none;font-weight:600;font-size:.95rem}
td.a a:hover{background:rgba(214,178,94,.12)}
.id{font-variant-numeric:tabular-nums;letter-spacing:.04em}
.last{display:inline-block;padding:0 10px;border-radius:5px;font-weight:600;margin-left:6px}
.last.PASS{background:var(--good);color:var(--mask)} .last.FAIL{background:var(--bad);color:var(--mask)}
.last.other{background:var(--enig);color:var(--mask)}
textarea{width:100%;min-height:15em;padding:12px 14px;border:1.5px solid var(--enig);border-radius:7px;
  background:rgba(0,0,0,.25);color:var(--silk);font:15px/1.45 ui-monospace,Menlo,Consolas,monospace;resize:vertical}
textarea:focus-visible{outline:2px solid var(--silk);outline-offset:3px}

footer{color:var(--silk-dim);font-size:.95rem;border-top:1.5px solid rgba(242,241,236,.28);padding-top:18px}
@media (max-width:560px){.jig{grid-template-columns:1fr;gap:18px}}
@media (prefers-reduced-motion:reduce){#body,.trace i{transition:none}}
</style>
</head>
<body>
<main>
<h1>PIC18Flasher</h1>

<section class="jig" aria-live="polite">
  <svg viewBox="0 0 200 200" aria-hidden="true">
    <g id="pads"></g>
    <rect class="outline" x="36" y="36" width="128" height="128" rx="3"/>
    <circle class="pin1" cx="22" cy="22" r="4"/>
    <g id="body">
      <rect x="40" y="40" width="120" height="120" rx="4" fill="var(--epoxy)"/>
      <circle cx="56" cy="56" r="6" fill="#383A3E"/>
      <text id="mark" x="100" y="108" text-anchor="middle"></text>
    </g>
  </svg>
  <div>
    <p class="det" id="det">Looking for a board</p>
    <p id="id"></p>
    <p class="counts"><b id="pass">0</b> passed, <b id="fail">0</b> failed since restart<span id="last"></span></p>
    <p id="mode"></p>
  </div>
</section>

<section class="up" id="files">
  <h2>Files</h2>
  <p>Scripts (.txt) and Intel HEX files on the flasher. A board on the jig runs
  <b>flash.txt</b> when it is inserted; Run starts any script on the board now. Uploading never runs anything.</p>
  <table><tbody id="list"></tbody></table>
  <p class="msg" role="status" id="filemsg"></p>
  <div class="row">
    <label class="drop"><input type="file"><span>Choose or drop a file</span></label>
    <button disabled>Upload</button>
  </div>
  <div class="trace"><i></i></div>
  <p class="msg" role="status"></p>
</section>

<section class="up" id="ed">
  <h2>flash.txt</h2>
  <p>One command per line; # starts a comment. {id} is the board's 16-digit user ID.</p>
  <textarea id="txt" spellcheck="false" aria-label="flash.txt"></textarea>
  <div class="row" style="margin-top:12px">
    <button class="ghost" id="load">Load</button>
    <button id="save">Save</button>
  </div>
  <p class="msg" role="status"></p>
</section>

<section class="up" id="fw">
  <h2>Flasher firmware</h2>
  <p>Updates this T-Display S3. Use TDisplayS3-PIC18K22-Flasher.ino.bin from the build
  folder, not the merged or bootloader .bin. The flasher restarts when the update is written.</p>
  <div class="row">
    <label class="drop"><input type="file" accept=".bin"><span>Choose or drop the .ino.bin file</span></label>
    <button disabled>Update flasher</button>
  </div>
  <div class="trace"><i></i></div>
  <p class="msg" role="status"></p>
</section>

<footer id="build">&nbsp;</footer>
</main>

<script>
const $ = (s, el = document) => el.querySelector(s);
const sleep = ms => new Promise(r => setTimeout(r, ms));

// QFP-44 footprint: 11 pads per side, 10-unit pitch
let pads = '';
for (let i = 0; i < 11; i++) {
  const p = 45 + i * 10;
  pads += `<rect x="${p}" y="8" width="6" height="24" rx="1.5"/><rect x="${p}" y="168" width="6" height="24" rx="1.5"/>` +
          `<rect x="8" y="${p}" width="24" height="6" rx="1.5"/><rect x="168" y="${p}" width="24" height="6" rx="1.5"/>`;
}
$('#pads').innerHTML = pads;

let md5 = '';
async function status() {
  const r = await fetch('/status', {cache: 'no-store', signal: AbortSignal.timeout(2500)});
  const s = await r.json();
  const on = s.detected !== 'no target';
  $('.jig').classList.toggle('on', on);
  if (on) $('#mark').textContent = s.detected;
  $('#det').textContent = on ? `${s.detected} on the jig` : 'No board on the jig';
  const idp = $('#id');
  idp.textContent = '';
  if (s.id) {
    idp.append('User ID ');
    const b = document.createElement('b');
    b.className = 'id';
    b.textContent = s.id === 'blank' ? 'blank (all FF)' : s.id;
    idp.append(b);
  }
  $('#pass').textContent = s.pass;
  $('#fail').textContent = s.fail;
  const last = $('#last');
  last.textContent = s.last ? ' ' + s.last : '';
  last.className = s.last ? 'last ' + (s.last === 'PASS' || s.last === 'FAIL' ? s.last : 'other') : '';
  $('#mode').textContent = s.auto ? 'Auto mode: each new board runs flash.txt once.'
                                  : 'Manual mode: press the flash button to run flash.txt.';
  $('#build').textContent = `Flasher firmware built ${s.build}`;
  md5 = s.md5;
  return s;
}
status().catch(() => {});
setInterval(() => status().catch(() => {}), 1000);

const fmsg = $('#filemsg');
const fsay = (t, c = '') => { fmsg.textContent = t; fmsg.className = 'msg ' + c; };

async function list() {
  const files = await (await fetch('/files', {cache: 'no-store'})).json();
  files.sort((a, b) => a.name.localeCompare(b.name));
  const tb = $('#list');
  tb.textContent = '';
  for (const f of files) {
    const tr = tb.insertRow();
    const n = tr.insertCell(), s = tr.insertCell(), a = tr.insertCell();
    n.className = 'n'; s.className = 's'; a.className = 'a';
    n.textContent = f.name;
    s.textContent = f.size.toLocaleString() + ' B';
    if (f.name.endsWith('.txt')) {
      const run = document.createElement('button');
      run.className = 'sm';
      run.textContent = 'Run';
      run.onclick = () => act('POST', '/run?name=' + encodeURIComponent(f.name), `${f.name} started.`);
      a.append(run);
    }
    const dl = document.createElement('a');
    dl.href = '/file?name=' + encodeURIComponent(f.name);
    dl.textContent = 'Download';
    a.append(dl);
    const del = document.createElement('button');
    del.className = 'sm ghost';
    del.textContent = 'Delete';
    del.onclick = async () => {
      if (!confirm(`Delete ${f.name}?`)) return;
      await act('DELETE', '/file?name=' + encodeURIComponent(f.name), `${f.name} deleted.`);
      list().catch(() => {});
    };
    a.append(del);
  }
  if (!files.length) tb.insertRow().insertCell().textContent = 'No files yet.';
}

async function act(method, url, ok) {
  try {
    const r = await fetch(url, {method});
    if (r.ok) fsay(ok, 'good');
    else fsay((await r.text()).trim() || `The flasher refused (HTTP ${r.status}).`, 'bad');
  } catch (e) {
    fsay('Lost the connection to the flasher.', 'bad');
  }
}

function wire(id, url, field, done) {
  const sec = $(id), inp = $('input', sec), btn = $('button', sec), drop = $('.drop', sec),
        name = $('.drop span', sec), bar = $('.trace i', sec), msg = $('.msg:last-of-type', sec);
  const hint = name.textContent;
  const say = (t, c = '') => { msg.textContent = t; msg.className = 'msg ' + c; };

  inp.onchange = () => {
    const f = inp.files[0];
    name.textContent = f ? f.name : hint;
    drop.classList.toggle('has', !!f);
    btn.disabled = !f;
    bar.style.width = 0;
    say('');
  };
  drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
  drop.ondragleave = () => drop.classList.remove('over');
  drop.ondrop = e => {
    e.preventDefault();
    drop.classList.remove('over');
    inp.files = e.dataTransfer.files;
    inp.onchange();
  };

  btn.onclick = () => {
    const f = inp.files[0];
    if (!f) return;
    const fd = new FormData();
    fd.append(field, f);
    const x = new XMLHttpRequest();
    x.open('POST', url);
    btn.disabled = true;
    x.upload.onprogress = e => {
      const p = Math.round(e.loaded / e.total * 100);
      bar.style.width = p + '%';
      say(`Uploading, ${p}%`);
    };
    x.onload = () => {
      if (x.status === 200) return done(say, btn, f);
      bar.style.width = 0;
      btn.disabled = false;
      say(x.responseText.trim() || `The flasher rejected the upload (HTTP ${x.status}).`, 'bad');
    };
    x.onerror = () => {
      btn.disabled = false;
      say('Lost the connection to the flasher during the upload. Try again.', 'bad');
    };
    x.send(fd);
  };
}

wire('#files', '/file', 'file', (say, btn, f) => {
  btn.disabled = false;
  say(`${f.name} saved.`, 'good');
  list().catch(() => {});
});

wire('#fw', '/update', 'firmware', async (say, btn) => {
  const old = md5;
  say('Update written. Restarting the flasher.');
  await sleep(3000);
  for (const t0 = Date.now(); Date.now() - t0 < 40000; await sleep(1000)) {
    try {
      const s = await status();
      btn.disabled = false;
      if (s.md5 === old) say('The flasher restarted but is still running the old firmware.', 'bad');
      else say('Updated. The flasher is running the new firmware.', 'good');
      return;
    } catch (e) {}
  }
  btn.disabled = false;
  say("The flasher hasn't come back after 40 seconds. Check its screen.", 'bad');
});

// flash.txt editor: Load / Save go through the same /file endpoint as everything else.
const ed = $('#ed'), txt = $('#txt'), emsg = $('.msg', ed);
const esay = (t, c = '') => { emsg.textContent = t; emsg.className = 'msg ' + c; };
$('#load').onclick = async () => {
  try {
    const r = await fetch('/file?name=flash.txt', {cache: 'no-store'});
    if (!r.ok) return esay('There is no flash.txt on the flasher yet.', 'bad');
    txt.value = await r.text();
    esay('Loaded.');
  } catch (e) {
    esay('Lost the connection to the flasher.', 'bad');
  }
};
$('#save').onclick = async () => {
  const fd = new FormData();
  fd.append('file', new Blob([txt.value], {type: 'text/plain'}), 'flash.txt');
  try {
    const r = await fetch('/file', {method: 'POST', body: fd});
    if (r.ok) { esay('Saved flash.txt.', 'good'); list().catch(() => {}); }
    else esay((await r.text()).trim() || `The flasher refused (HTTP ${r.status}).`, 'bad');
  } catch (e) {
    esay('Lost the connection to the flasher.', 'bad');
  }
};

list().catch(() => {});
$('#load').click();
</script>
</body>
</html>
)rawliteral";
