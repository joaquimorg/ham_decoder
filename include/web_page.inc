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
.ftx { max-height: 420px; overflow-y: auto; background: var(--panel2); border-radius: 8px; }
.ftx table { width: 100%; border-collapse: collapse; font: 14px/1.5 var(--mono); }
.ftx th { position: sticky; top: 0; background: var(--panel2); color: var(--muted); font-weight: 600;
          text-align: left; padding: 6px 10px; border-bottom: 1px solid var(--line); }
.ftx td { padding: 3px 10px; white-space: nowrap; }
.ftx td.n { text-align: right; color: var(--muted); }
.ftx tr.cq td.msg { color: var(--accent); }
.ftx tr.new td { animation: flash 2s; }
@keyframes flash { from { background: rgba(88,166,255,.25); } to { background: transparent; } }
.imgbox { max-height: 75vh; overflow-y: auto; background: #000; border-radius: 6px; }
#img { image-rendering: auto; border-radius: 0; }
#img[hidden] { display: none; }
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

  <section class="card">
    <div class="row" style="justify-content: space-between; margin-bottom: 10px">
      <h2 style="margin: 0">FT8 / FT4</h2>
      <div class="row">
        <select id="ftxMode">
          <option value="0">Desligado</option>
          <option value="1">FT8</option>
          <option value="2">FT4</option>
        </select>
        <button id="ftxClear">Limpar</button>
      </div>
    </div>
    <div class="note" id="ftxInfo" style="margin-bottom: 8px">-</div>
    <div class="ftx">
      <table>
        <thead><tr><th>UTC</th><th>dB</th><th>DT</th><th>Hz</th><th>Mensagem</th></tr></thead>
        <tbody id="ftxRows"></tbody>
      </table>
    </div>
  </section>

  <section class="card">
    <div class="row" style="justify-content: space-between; margin-bottom: 10px">
      <h2 style="margin: 0">Imagem (FAX / SSTV)</h2>
      <div class="row">
        <button id="faxStart">Iniciar FAX agora</button>
        <button id="imgStop">Parar</button>
        <button id="imgSave">Guardar PNG</button>
        <button id="imgClear">Limpar</button>
      </div>
    </div>
    <div class="note" id="imgInfo" style="margin-bottom: 8px">Sem imagem.</div>
    <div class="imgbox"><canvas id="img" hidden></canvas></div>
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
      <h2>Descodificador FAX</h2>
      <div class="row">
        <label for="faxLpm" style="min-width: 90px">Velocidade</label>
        <select id="faxLpm">
          <option value="60">60 linhas/min</option>
          <option value="90">90 linhas/min</option>
          <option value="120">120 linhas/min</option>
          <option value="240">240 linhas/min</option>
        </select>
      </div>
      <div class="row">
        <label for="faxIoc" style="min-width: 90px">IOC</label>
        <select id="faxIoc">
          <option value="576">576</option>
          <option value="288">288</option>
        </select>
      </div>
      <div class="row">
        <label><input type="checkbox" id="faxAuto"> Começar sozinho com o tom de início</label>
      </div>
      <div class="note" style="margin-top: 8px">Sintoniza em USB com a portadora 1,9 kHz abaixo da frequência publicada (1500 Hz = preto, 2300 Hz = branco). A meio de uma emissão, usa «Iniciar FAX agora».</div>
      <div class="note" style="margin-top: 8px">SSTV (Martin, Scottie, Robot, PD) começa sozinho com o cabeçalho VIS; não precisa de configuração.</div>
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

  const MAX_LINES = 30;    // older lines of decoded text are dropped
  function appendText(id, add) {
    if (!add) return;
    const el = $(id);
    const atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
    const lines = (el.textContent + add).slice(-6000).split('\n');
    el.textContent = lines.slice(-MAX_LINES).join('\n');
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
    if (document.activeElement !== $('faxLpm')) $('faxLpm').value = String(d.fax_lpm);
    if (document.activeElement !== $('faxIoc')) $('faxIoc').value = String(d.fax_ioc);
    $('faxAuto').checked = d.fax_auto;
    showFtx(d);
    faxState = d.fax_state;
    sstv = d.sstv_rx ? `a receber ${d.sstv_mode}` : 'à espera do cabeçalho VIS';
    showImgInfo();

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

  // Image (FAX): lines fetched from /api/img while the tab is visible, and
  // accumulated in the canvas (one canvas row per line; the CSS aspect ratio
  // gives the lines their real height).
  const imgCv = $('img'), imgCtx = imgCv.getContext('2d');
  const img = { id: 0, next: 0, base: 0, w: 0, ch: 1, aspect: 1, total: 0, active: false, title: '' };
  let faxState = 0, sstv = '';

  function showImgInfo() {
    const fax = ['à espera do tom de início', 'a alinhar (linhas de fase)', 'a receber'][faxState] || '';
    const cur = img.id ? `${img.title} · ${img.total} linhas${img.active ? '' : ' · terminada'}` : 'Sem imagem';
    $('imgInfo').textContent = `${cur} · FAX: ${fax} · SSTV: ${sstv}`;
  }

  function imgResize(h) {
    if (imgCv.height === h && imgCv.width === img.w) return;
    const old = document.createElement('canvas');
    old.width = imgCv.width; old.height = imgCv.height;
    if (imgCv.width === img.w && imgCv.height) old.getContext('2d').drawImage(imgCv, 0, 0);
    imgCv.width = img.w; imgCv.height = h;
    if (old.width === img.w && old.height) imgCtx.drawImage(old, 0, 0);
    imgCv.style.aspectRatio = `${img.w} / ${Math.max(1, h * img.aspect)}`;
    imgCv.hidden = !h;
  }

  // Returns true when the board holds more lines than this reply carried.
  function imgData(buf) {
    if (buf.byteLength < 52) return false;
    const dv = new DataView(buf);
    const id = dv.getUint32(0, true), first = dv.getUint32(4, true);
    img.total = dv.getUint32(8, true);
    img.active = !!dv.getUint8(15);
    if (!id) { showImgInfo(); return false; }
    if (id !== img.id) {
      img.id = id; img.base = first; img.next = first;
      img.w = dv.getUint16(12, true); img.ch = dv.getUint8(14); img.aspect = dv.getFloat32(16, true);
      img.title = new TextDecoder().decode(new Uint8Array(buf, 20, 32)).replace(/\0[\s\S]*$/, '');
      imgResize(0);
    }
    const bytes = img.w * img.ch, n = Math.floor((buf.byteLength - 52) / bytes);
    if (n > 0) {
      const row0 = first - img.base;
      const px = new Uint8Array(buf, 52, n * bytes), out = imgCtx.createImageData(img.w, n);
      imgResize(Math.max(imgCv.height, row0 + n));
      for (let i = 0, o = 0; i < n * img.w; i++, o += 4) {
        if (img.ch === 3) { out.data[o] = px[i * 3]; out.data[o + 1] = px[i * 3 + 1]; out.data[o + 2] = px[i * 3 + 2]; }
        else out.data[o] = out.data[o + 1] = out.data[o + 2] = px[i];
        out.data[o + 3] = 255;
      }
      imgCtx.putImageData(out, 0, row0);
      img.next = first + n;
    }
    showImgInfo();
    return img.next < img.total;
  }

  async function pollImg() {
    let again = 1000;
    if (!document.hidden) {
      const ctl = new AbortController();
      const guard = setTimeout(() => ctl.abort(), 8000);
      try {
        const res = await fetch(`/api/img?i=${img.id}&l=${img.next}`, { cache: 'no-store', signal: ctl.signal });
        if (imgData(await res.arrayBuffer())) again = 50;
      } catch (e) { again = 3000; }
      clearTimeout(guard);
    }
    setTimeout(pollImg, again);
  }

  // FT8/FT4: messages fetched from /api/ftx, newest on top.
  const FTX_ROWS = 200;
  let ftxSeq = 0, ftxMode = 0, lastTimeSend = 0;

  function showFtx(d) {
    ftxMode = d.ftx_mode;
    if (document.activeElement !== $('ftxMode')) $('ftxMode').value = String(d.ftx_mode);
    let info;
    if (!d.ftx_mode) info = 'Desligado.';
    else if (!d.ftx_time) {
      info = 'À espera da hora UTC (NTP)…';
      // Without internet (access point) the board takes this browser's clock.
      if (Date.now() - lastTimeSend > 30000) { lastTimeSend = Date.now(); postConfig({ utc_ms: Date.now() }); }
    } else {
      const skew = d.utc - Date.now() / 1000;
      info = d.ftx_n < 0 ? 'A recolher o primeiro período…'
        : `Último período: ${d.ftx_n} mensagens, descodificadas em ${d.ftx_ms} ms`;
      if (d.ftx_lost) info += ` · ${d.ftx_lost} períodos perdidos`;
      if (Math.abs(skew) > 2) info += ` · relógio da placa ${skew > 0 ? '+' : ''}${skew.toFixed(0)} s em relação a este computador`;
    }
    $('ftxInfo').textContent = info;
  }

  function ftxRow(m) {
    const [slot, snr, dt, hz, text] = m;
    const tr = document.createElement('tr');
    const t = new Date(slot * 1000).toISOString().slice(11, 19);
    tr.className = 'new' + (/^CQ\b/.test(text) ? ' cq' : '');
    for (const [v, cls] of [[t, ''], [snr, 'n'], [dt.toFixed(1), 'n'], [hz, 'n'], [text, 'msg']]) {
      const td = document.createElement('td'); td.textContent = v; if (cls) td.className = cls; tr.appendChild(td);
    }
    return tr;
  }

  async function pollFtx() {
    let again = 2000;
    if (!document.hidden && ftxMode) {
      const ctl = new AbortController();
      const guard = setTimeout(() => ctl.abort(), 5000);
      try {
        const res = await fetch(`/api/ftx?s=${ftxSeq}`, { cache: 'no-store', signal: ctl.signal });
        const d = await res.json();
        const body = $('ftxRows');
        for (const m of d.m) body.insertBefore(ftxRow(m), body.firstChild);
        while (body.children.length > FTX_ROWS) body.lastChild.remove();
        ftxSeq = d.s;
      } catch (e) { again = 5000; }
      clearTimeout(guard);
    }
    setTimeout(pollFtx, again);
  }

  $('ftxMode').onchange = () => { ftxMode = +$('ftxMode').value; postConfig({ ftx_mode: $('ftxMode').value }); };
  $('ftxClear').onclick = () => { $('ftxRows').innerHTML = ''; };

  $('faxStart').onclick = () => postConfig({ fax_start: 1 });
  $('imgStop').onclick = () => postConfig({ img_stop: 1 });
  $('imgClear').onclick = () => { img.base = img.next; imgResize(0); };
  $('imgSave').onclick = () => {
    if (!imgCv.height) return;
    const c = document.createElement('canvas');    // lines at their real height
    c.width = img.w; c.height = Math.max(1, Math.round(imgCv.height * img.aspect));
    c.getContext('2d').drawImage(imgCv, 0, 0, c.width, c.height);
    const a = document.createElement('a');
    a.download = `${img.title.replace(/[^\w-]+/g, '_')}_${new Date().toISOString().slice(0, 16).replace(/[:T]/g, '')}.png`;
    a.href = c.toDataURL('image/png'); a.click();
  };
  $('faxLpm').onchange = () => postConfig({ fax_lpm: $('faxLpm').value });
  $('faxIoc').onchange = () => postConfig({ fax_ioc: $('faxIoc').value });
  $('faxAuto').onchange = () => postConfig({ fax_auto: $('faxAuto').checked ? 1 : 0 });

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
  pollImg();
  pollFtx();
})();
</script>
</body>
</html>
)HTML";
