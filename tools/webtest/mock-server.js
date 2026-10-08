// Mock of the display's settings server for browser tests: serves the real main/web/index.html and answers
// /api/* like the firmware (web.c), with the state in memory. POST /__reset restores the starting state;
// GET /__state returns it (tests check what the page sent); POST /__update {state, latest, notes, progress, error,
// rolled_back, channel, check_result} sets the updater's state (ota.c) for the firmware card: every state the page can
// show must be scriptable here (a page bug hid the Install button in two releases, and nothing tested "available").
// POST /__presence {...} sets screen dimming's state (espforge's forge_presence): {state, mic_ok, imu_ok, level_db,
// calibrating, cal, ...}; {calibrating: false, cal: 'noisy'} ends a calibration as a noisy room would.
//   node mock-server.js [port]     (default 8099)
const http = require('http');
const fs = require('fs');
const path = require('path');

const PAGE = path.join(__dirname, '..', '..', 'main', 'web', 'index.html');
const port = Number(process.argv[2] || process.env.PORT || 8099);
const MAX_PLACES = 4;
const STATES = ['idle', 'checking', 'up_to_date', 'available', 'downloading', 'done', 'failed'];   // web.c ota_state_name

function fresh() {
  return {
    places: [{ name: 'Québec', lat: 46.8139, lon: -71.208 }],
    active: 0,
    units: { temp: 'c', wind: 'kmh', clock: 24, lang: 'en' },
    ssid: 'HomeNet',
    version: 'v1.5.0-test',
    update: { current: 'v1.5.0-test', latest: '', channel: 'stable', state: 'up_to_date', progress: 0, error: '',
              notes: '', pending_verify: false, uptime_s: 300, rolled_back: '' },
    checkResult: 'up_to_date',                // what a check ends in (POST /__update {check_result})
    presence: { ok: true, enabled: true, state: 'active', mic_ok: true, calibrating: false, calib_left_s: 0,
                cal: 'none', cal_spread_db: 0, brightness: 100,
                level_db: -48, threshold_db: -55, baseline_db: -60, margin_db: 5, wake_progress: 0, wake_s: 3,
                quiet_s: 12, dim_s: 600, off_s: 3000, bright_pct: 100, dim_pct: 20,
                imu_ok: true, motion_g: 0.01, motion_wake: true, motion_thr: 0.1 },
    sound: { level: 2, volume: 60, quiet_from: '22:00', quiet_to: '07:00', ok: true, tests: 0 },
    wifi: null,
    key: null,                                // the display's key (web.c): POSTs need X-Key when set
    setupNet: false,                          // the page is on the setup network (web.c from_setup_ap)
    log: [],                                  // every API call: {method, url, body}
  };
}
let st = fresh();

function json(res, code, obj) {
  res.writeHead(code, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(obj));
}

function body(req) {
  return new Promise(resolve => {
    let b = '';
    req.on('data', c => (b += c));
    req.on('end', () => { try { resolve(b ? JSON.parse(b) : {}); } catch (e) { resolve(null); } });
  });
}

const cur = () => st.places[st.active];
const config = () => ({
  name: cur().name, lat: cur().lat, lon: cur().lon, ssid: st.ssid, version: st.version,
  places: st.places, active: st.active, max_places: MAX_PLACES, units: st.units,
  languages: [{ code: 'en', name: 'English' }, { code: 'fr', name: 'Français' }, { code: 'iu', name: 'ᐃᓄᒃᑎᑐᑦ (draft)' }],
});

