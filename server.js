// SamudraMesh dashboard server & telemetry hub. Pure Node 18+ (no npm packages needed).
// Run:  node server.js   then open http://localhost:3000
const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');

const PORT = process.env.PORT || 3000;
const PUB = path.join(__dirname, 'public');
const HISTORY_FILE = path.join(__dirname, 'telemetry_history.json');
const MIME = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.svg': 'image/svg+xml',
  '.json': 'application/json',
  '.csv': 'text/csv'
};

let latest = null;
let lastNode = 0;
let cmd = { gate: 'auto', conveyor: 'auto' };   // manual overrides from the dashboard
const clients = new Set();
const historyBuffer = [];
const MAX_HISTORY = 600; // Keep last 10 minutes at 1 Hz in memory

// Load initial history if file exists
try {
  if (fs.existsSync(HISTORY_FILE)) {
    const raw = fs.readFileSync(HISTORY_FILE, 'utf8');
    const parsed = JSON.parse(raw);
    if (Array.isArray(parsed)) {
      historyBuffer.push(...parsed.slice(-MAX_HISTORY));
    }
  }
} catch (e) {
  console.log('Starting fresh telemetry history buffer');
}

const r2 = x => Math.round(x * 100) / 100;

// Save history periodically
let saveCooldown = 0;
function recordTelemetry(data) {
  latest = data;
  historyBuffer.push(data);
  if (historyBuffer.length > MAX_HISTORY) {
    historyBuffer.shift();
  }

  saveCooldown++;
  if (saveCooldown >= 30) { // Persist to disk every 30 seconds
    saveCooldown = 0;
    try {
      fs.writeFileSync(HISTORY_FILE, JSON.stringify(historyBuffer.slice(-200), null, 2));
    } catch (e) {}
  }

  const msg = `data: ${JSON.stringify(data)}\n\n`;
  clients.forEach(r => r.write(msg));
}

const readBody = req => new Promise(res => {
  let b = '';
  req.on('data', c => (b += c));
  req.on('end', () => {
    try { res(JSON.parse(b || '{}')); } catch { res({}); }
  });
});

// ---------- Simulator (runs when no physical ESP32 has posted in the last 5 s) ----------
const sim = { t: 0, bin: 15, kg: 0, conv: false, bypass: false, g: 0 };
function tick() {
  if (Date.now() - lastNode < 5000) return; // Real hardware node is active

  const s = sim;
  s.t += 1;
  const tide = Math.sin(2 * Math.PI * s.t / 120); // 120-sec tide cycle
  const ph = s.t % 90;
  const rain = ph > 60 ? Math.sin(Math.PI * (ph - 60) / 30) : 0; // Monsoon surge every 90 sec
  const down = 1.4 + 0.6 * tide + (Math.random() - .5) * 0.02;
  const up = Math.max(down + 0.05, 1.2 + 1.1 * rain) + 0.30 * (s.bin / 100) * (s.conv ? 0 : 1) - 0.4 * (s.g / 100);
  const dH = up - down;
  const flow = 0.6 * Math.sign(dH) * Math.sqrt(Math.abs(dH)) * (1 + rain) - 0.8 * Math.cos(2 * Math.PI * s.t / 120);

  // Plastic accumulation & extraction
  s.bin = Math.min(100, s.bin + 3 * Math.abs(flow) * (0.4 + rain));
  if (cmd.conveyor === 'on') s.conv = true;
  else if (cmd.conveyor === 'off') s.conv = false;
  else {
    if (s.bin >= 70) s.conv = true;
    if (s.bin <= 15) s.conv = false;
  }
  if (s.conv) {
    const rem = Math.min(s.bin, 2.5);
    s.bin -= rem;
    s.kg += rem * 0.2;
  }

  // Failsafe Bypass Gate logic
  const trigger = (up > 2.0 && dH > 0.15) || dH > 0.45;
  if (trigger) s.bypass = true;
  else if (up < 1.8 && dH < 0.2) s.bypass = false;

  if (cmd.gate === 'open') s.bypass = true;
  else if (cmd.gate === 'close') s.bypass = false;

  s.g = Math.max(0, Math.min(100, s.g + (s.bypass ? 20 : -20)));
  const mode = s.bypass ? 'BYPASS' : flow > 0.05 ? 'EBB_CAPTURE' : flow < -0.05 ? 'FLOOD_CAPTURE' : 'SLACK';

  recordTelemetry({
    node: 'SM-SIM',
    ts: Date.now(),
    src: 'sim',
    mode,
    kg: r2(s.kg),
    cmd,
    s: {
      levelUp: r2(up),
      levelDown: r2(down),
      dH: r2(dH),
      flow: r2(flow),
      binFill: r2(s.bin),
      tilt: r2(2 + 7 * Math.abs(flow) + Math.random() * .6),
      battV: r2(12.9 - (s.conv ? .15 : 0) + .1 * Math.sin(s.t / 20))
    },
    a: {
      convRpm: s.conv ? 58 + Math.round(Math.random() * 3) : 0,
      gate1: s.g,
      gate2: s.g
    }
  });
}
setInterval(tick, 1000);

