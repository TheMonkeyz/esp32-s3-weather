// Shared test setup: fresh mock state, external services answered locally, page errors collected.
//   - Leaflet (unpkg.com) comes from node_modules/leaflet (same version and integrity hashes as the page uses)
//   - OSM map tiles are a plain grey tile; city search and reverse geocoding return fixed answers
const base = require('@playwright/test');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const LEAFLET_DIST = path.join(__dirname, '..', 'node_modules', 'leaflet', 'dist');

const GEOCODE = {                                  // geocoding-api.open-meteo.com/v1/search
  results: [
    { name: 'Montréal', latitude: 45.5019, longitude: -73.5674, admin1: 'Quebec', country: 'Canada' },
    { name: 'Montreal-Ouest', latitude: 45.4536, longitude: -73.6492, admin1: 'Quebec', country: 'Canada' },
  ],
};

function greyTile() {                              // 256x256 grey PNG
  const crc = buf => { let c, t = []; for (let n = 0; n < 256; n++) { c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
    let x = 0xffffffff; for (const b of buf) x = t[(x ^ b) & 0xff] ^ (x >>> 8); return (x ^ 0xffffffff) >>> 0; };
  const chunk = (type, data) => { const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([len, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(256, 0); ihdr.writeUInt32BE(256, 4); ihdr[8] = 8; ihdr[9] = 0;
  const raw = Buffer.alloc(257 * 256, 0x60); for (let y = 0; y < 256; y++) raw[y * 257] = 0;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
                        chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const TILE = greyTile();

// A screenshot for people to look at (shots/, docs/TESTING.md §5), never a check: Chromium sometimes answers
// "Unable to capture screenshot" (seen in CI). Try again once after a moment (a full page then only the viewport),
// and if that fails too, warn and go on: the test's result must not depend on it.
async function reviewShot(target, file, options = {}) {
  fs.mkdirSync(path.dirname(path.resolve(file)), { recursive: true });
  try {
    await target.screenshot({ ...options, path: file });
    return;
  } catch (e) {
    try {
      await new Promise(r => setTimeout(r, 500));
      await target.screenshot({ ...options, fullPage: undefined, path: file });
      return;
    } catch (e2) {
      const msg = `review screenshot ${path.basename(file)} not saved: ${e2.message.split('\n')[0]}`;
      console.warn('warning: ' + msg);
      try { base.test.info().annotations.push({ type: 'warning', description: msg }); } catch (_) { /* outside a test */ }
    }
  }
}
exports.reviewShot = reviewShot;

exports.test = base.test.extend({
  noInternet: [false, { option: true }],           // true: the phone has no internet (map, search unavailable)
  page: async ({ page, request, noInternet }, use, testInfo) => {
    await request.post('/__reset');
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    // (a 401 is the display refusing a change without its key: the page handles it, the browser still logs it)
    page.on('console', m => { if (m.type() === 'error' && !noInternet && !/status of 401/.test(m.text())) errors.push(m.text()); });
    if (noInternet) {
      await page.route(/^https:\/\//, r => r.abort('internetdisconnected'));
    } else {
      await page.route('https://unpkg.com/leaflet@1.9.4/dist/**', r => {
        const f = path.join(LEAFLET_DIST, new URL(r.request().url()).pathname.split('/dist/')[1]);
        const type = f.endsWith('.css') ? 'text/css' : f.endsWith('.js') ? 'application/javascript' : 'image/png';
        r.fulfill({ body: fs.readFileSync(f), contentType: type, headers: { 'Access-Control-Allow-Origin': '*' } });
      });
      await page.route('https://tile.openstreetmap.org/**', r => r.fulfill({ body: TILE, contentType: 'image/png' }));
      await page.route('https://geocoding-api.open-meteo.com/**', r => r.fulfill({ json: GEOCODE }));
      await page.route('https://nominatim.openstreetmap.org/**', r =>
        r.fulfill({ json: { address: { town: 'Lac-Beauport' } } }));
    }
    await use(page);
    await reviewShot(page, path.join(__dirname, '..', 'shots', testInfo.title.replace(/[^a-z0-9]+/gi, '_') + '.png'),
                     { fullPage: true });
    base.expect(errors, 'errors in the page').toEqual([]);
  },
});
exports.expect = base.expect;

// The mock's state (what the page sent to the "display")
exports.state = async request => (await request.get('/__state')).json();
// Seed places directly in the mock (like the display already having them)
exports.addPlace = (request, name, lat, lon, index) =>
  request.post('/api/location', { data: { name, lat, lon, index } });
