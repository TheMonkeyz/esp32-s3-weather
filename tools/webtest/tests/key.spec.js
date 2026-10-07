// The display's key (web.c, "Who may change things"): from the settings QR code (#k=...), kept by the phone
const { test, expect, state, reviewShot } = require('./fixtures');

const KEY = '0123456789abcdef';

test('the key from the QR code is kept and sent with changes', async ({ page, request }) => {
  await request.post('/__key', { data: { key: KEY } });
  await page.goto('/#k=' + KEY);
  await expect(page.locator('#places')).toContainText('Québec');
  await expect(page.locator('#setupNote')).toBeHidden();             // only on the setup network
  expect(new URL(page.url()).hash).toBe('');                      // not left in the address bar
  await page.locator('#uTemp').selectOption('f');
  await expect(page.locator('#umsg')).toHaveText('Saved');
  expect((await state(request)).units.temp).toBe('f');
  await page.goto('/');                                           // later, without the code: still remembered
  await page.locator('#uTemp').selectOption('c');
  await expect(page.locator('#umsg')).toHaveText('Saved');
  await expect(page.locator('#keyNote')).toBeHidden();
  expect((await state(request)).units.temp).toBe('c');
});

test('without the key a change is refused and the page says how to get it', async ({ page, request }) => {
  await request.post('/__key', { data: { key: KEY } });
  await page.goto('/');
  await page.locator('#uTemp').selectOption('f');
  await expect(page.locator('#umsg')).toHaveText('Not saved (401)');
  await expect(page.locator('#keyNote')).toBeVisible();
  await expect(page.locator('#keyNote')).toContainText('scan the code on the display');
  expect((await state(request)).units.temp).toBe('c');
  await reviewShot(page.locator('#keyNote'), 'shots/review_key_note.png');
});

test('a refused Wi-Fi save says so (it said "restarting")', async ({ page, request }) => {
  await request.post('/__key', { data: { key: KEY } });
  await page.goto('/');
  await page.locator('#ssid').fill('HomeNet');
  await page.getByRole('button', { name: 'Save Wi-Fi & restart' }).click();
  await expect(page.locator('#wmsg')).toHaveText('Not saved (401)');
  expect((await state(request)).wifi).toBe(null);
});

test('network names are limited to 32 characters', async ({ page }) => {
  await page.goto('/');
  await page.locator('#ssid').fill('x'.repeat(40));
  await expect(page.locator('#ssid')).toHaveValue('x'.repeat(32));
});

test('on the setup network: no key needed, places listed without their coordinates', async ({ page, request }) => {
  await request.post('/__key', { data: { key: KEY } });
  await request.post('/__setup');
  await page.goto('/');
  await expect(page.locator('#places')).toContainText('Québec');
  await expect(page.locator('#setupNote')).toContainText('no internet');     // why the map and search won't load
  await page.setViewportSize({ width: 390, height: 844 });
  await reviewShot(page.locator('#locCard'), 'shots/setup_places_note.png');
  await expect(page.locator('#places .muted')).toHaveText('');
  await page.locator('#places .pl').first().click();
  await expect(page.locator('#lat')).toHaveValue('');              // not moved by a save without a new spot
  await page.getByRole('button', { name: 'Save', exact: true }).click();
  await expect(page.locator('#msg')).toHaveText('Pick a spot on the map or enter valid coordinates.');
  await page.locator('#editor #cancelEd').click();
  await page.locator('#uTemp').selectOption('f');                 // a change without the key: allowed here
  await expect(page.locator('#umsg')).toHaveText('Saved');
});
