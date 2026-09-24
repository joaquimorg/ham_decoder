// Web interface served at "/". Self-contained: no external scripts or fonts,
// so it also works on the "RX-Analyzer" access point without internet.
static const char WEB_PAGE[] = R"HTML(<!DOCTYPE html>
<html lang="pt">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>RX Analyzer</title>
<style>
:root {
  --bg: #0e1116; --panel: #161b22; --panel2: #1c232d; --line: #2a3340;
  --text: #e6edf3; --muted: #8b98a8; --accent: #3fb950; --manual: #f0b429;
  --bad: #f85149; --mono: ui-monospace, "Cascadia Mono", Consolas, monospace;
}
@media (prefers-color-scheme: light) {
  :root { --bg: #f4f6f8; --panel: #ffffff; --panel2: #eef1f4; --line: #d5dbe1;
          --text: #1b2330; --muted: #5d6b7a; --accent: #1a7f37; --manual: #b7791f; --bad: #cf222e; }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text);
       font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
header { display: flex; flex-wrap: wrap; align-items: center; gap: 10px 16px;
         padding: 12px 16px; border-bottom: 1px solid var(--line); background: var(--panel); }
h1 { font-size: 18px; margin: 0; letter-spacing: .3px; }
.pills { display: flex; flex-wrap: wrap; gap: 6px; margin-left: auto; }
.pill { font-size: 12px; padding: 3px 9px; border-radius: 999px; background: var(--panel2);
        color: var(--muted); border: 1px solid var(--line); white-space: nowrap; }
.pill.ok { color: var(--accent); } .pill.bad { color: var(--bad); }
main { max-width: 1200px; margin: 0 auto; padding: 16px; display: grid; gap: 16px; }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 14px; }
.card h2 { font-size: 13px; font-weight: 600; text-transform: uppercase; letter-spacing: .6px;
           color: var(--muted); margin: 0 0 10px; }
.status { display: grid; grid-template-columns: minmax(200px, 1.4fr) repeat(6, minmax(90px, 1fr)); gap: 10px; }
.metric { background: var(--panel2); border-radius: 8px; padding: 10px 12px; }
.metric .k { font-size: 12px; color: var(--muted); }
.metric .v { font: 600 20px/1.3 var(--mono); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.metric.label .v { font-size: 22px; color: var(--accent); }
.metric .v.clip { color: var(--bad); }
.scope { position: relative; }
canvas { display: block; width: 100%; background: #000; border-radius: 6px; }
#spec { height: 150px; }
#wf { height: 300px; margin-top: 4px; cursor: crosshair; image-rendering: auto; }
.axis { position: relative; height: 18px; font: 11px var(--mono); color: var(--muted); }
.axis span { position: absolute; transform: translateX(-50%); }
.marker { position: absolute; top: 0; bottom: 18px; width: 0; border-left: 2px dashed var(--accent);
          pointer-events: none; display: none; }
.marker.manual { border-color: var(--manual); }
.marker.rtty { border-left: 2px dashed #58a6ff; }
.marker.rtty.space { border-left-style: dotted; }
select { background: var(--panel2); color: var(--text); border: 1px solid var(--line);
         border-radius: 6px; padding: 6px 8px; font: inherit; }
.hover { position: absolute; top: 6px; right: 8px; font: 12px var(--mono); color: #fff;
         background: rgba(0,0,0,.55); padding: 2px 6px; border-radius: 4px; pointer-events: none; }
.hint { font-size: 12px; color: var(--muted); margin-top: 6px; }
.decoded { font: 17px/1.55 var(--mono); background: var(--panel2); border-radius: 8px; padding: 12px;
        min-height: 120px; max-height: 260px; overflow-y: auto; white-space: pre-wrap;
        word-break: break-word; margin: 0; }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 8px 12px; }
.row + .row { margin-top: 12px; }
.grid2 { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 16px; }
label { font-size: 14px; }
input[type=number], input[type=text], input[type=password] {
  background: var(--panel2); color: var(--text); border: 1px solid var(--line);
  border-radius: 6px; padding: 6px 8px; font: 14px var(--mono); }
input[type=number] { width: 90px; }
input[type=range] { width: 180px; accent-color: var(--accent); }
button { background: var(--panel2); color: var(--text); border: 1px solid var(--line);
         border-radius: 6px; padding: 6px 12px; font: inherit; cursor: pointer; }
button:hover { border-color: var(--muted); }
button.primary { background: var(--accent); border-color: var(--accent); color: #fff; }
.seg { display: inline-flex; border: 1px solid var(--line); border-radius: 6px; overflow: hidden; }
.seg button { border: 0; border-radius: 0; }
.seg button.on { background: var(--accent); color: #fff; }
.seg button.on.manual { background: var(--manual); }
.val { font: 13px var(--mono); color: var(--muted); min-width: 60px; }
.note { font-size: 13px; color: var(--muted); }
@media (max-width: 900px) {
  .status { grid-template-columns: repeat(2, 1fr); }
  .metric.label { grid-column: 1 / -1; }
  #wf { height: 220px; }
}
</style>
</head>
<body>
<header>
  <h1>RX Analyzer</h1>
  <div class="pills">
    <span class="pill" id="link">a ligar...</span>
    <span class="pill" id="src">-</span>
    <span class="pill" id="net">-</span>
    <span class="pill" id="up">-</span>
    <span class="pill" id="load">-</span>
    <span class="pill" id="boot">-</span>
  </div>
</header>
<main>
  <section class="card">
    <div class="status">
      <div class="metric label"><div class="k">Sinal</div><div class="v" id="label">-</div></div>
      <div class="metric"><div class="k">TinyML</div><div class="v" id="ml">-</div></div>
      <div class="metric"><div class="k">SNR</div><div class="v" id="snr">-</div></div>
      <div class="metric"><div class="k">RMS</div><div class="v" id="rms">-</div></div>
      <div class="metric"><div class="k">Pico</div><div class="v" id="pk">-</div></div>
      <div class="metric"><div class="k">Tom CW</div><div class="v" id="tone">-</div></div>
      <div class="metric"><div class="k">Velocidade</div><div class="v" id="wpm">-</div></div>
      <div class="metric"><div class="k">RTTY marca/espaço</div><div class="v" id="rtty">-</div></div>
    </div>
  </section>

  <section class="card">
    <h2>Espectro e waterfall</h2>
    <div class="scope" id="scope">
      <canvas id="spec"></canvas>
      <canvas id="wf"></canvas>
      <div class="axis" id="axis"></div>
      <div class="marker" id="marker"></div>
      <div class="marker rtty" id="mMark"></div>
      <div class="marker rtty space" id="mSpace"></div>
      <div class="hover" id="hover" hidden></div>
    </div>
    <div class="hint">Verde: tom CW (âmbar em manual). Azul: marca (tracejado) e espaço (pontilhado) do RTTY. Clica no waterfall para fixar o tom CW nessa frequência.</div>
  </section>

  <section class="card">
    <div class="row" style="justify-content: space-between; margin-bottom: 10px">
      <h2 style="margin: 0">Texto CW</h2>
      <div class="row">
        <button id="copy">Copiar</button>
        <button id="clear">Limpar</button>
      </div>
    </div>
    <pre class="decoded" id="text"></pre>
  </section>

  <section class="card">
    <div class="row" style="justify-content: space-between; margin-bottom: 10px">
      <h2 style="margin: 0">Texto RTTY</h2>
      <div class="row">
        <button id="rcopy">Copiar</button>
        <button id="rclear">Limpar</button>
      </div>
    </div>
    <pre class="decoded" id="rtext"></pre>
  </section>

  <div class="grid2">
    <section class="card">
      <h2>Descodificador CW</h2>
      <div class="row">
        <span>Tom</span>
        <div class="seg"><button id="auto">Automático</button><button id="manual">Manual</button></div>
        <input type="number" id="toneHz" min="100" max="3500" step="1">
        <span class="note">Hz</span>
        <button id="applyTone">Aplicar</button>
      </div>
      <div class="row">
        <label for="contrast">Contraste mínimo</label>
        <input type="range" id="contrast" min="2" max="12" step="0.5">
        <span class="val" id="contrastVal"></span>
      </div>
      <div class="note" style="margin-top: 8px">Valores mais baixos descodificam sinais mais fracos, mas deixam passar mais ruído.</div>
    </section>

    <section class="card">
      <h2>Descodificador RTTY</h2>
      <div class="row">
        <label for="baud" style="min-width: 90px">Velocidade</label>
        <select id="baud">
          <option value="45.45">45,45 baud</option>
          <option value="50">50 baud</option>
          <option value="75">75 baud</option>
          <option value="100">100 baud</option>
        </select>
      </div>
      <div class="row">
        <label for="pol" style="min-width: 90px">Polaridade</label>
        <select id="pol">
          <option value="0">Automática</option>
          <option value="1">Normal (marca = tom baixo)</option>
          <option value="2">Invertida</option>
        </select>
      </div>
      <div class="note" style="margin-top: 8px">Os tons são detetados automaticamente (desvios de 170 a 850 Hz). Em automático, a marca é o tom que domina no tempo.</div>
    </section>

    <section class="card">
      <h2>Waterfall</h2>
      <div class="row">
        <label for="wfMin">Mínimo</label>
        <input type="range" id="wfMin" min="-140" max="-40" step="1">
        <span class="val" id="wfMinVal"></span>
      </div>
      <div class="row">
        <label for="wfMax">Máximo</label>
        <input type="range" id="wfMax" min="-80" max="0" step="1">
        <span class="val" id="wfMaxVal"></span>
      </div>
      <div class="note" style="margin-top: 8px">Estes valores ficam guardados neste browser.</div>
    </section>

    <section class="card">
      <h2>Rede Wi-Fi</h2>
      <form id="wifi">
        <div class="row"><label style="min-width: 80px" for="ssid">Rede</label><input type="text" id="ssid" name="ssid" maxlength="32" autocomplete="off" required></div>
        <div class="row"><label style="min-width: 80px" for="pass">Password</label><input type="password" id="pass" name="pass" maxlength="64"></div>
        <div class="row"><button class="primary" type="submit">Guardar e reiniciar</button><span class="note" id="wifiMsg"></span></div>
      </form>
    </section>
  </div>
</main>

<script>
(() => {
  const BINS = 300;
  const $ = id => document.getElementById(id);
  const spec = $('spec'), wf = $('wf');
  const sctx = spec.getContext('2d'), wctx = wf.getContext('2d');
  wf.width = BINS; wf.height = 300;
  const rowImg = wctx.createImageData(BINS, 1);

  let binHz = 12000 / 1024, rSeq = 0, tSeq = 0, uSeq = 0, first = true;
  let rtty = { mark: 0, space: 0 };
  let avg = new Float32Array(BINS).fill(-120);
  let cfg = { auto: true, manual: 700, contrast: 5 };
  let tone = 0;

  const store = {
    get(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : +v; } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch (e) {} }
  };
  let wfMin = store.get('wfMin', -110), wfMax = store.get('wfMax', -30);

  // Waterfall palette: black, blue, cyan, yellow, red, white.
  const stops = [[0,0,0,0],[.25,0,30,140],[.45,0,170,220],[.65,240,230,60],[.85,240,60,30],[1,255,255,255]];
  const lut = new Uint8ClampedArray(256 * 3);
  for (let i = 0; i < 256; i++) {
    const x = i / 255;
    let j = 1; while (j < stops.length - 1 && x > stops[j][0]) j++;
    const a = stops[j - 1], b = stops[j], t = (x - a[0]) / (b[0] - a[0]);
    for (let c = 0; c < 3; c++) lut[i * 3 + c] = a[c + 1] + (b[c + 1] - a[c + 1]) * t;
  }

  function maxHz() { return BINS * binHz; }

  function buildAxis() {
    const axis = $('axis'); axis.innerHTML = '';
    for (let f = 0; f <= maxHz(); f += 500) {
      const s = document.createElement('span');
      s.style.left = (f / maxHz() * 100) + '%';
      s.textContent = f;
      axis.appendChild(s);
    }
  }

  function decodeRow(b64) {
    const bin = atob(b64), row = new Float32Array(BINS);
    for (let i = 0; i < BINS; i++) row[i] = bin.charCodeAt(i) / 2 - 120;
    return row;
  }

  function drawRow(row) {
    wctx.drawImage(wf, 0, 0, BINS, wf.height - 1, 0, 1, BINS, wf.height - 1);
    const span = Math.max(1, wfMax - wfMin);
    for (let i = 0; i < BINS; i++) {
      let v = Math.round((row[i] - wfMin) / span * 255);
      v = v < 0 ? 0 : v > 255 ? 255 : v;
      rowImg.data[i * 4] = lut[v * 3]; rowImg.data[i * 4 + 1] = lut[v * 3 + 1];
      rowImg.data[i * 4 + 2] = lut[v * 3 + 2]; rowImg.data[i * 4 + 3] = 255;
    }
    wctx.putImageData(rowImg, 0, 0);
    for (let i = 0; i < BINS; i++) avg[i] += (row[i] - avg[i]) * 0.3;
  }

  function drawSpectrum() {
    const dpr = window.devicePixelRatio || 1;
    const w = spec.clientWidth * dpr, h = spec.clientHeight * dpr;
    if (spec.width !== w || spec.height !== h) { spec.width = w; spec.height = h; }
    sctx.clearRect(0, 0, w, h);
    const top = 0, bottom = -130;
    const y = db => (db - top) / (bottom - top) * h;
    sctx.strokeStyle = 'rgba(255,255,255,.08)'; sctx.lineWidth = 1;
    sctx.font = (10 * dpr) + 'px monospace'; sctx.fillStyle = 'rgba(255,255,255,.35)';
    for (let db = -20; db > bottom; db -= 20) {
      sctx.beginPath(); sctx.moveTo(0, y(db)); sctx.lineTo(w, y(db)); sctx.stroke();
      sctx.fillText(db + ' dB', 4 * dpr, y(db) - 3 * dpr);
    }
    for (let f = 500; f < maxHz(); f += 500) {
      const x = f / maxHz() * w;
      sctx.beginPath(); sctx.moveTo(x, 0); sctx.lineTo(x, h); sctx.stroke();
    }
    const grad = sctx.createLinearGradient(0, 0, 0, h);
    grad.addColorStop(0, 'rgba(63,185,80,.55)'); grad.addColorStop(1, 'rgba(63,185,80,0)');
    sctx.beginPath(); sctx.moveTo(0, h);
    for (let i = 0; i < BINS; i++) sctx.lineTo((i + .5) / BINS * w, y(avg[i]));
    sctx.lineTo(w, h); sctx.closePath(); sctx.fillStyle = grad; sctx.fill();
    sctx.beginPath();
    for (let i = 0; i < BINS; i++) {
      const x = (i + .5) / BINS * w;
      i ? sctx.lineTo(x, y(avg[i])) : sctx.moveTo(x, y(avg[i]));
    }
    sctx.strokeStyle = '#3fb950'; sctx.lineWidth = 1.5 * dpr; sctx.stroke();
  }

  function place(el, f) {
    if (!f) { el.style.display = 'none'; return; }
    el.style.display = 'block';
    el.style.left = (f / maxHz() * 100) + '%';
  }

  function placeMarker() {
    place($('marker'), cfg.auto ? tone : cfg.manual);
    $('marker').classList.toggle('manual', !cfg.auto);
    place($('mMark'), rtty.mark);
    place($('mSpace'), rtty.space);
  }

  function appendText(id, add) {
    if (!add) return;
    const el = $(id);
    const atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
    el.textContent = (el.textContent + add).slice(-6000);
    if (atEnd) el.scrollTop = el.scrollHeight;
  }

  function fmtUptime(s) {
    const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
    return h ? `${h} h ${m} min` : `${m} min ${s % 60} s`;
  }

  function showCfg() {
    $('auto').classList.toggle('on', cfg.auto);
    $('manual').classList.toggle('on', !cfg.auto);
    $('manual').classList.toggle('manual', !cfg.auto);
    if (document.activeElement !== $('toneHz')) $('toneHz').value = Math.round(cfg.manual);
    if (document.activeElement !== $('contrast')) $('contrast').value = cfg.contrast;
    $('contrastVal').textContent = '×' + (+$('contrast').value).toFixed(1);
  }

  function update(d) {
    binHz = d.bin_hz || binHz;
    if (first) { buildAxis(); first = false; }
    if (d.rows) {
      for (const r of d.rows) drawRow(decodeRow(r));
      if (d.rows.length) drawSpectrum();
    }
    rSeq = d.r;

    appendText('text', d.text);
    appendText('rtext', d.rtext);
    tSeq = d.t;
    uSeq = d.u;
    rtty.mark = d.rtty_mark; rtty.space = d.rtty_space;
    $('rtty').textContent = d.rtty_mark
      ? `${d.rtty_mark.toFixed(0)}/${d.rtty_space.toFixed(0)}${d.rtty_on ? '' : ' (silêncio)'}` : '-';
    if (document.activeElement !== $('baud')) $('baud').value = String(+d.rtty_baud.toFixed(2));
    if (document.activeElement !== $('pol')) $('pol').value = String(d.rtty_pol);

    $('label').textContent = d.label || '-';
    $('ml').textContent = d.ml ? d.ml + ' ' + Math.round(d.ml_p * 100) + '%' : '-';
    $('snr').textContent = d.snr.toFixed(0) + ' dB';
    $('rms').textContent = d.rms.toFixed(0) + ' dBFS';
    $('pk').textContent = d.pk.toFixed(0) + ' dBFS';
    $('pk').classList.toggle('clip', d.clip);
    tone = d.tone;
    $('tone').textContent = d.tone ? d.tone.toFixed(0) + ' Hz' : '-';
    $('wpm').textContent = d.tone ? d.wpm.toFixed(0) + ' wpm' : '-';

    cfg.auto = d.cw_auto; cfg.manual = d.cw_manual; cfg.contrast = d.contrast;
    showCfg(); placeMarker();

    $('src').textContent = d.source;
    $('net').textContent = d.ip ? `${d.ssid} · ${d.ip} · ${d.rssi} dBm` : (d.ap ? 'ponto de acesso RX-Analyzer' : 'sem rede');
    $('net').className = 'pill' + (d.ip && d.rssi < -75 ? ' bad' : '');
    $('up').textContent = 'ligado há ' + fmtUptime(d.uptime);
    $('load').textContent = `análise ${d.load.toFixed(0)}% · ${d.heap} KB livres`;
    $('load').className = 'pill' + (d.load > 80 ? ' bad' : '');
    $('boot').textContent = `arranque: ${d.boot}` + (d.resets ? ` · ${d.resets} reinícios inesperados` : '');
    $('boot').className = 'pill' + (d.boot_bad || d.resets ? ' bad' : '');
    if (document.activeElement !== $('ssid') && !$('ssid').value) $('ssid').value = d.ssid || '';
  }

  function setLink(ok, text) {
    $('link').textContent = text; $('link').className = 'pill ' + (ok ? 'ok' : 'bad');
  }

  // Binary frame from the WebSocket: [1][n][n x BINS levels].
  function binaryRows(buf) {
    const v = new Uint8Array(buf);
    if (v[0] !== 1) return;
    const n = v[1];
    for (let k = 0; k < n; k++) {
      const row = new Float32Array(BINS), o = 2 + k * BINS;
      for (let i = 0; i < BINS; i++) row[i] = v[o + i] / 2 - 120;
      drawRow(row);
    }
    if (n) drawSpectrum();
  }

  // Live data over a WebSocket (the board pushes every 250 ms). Reconnects on
  // its own; after 3 failed attempts in a row it falls back to polling.
  let ws = null, wsFails = 0, lastMsg = 0, polling = false;
  function connectWs() {
    if (polling || ws || document.hidden) return;
    ws = new WebSocket(`ws://${location.host}/ws`);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => { wsFails = 0; polling = false; clearTimeout(timer); lastMsg = Date.now(); setLink(true, 'ao vivo'); };
    ws.onmessage = ev => {
      lastMsg = Date.now();
      if (typeof ev.data === 'string') update(JSON.parse(ev.data));
      else binaryRows(ev.data);
    };
    ws.onclose = () => {
      ws = null;
      if (document.hidden) return;              // closed on purpose
      setLink(false, 'sem ligação');
      if (++wsFails >= 3) {
        // Poll meanwhile, and try the WebSocket again every 30 s.
        polling = true; setLink(false, 'modo alternativo'); schedule(0);
        setTimeout(() => { polling = false; wsFails = 0; connectWs(); }, 30000);
      } else setTimeout(connectWs, 2000);
    };
  }
  setInterval(() => { if (ws && Date.now() - lastMsg > 5000) ws.close(); }, 2000);
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { if (ws) ws.close(); } else connectWs();
  });

  // Polling fallback. Each request has a timeout: a request left hanging used
  // to stop the polling loop for good. Polling pauses while the tab is hidden.
  let timer = null;
  function schedule(ms) { clearTimeout(timer); timer = setTimeout(poll, ms); }
  async function poll() {
    if (document.hidden || !polling) return;
    const ctl = new AbortController();
    const guard = setTimeout(() => ctl.abort(), 5000);
    try {
      const res = await fetch(`/api/data?r=${rSeq}&t=${tSeq}&u=${uSeq}`, { cache: 'no-store', signal: ctl.signal });
      update(await res.json());
      setLink(true, 'ao vivo (pedidos)');
      schedule(400);    // ~2.5 requests/s: fewer Wi-Fi bursts, same waterfall
    } catch (e) {
      setLink(false, 'sem ligação');
      schedule(1500);
    } finally {
      clearTimeout(guard);
    }
  }
  document.addEventListener('visibilitychange', () => { if (!document.hidden && polling) schedule(0); });

  async function postConfig(fields) {
    await fetch('/api/config', { method: 'POST', body: new URLSearchParams(fields) });
  }

  $('auto').onclick = () => { cfg.auto = true; showCfg(); placeMarker(); postConfig({ cw_auto: 1 }); };
  $('manual').onclick = () => { cfg.auto = false; showCfg(); placeMarker(); postConfig({ cw_auto: 0, cw_tone: $('toneHz').value }); };
  $('applyTone').onclick = () => { cfg.auto = false; cfg.manual = +$('toneHz').value; showCfg(); placeMarker();
                                   postConfig({ cw_auto: 0, cw_tone: $('toneHz').value }); };
  $('contrast').oninput = () => { $('contrastVal').textContent = '×' + (+$('contrast').value).toFixed(1); };
  $('contrast').onchange = () => postConfig({ contrast: $('contrast').value });

  function freqAt(ev) {
    const r = wf.getBoundingClientRect();
    return Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width)) * maxHz();
  }
  wf.addEventListener('mousemove', ev => { const h = $('hover'); h.hidden = false; h.textContent = freqAt(ev).toFixed(0) + ' Hz'; });
  wf.addEventListener('mouseleave', () => { $('hover').hidden = true; });
  wf.addEventListener('click', ev => {
    const f = Math.round(freqAt(ev));
    cfg.auto = false; cfg.manual = f; $('toneHz').value = f; showCfg(); placeMarker();
    postConfig({ cw_auto: 0, cw_tone: f });
  });

  function setupRange(id, key, apply) {
    const el = $(id), out = $(id + 'Val');
    el.value = key === 'wfMin' ? wfMin : wfMax;
    out.textContent = el.value + ' dB';
    el.oninput = () => { out.textContent = el.value + ' dB'; apply(+el.value); store.set(key, el.value); };
  }
  setupRange('wfMin', 'wfMin', v => { wfMin = v; });
  setupRange('wfMax', 'wfMax', v => { wfMax = v; });

  function copyButton(btn, src) {
    $(btn).onclick = async () => {
      try { await navigator.clipboard.writeText($(src).textContent); $(btn).textContent = 'Copiado'; }
      catch (e) { $(btn).textContent = 'Sem acesso'; }
      setTimeout(() => { $(btn).textContent = 'Copiar'; }, 1500);
    };
  }
  copyButton('copy', 'text');
  copyButton('rcopy', 'rtext');
  $('clear').onclick = () => { $('text').textContent = ''; };
  $('rclear').onclick = () => { $('rtext').textContent = ''; };
  $('baud').onchange = () => postConfig({ rtty_baud: $('baud').value });
  $('pol').onchange = () => postConfig({ rtty_pol: $('pol').value });

  $('wifi').onsubmit = async ev => {
    ev.preventDefault();
    $('wifiMsg').textContent = 'a guardar...';
    try {
      await fetch('/api/wifi', { method: 'POST', body: new URLSearchParams({ ssid: $('ssid').value, pass: $('pass').value }) });
      $('wifiMsg').textContent = 'Guardado. A placa vai reiniciar e ligar-se a esta rede; procura o novo endereço no monitor série ou no router.';
    } catch (e) { $('wifiMsg').textContent = 'Erro ao guardar.'; }
  };

  window.addEventListener('resize', drawSpectrum);
  connectWs();
})();
</script>
</body>
</html>
)HTML";
