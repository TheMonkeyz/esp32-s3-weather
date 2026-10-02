#!/usr/bin/env node
/*
 * The README's hero picture, docs/img/hero.png: three round screens from web/flash/img/ (hourly, weather, radar)
 * fanned out, each in a dark bezel with a soft shadow, on a transparent background so it suits GitHub's light
 * and dark themes. Refresh it after new screenshots (tools/round_shots.py):
 *
 *   cd tools/webtest && npm install && npx playwright install chromium   # once (see docs/TESTING.md §5)
 *   node tools/make_hero.js [out.png]          # default: docs/img/hero.png
 *
 * Playwright is taken from tools/webtest/node_modules, else from NODE_PATH.
 */
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const OUT = process.argv[2] || path.join(ROOT, 'docs', 'img', 'hero.png');
let playwright;
try { playwright = require(path.join(__dirname, 'webtest', 'node_modules', 'playwright')); }
catch { playwright = require('playwright'); }

const shot = name => 'data:image/png;base64,' +
  fs.readFileSync(path.join(ROOT, 'web', 'flash', 'img', `${name}.png`)).toString('base64');

// x, y = centre, d = screen diameter (466 = the panel's own pixels), a = tilt in degrees. Drawn in this order.
const DEVICES = [
  { name: 'hourly', x: 268, y: 326, d: 372, a: -9 },
  { name: 'radar', x: 1012, y: 326, d: 372, a: 9 },
  { name: 'weather', x: 640, y: 296, d: 466, a: 0 },
];
const W = 1280, H = 620;

const html = `<!doctype html><meta charset="utf-8"><style>
  html, body { margin: 0; background: transparent; }
  #stage { position: relative; width: ${W}px; height: ${H}px; }
  .dev { position: absolute; box-sizing: border-box; border-radius: 50%;
    background: linear-gradient(155deg, #5b616b 0%, #262a30 28%, #101215 62%, #2b2f35 100%);
    box-shadow: 0 30px 48px -14px rgba(0, 0, 0, .55), 0 8px 18px rgba(0, 0, 0, .28),
                inset 0 1px 1px rgba(255, 255, 255, .35), inset 0 -1px 2px rgba(0, 0, 0, .6); }
  .glass { position: absolute; inset: var(--b); border-radius: 50%; background: #000;
    box-shadow: 0 0 0 1px rgba(0, 0, 0, .9), inset 0 0 0 1px rgba(255, 255, 255, .06); }
  .glass img { position: absolute; inset: var(--g); width: calc(100% - 2 * var(--g)); border-radius: 50%; }
</style><div id="stage">${DEVICES.map(({ name, x, y, d, a }) => {
  const b = Math.round(d * 0.045), g = Math.round(d * 0.022), s = d + 2 * (b + g);
  return `<div class="dev" style="--b:${b}px; --g:${g}px; width:${s}px; height:${s}px; left:${x - s / 2}px;
    top:${y - s / 2}px; transform: rotate(${a}deg)"><div class="glass"><img src="${shot(name)}"></div></div>`;
}).join('')}</div>`;

(async () => {
  const browser = await playwright.chromium.launch();
  const page = await browser.newPage({ viewport: { width: W, height: H } });
  await page.setContent(html);
  await page.locator('#stage').screenshot({ path: OUT, omitBackground: true });
  await browser.close();
  console.log(`${OUT}: ${fs.statSync(OUT).size} bytes`);
})();