// ---------- CSV Report Generator for BMC Shift Audits ----------
function generateCsvReport() {
  const headers = [
    'Timestamp_Unix',
    'DateTime_ISO',
    'Source',
    'Node_ID',
    'Operational_Mode',
    'Level_Upstream_m',
    'Level_Downstream_m',
    'Head_Difference_dH_m',
    'Flow_Velocity_ms',
    'Collector_Fill_pct',
    'Boom_Tilt_deg',
    'Battery_Voltage_V',
    'Conveyor_Motor_RPM',
    'Bypass_Gate1_pct',
    'Bypass_Gate2_pct',
    'Plastic_Extracted_kg'
  ];

  const rows = historyBuffer.map(d => {
    const s = d.s || {};
    const a = d.a || {};
    return [
      d.ts,
      new Date(d.ts).toISOString(),
      d.src || 'unknown',
      d.node || 'SM-01',
      d.mode || 'UNKNOWN',
      s.levelUp !== undefined ? s.levelUp : '',
      s.levelDown !== undefined ? s.levelDown : '',
      s.dH !== undefined ? s.dH : '',
      s.flow !== undefined ? s.flow : '',
      s.binFill !== undefined ? s.binFill : '',
      s.tilt !== undefined ? s.tilt : '',
      s.battV !== undefined ? s.battV : '',
      a.convRpm !== undefined ? a.convRpm : '',
      a.gate1 !== undefined ? a.gate1 : '',
      a.gate2 !== undefined ? a.gate2 : '',
      d.kg !== undefined ? d.kg : ''
    ].join(',');
  });

  return [headers.join(','), ...rows].join('\n');
}

// ---------- HTTP Server & API Endpoints ----------
http.createServer(async (req, res) => {
  const url = req.url.split('?')[0];

  // 1. ESP32 Physical Telemetry Post
  if (req.method === 'POST' && url === '/api/telemetry') {
    const b = await readBody(req);
    lastNode = Date.now();
    recordTelemetry({ ...b, src: 'esp32', ts: lastNode, cmd });
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end(JSON.stringify(cmd));
  }

  // 2. Dashboard Actuator Commands
  if (req.method === 'POST' && url === '/api/command') {
    const b = await readBody(req);
    if (['auto', 'open', 'close'].includes(b.gate)) cmd.gate = b.gate;
    if (['auto', 'on', 'off'].includes(b.conveyor)) cmd.conveyor = b.conveyor;
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end(JSON.stringify(cmd));
  }

  // 3. Historical Telemetry Data
  if (req.method === 'GET' && url === '/api/history') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end(JSON.stringify(historyBuffer));
  }

  // 4. Municipal CSV Export Endpoint
  if (req.method === 'GET' && url === '/api/export') {
    const csvData = generateCsvReport();
    res.writeHead(200, {
      'Content-Type': 'text/csv',
      'Content-Disposition': `attachment; filename="BMC_Samudramesh_Shift_Report_${Date.now()}.csv"`
    });
    return res.end(csvData);
  }

  // 5. Live Server-Sent Events (SSE) Stream
  if (url === '/events') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      'Connection': 'keep-alive'
    });
    if (latest) res.write(`data: ${JSON.stringify(latest)}\n\n`);
    clients.add(res);
    req.on('close', () => clients.delete(res));
    return;
  }

  // 6. Static File Serving (supports Samudramesh.html and index.html)
  let defaultFile = fs.existsSync(path.join(__dirname, 'Samudramesh.html')) ? 'Samudramesh.html' : 'index.html';
  let file = path.join(__dirname, url === '/' ? defaultFile : url);
  if (!fs.existsSync(file)) {
    file = path.join(PUB, url === '/' ? 'index.html' : url);
  }
  if (!fs.existsSync(file)) {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    return res.end('404 Not Found');
  }

  res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
}).listen(PORT, () => {
  console.log(`====================================================`);
  console.log(`SamudraMesh Telemetry Server: http://localhost:${PORT}`);
  console.log(`Live Dashboard:             http://localhost:${PORT}`);
  console.log(`Export BMC Report:          http://localhost:${PORT}/api/export`);
  console.log(`Historical JSON:            http://localhost:${PORT}/api/history`);
  console.log(`----------------------------------------------------`);
  Object.values(os.networkInterfaces()).flat().filter(i => i.family === 'IPv4' && !i.internal)
    .forEach(i => console.log(`ESP32 Telemetry Endpoint:  http://${i.address}:${PORT}/api/telemetry`));
  console.log(`====================================================`);
});
