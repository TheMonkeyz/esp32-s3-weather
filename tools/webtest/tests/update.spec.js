// Firmware card: an offered update with release notes, Install, the download, a language switch meanwhile
const { test, expect, state, reviewShot } = require('./fixtures');

// ota.c's notes: "vX|date" header lines, one line per change, a blank line between releases
const NOTES = 'v1.12.0|October 3, 2026\nThe Install button shows again.\nRed warnings first.\n\n' +
              'v1.11.1|October 2, 2026\nRadar zoom follows the finger.\n';
const offer = request => request.post('/__update', { data: { state: 'available', latest: 'v1.12.0', notes: NOTES } });

test('an offered update shows its notes and the Install button', async ({ page, request }) => {
  await offer(request);
  await page.goto('/');
  await expect(page.locator('#fwState')).toHaveText('Version v1.12.0 is available.');
  await expect(page.locator('#fwInstall')).toBeVisible();
  await expect(page.locator('#fwInstall')).toHaveText('Install v1.12.0');
  const notes = page.locator('#fwNotes');
  await expect(notes).toBeVisible();
  await expect(notes.locator('div').first()).toHaveText("What's new");
  await expect(notes).toContainText('v1.12.0 · October 3, 2026');
  await expect(notes).toContainText('v1.11.1 · October 2, 2026');
  await expect(notes.locator('li')).toHaveCount(3);
  await expect(page.locator('#msg')).not.toHaveClass('err');           // window.onerror writes page errors there
  await reviewShot(page.locator('#fwCard'), 'shots/review_update_card.png');
});

test('Install asks first, then the display downloads and restarts', async ({ page, request }) => {
  await offer(request);
  await page.goto('/');
  let asked = '';
  page.on('dialog', d => { asked = d.message(); d.accept(); });
  await page.locator('#fwInstall').click();
  await expect(page.locator('#fwState')).toHaveText('Installed. The display is restarting…');
  expect(asked).toContain('Install v1.12.0?');
  await expect(page.locator('#fwInstall')).toBeHidden();
  await expect(page.locator('#fwNotes')).toBeHidden();
  const posts = (await state(request)).log.filter(e => e.method === 'POST' && e.url === '/api/update');
  expect(posts.map(e => e.body)).toEqual([{ action: 'install' }]);
});

test('Install declined: nothing is sent', async ({ page, request }) => {
  await offer(request);
  await page.goto('/');
  page.on('dialog', d => d.dismiss());
  await page.locator('#fwInstall').click();
  await expect(page.locator('#fwInstall')).toBeVisible();
  const posts = (await state(request)).log.filter(e => e.method === 'POST' && e.url === '/api/update');
  expect(posts).toEqual([]);
});

test('switching language while an update is offered', async ({ page, request }) => {
  await offer(request);
  await page.goto('/');
  await expect(page.locator('#fwInstall')).toBeVisible();
  await page.locator('#uLang').selectOption('fr');
  await expect(page.locator('#fwState')).toHaveText('La version v1.12.0 est disponible.');
  await expect(page.locator('#fwInstall')).toHaveText('Installer v1.12.0');
  await expect(page.locator('#fwNotes div').first()).toHaveText('Nouveautés');
  await expect(page.locator('#msg')).not.toHaveClass('err');
  await reviewShot(page.locator('#fwCard'), 'shots/review_update_card_fr.png');
});

// ---- every state the updater can report (web.c ota_state_name), not only "available" ----
// Each one is scriptable in the mock (POST /__update); the page must show its own text, never "undefined", and the
// buttons that go with it. espforge's update.spec.js does the same for its page (docs/LESSONS.md there, L24).
const STATES = {
  idle: [{}, 'Checks for updates a minute after start-up, then every 6 hours.'],
  checking: [{ check_result: 'checking' }, 'Checking…'],             // stays "checking" for the test
  up_to_date: [{ latest: 'v1.12.0' }, 'Up to date (latest: v1.12.0).'],
  available: [{ latest: 'v1.13.0', notes: NOTES }, 'Version v1.13.0 is available.'],
  downloading: [{ latest: 'v1.13.0', progress: 42 }, 'Downloading v1.13.0… 42 %. Keep the display plugged in.'],
  done: [{ latest: 'v1.13.0', progress: 100 }, 'Installed. The display is restarting…'],
  failed: [{ error: 'TLS handshake failed' }, 'Update check failed: TLS handshake failed.'],
};
const script = (request, name, extra = {}) =>
  request.post('/__update', { data: { state: name, ...STATES[name][0], ...extra } });