const routes = {
  'GET /api/config': () => {                       // web.c config_get: no coordinates on the setup network
    const c = config();
    if (!st.setupNet) return [200, c];
    return [200, { ...c, lat: undefined, lon: undefined, ssid: '', setup: true, places: c.places.map(p => ({ name: p.name })) }];
  },
  'POST /api/location': b => {                     // web.c location_post
    if (!b || typeof b.lat !== 'number' || typeof b.lon !== 'number') return [400, 'bad location'];
    const i = typeof b.index === 'number' ? b.index : st.active;
    if (b.lat < -85 || b.lat > 85 || b.lon < -180 || b.lon > 180) return [400, 'bad location'];
    if (i < 0 || i > st.places.length || i >= MAX_PLACES) return [400, 'bad location'];
    st.places[i] = { name: (b.name || '').trim() || 'My location', lat: b.lat, lon: b.lon };
    return [200, { ok: true }];
  },
  'POST /api/places': b => {                       // web.c places_post
    if (b && typeof b.select === 'number') {
      if (b.select < 0 || b.select >= st.places.length) return [400, 'bad place'];
      st.active = b.select;
    } else if (b && typeof b.delete === 'number') {
      const i = b.delete;
      if (i < 0 || i >= st.places.length || st.places.length === 1) return [400, 'bad place'];
      st.places.splice(i, 1);
      if (st.active > i || st.active >= st.places.length) st.active = Math.max(0, st.active - 1);
    } else return [400, 'bad place'];
    return [200, { ok: true }];
  },
  'POST /api/units': b => { if (!b) return [400, 'bad json']; Object.assign(st.units, b); return [200, { ok: true }]; },
  'GET /api/update': () => {                       // web.c update_get: notes only while an update is offered
    const u = st.update;
    if (u.state === 'checking' && st.checkResult !== 'checking') u.state = st.checkResult;   // a check: one poll
    else if (u.state === 'downloading' && st.installing) {   // an install moves on at each poll: 50 %, 100 %, done
      u.progress = Math.min(100, u.progress + 50);
      if (u.progress === 100) u.state = 'done';
    }
    const out = { ...u };
    if (u.state !== 'available') delete out.notes;
    if (!u.rolled_back) delete out.rolled_back;      // web.c adds it only when there is one
    return [200, out];
  },
  'POST /api/update': b => {                       // web.c update_post: channel and/or action
    if (!b) return [400, 'bad json'];
    if (b.channel) st.update.channel = b.channel;
    if (b.action === 'check') st.update.state = 'checking';
    if (b.action === 'install') {
      if (st.update.state !== 'available') return [409, { error: 'nothing to install' }];
      st.update.state = 'downloading'; st.update.progress = 0; st.installing = true;
    }
    const out = { ...st.update };                    // the answer to a POST: the state as it is (no poll step)
    if (out.state !== 'available') delete out.notes;
    if (!out.rolled_back) delete out.rolled_back;
    return [200, out];
  },
  // forge_presence's presence_web.c: GET's answer is the settings, then the live state, with "ok", "cal" (the last
  // calibration's verdict) and "cal_spread_db"
  'GET /api/presence': () => {
    const p = st.presence;
    if (p.calibrating && --p.calib_left_s <= 0) {            // 1 s a poll; a quiet room: the new baseline
      Object.assign(p, { calibrating: false, calib_left_s: 0, cal: 'ok', cal_spread_db: 2, baseline_db: -66 });
      p.threshold_db = p.baseline_db + p.margin_db;
    }
    return [200, { ...p, ok: true }];
  },
  'POST /api/presence': b => {                    // as presence_set_config(): clamped, baseline kept
    if (!b) return [400, 'bad json'];
    const p = st.presence;
    const num = (k, lo, hi) => { if (typeof b[k] === 'number') p[k] = Math.min(hi, Math.max(lo, b[k])); };
    if (typeof b.enabled === 'boolean') p.enabled = b.enabled;
    if (typeof b.motion_wake === 'boolean') p.motion_wake = b.motion_wake;
    num('margin_db', 1, 60); num('wake_s', 0.2, 60); num('dim_s', 1, 86400); num('off_s', 1, 86400);
    num('bright_pct', 5, 100); num('dim_pct', 1, 100); num('motion_thr', 0.02, 0.5);
    p.threshold_db = p.baseline_db + p.margin_db;
    p.state = 'active'; p.brightness = p.bright_pct;
    return [200, { ...p, ok: !st.presenceNotSaved }];    // "ok":false: applied, not saved (NVS refused)
  },
  'POST /api/calibrate': b => {                   // a refusal is an answer: 200 {ok: false, why}
    const p = st.presence;
    if (!p.mic_ok) return [200, { ok: false, why: 'no_mic' }];
    if (p.calibrating) return [200, { ok: false, why: 'busy' }];
    p.calibrating = true;
    p.calib_left_s = (b && b.seconds) || 5;
    return [200, { ...p, ok: true }];
  },
  'GET /api/scan': () => [200, [{ ssid: 'HomeNet', rssi: -50, secure: true }, { ssid: 'Cafe', rssi: -75, secure: false }]],
  'POST /api/wifi': b => { st.wifi = b; return [200, { ok: true }]; },
  'GET /api/sound': () => [200, st.sound],
  'POST /api/sound': b => {
    if (!b) return [400, 'bad json'];
    if (b.test) st.sound.tests++;
    else for (const k of ['level', 'volume', 'quiet_from', 'quiet_to']) if (k in b) st.sound[k] = b[k];
    return [200, st.sound];
  },
};

http.createServer(async (req, res) => {
  const url = req.url.split('?')[0];
  if (req.method === 'POST' && url === '/__reset') { st = fresh(); return json(res, 200, { ok: true }); }
  if (req.method === 'GET' && url === '/__state') return json(res, 200, st);
  if (req.method === 'POST' && url === '/__setup') { st.setupNet = true; return json(res, 200, { ok: true }); }
  if (req.method === 'POST' && url === '/__key') { st.key = (await body(req) || {}).key || null; return json(res, 200, { ok: true }); }
  if (req.method === 'POST' && url === '/__presence') {
    const b = await body(req) || {};
    if ('not_saved' in b) { st.presenceNotSaved = !!b.not_saved; delete b.not_saved; }   // the next saves fail
    Object.assign(st.presence, b);
    return json(res, 200, st.presence);
  }
  if (req.method === 'POST' && url === '/__update') {
    const b = await body(req) || {};
    if (b.state && !STATES.includes(b.state)) return json(res, 400, { error: 'state', states: STATES });
    if ('check_result' in b) { st.checkResult = b.check_result; delete b.check_result; }
    Object.assign(st.update, b);
    return json(res, 200, st.update);
  }
  if (req.method === 'GET' && url === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
    return res.end(fs.readFileSync(PAGE));
  }
  const fn = routes[req.method + ' ' + url];
  if (!fn) { res.writeHead(404); return res.end('not found'); }
  if (req.method === 'POST' && st.key && !st.setupNet && req.headers['x-key'] !== st.key) {   // web.c guarded()
    st.log.push({ method: req.method, url, refused: 401 });
    return json(res, 401, { error: 'key' });
  }
  // web.c: a POST must say it is JSON (415 otherwise: a form posted from another site can't)
  if (req.method === 'POST' && !(req.headers['content-type'] || '').startsWith('application/json')) {
    st.log.push({ method: req.method, url, refused: 415 });
    return json(res, 415, { error: 'json' });
  }
  const b = req.method === 'POST' ? await body(req) : undefined;
  st.log.push({ method: req.method, url, body: b });
  const [code, out] = fn(b);
  if (typeof out === 'string') { res.writeHead(code); return res.end(out); }
  json(res, code, out);
}).listen(port, () => console.log(`mock display on http://localhost:${port}/`));