for (const [name, [, text]] of Object.entries(STATES)) {
  test(`state ${name}: its text and buttons`, async ({ page, request }) => {
    await script(request, name);
    await page.goto('/');
    await expect(page.locator('#fwState')).toHaveText(text);
    const busy = ['checking', 'downloading', 'done'].includes(name);
    await expect(page.locator('#fwInstall')).toBeVisible({ visible: name === 'available' });
    await expect(page.locator('#fwMeter')).toBeVisible({ visible: name === 'downloading' || name === 'done' });
    if (busy) await expect(page.locator('#fwCheck')).toBeDisabled(); else await expect(page.locator('#fwCheck')).toBeEnabled();
    await expect(page.locator('#fwNotes')).toBeVisible({ visible: name === 'available' });
    await expect(page.locator('#msg')).not.toHaveClass('err');
  });
}

test('in French every state has its own French text', async ({ page, request }) => {
  await page.goto('/');
  await page.locator('#uLang').selectOption('fr');
  await expect.poll(async () => (await state(request)).units.lang).toBe('fr');
  const seen = {};
  for (const [name, [, english]] of Object.entries(STATES)) {
    await script(request, name);
    await page.reload();
    const s = page.locator('#fwState');
    await expect(s).not.toHaveText(english);
    const fr = (await s.textContent()).trim();
    expect(fr, name).not.toMatch(/undefined|\[object|NaN|^$/);
    seen[name] = fr;
  }
  expect(new Set(Object.values(seen)).size, JSON.stringify(seen)).toBe(Object.keys(seen).length);
  expect(await page.evaluate(() => typeof t)).toBe('function');
});

test('a rollback is reported, but not over a download in progress', async ({ page, request }) => {
  await script(request, 'idle', { rolled_back: 'v1.13.0-rc.1' });
  await page.goto('/');
  await expect(page.locator('#fwState'))
    .toHaveText('v1.13.0-rc.1 was undone: the display restarted before it was confirmed. ' + STATES.idle[1]);
  await script(request, 'downloading', { rolled_back: 'v1.13.0-rc.1' });
  await page.reload();
  await expect(page.locator('#fwState')).toHaveText(STATES.downloading[1]);
});

test('an install that failed leaves the update offered, with the reason', async ({ page, request }) => {
  await script(request, 'available', { error: 'Download interrupted' });
  await page.goto('/');
  await expect(page.locator('#fwState')).toHaveText('Version v1.13.0 is available. Download interrupted.');
  await expect(page.locator('#fwInstall')).toBeVisible();
});

test('Check asks the display, shows Checking…, then its answer', async ({ page, request }) => {
  await page.goto('/');
  await request.post('/__update', { data: { check_result: 'available', latest: 'v1.13.0', notes: NOTES } });
  await page.locator('#fwCheck').click();
  await expect(page.locator('#fwState')).toHaveText('Checking…');
  await expect(page.locator('#fwCheck')).toBeDisabled();
  // the page polls every 1.5 s while busy; the mock's check takes one poll
  await expect(page.locator('#fwState')).toHaveText(STATES.available[1], { timeout: 6000 });
  await expect(page.locator('#fwInstall')).toBeVisible();
  await expect(page.locator('#fwCheck')).toBeEnabled();
  const sent = (await state(request)).log.filter(e => e.method === 'POST' && e.url === '/api/update').map(e => e.body);
  expect(sent).toEqual([{ action: 'check' }]);
});

test('the channel picker posts the channel and explains Beta', async ({ page, request }) => {
  await page.goto('/');
  await expect(page.locator('#fwBeta')).toBeHidden();
  await page.locator('#fwChan').selectOption('beta');
  await expect.poll(async () => (await state(request)).update.channel).toBe('beta');
  await expect(page.locator('#fwBeta')).toBeVisible();
  const sent = (await state(request)).log.filter(e => e.method === 'POST' && e.url === '/api/update').map(e => e.body);
  expect(sent).toEqual([{ channel: 'beta' }]);
});

test('every change the page posts is JSON (the display refuses anything else: 415)', async ({ page, request }) => {
  await page.goto('/');
  await page.locator('#fwChan').selectOption('beta');
  await page.locator('#fwCheck').click();
  await expect.poll(async () => (await state(request)).log.filter(e => e.url === '/api/update').length).toBeGreaterThan(1);
  expect((await state(request)).log.filter(e => e.refused === 415)).toEqual([]);
});
