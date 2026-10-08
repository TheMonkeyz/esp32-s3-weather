// Screen & presence (espforge's forge_presence on the display): calibration shows its state and its verdict ("cal"),
// refusals are 200 {"ok":false,"why"} answers, a save the display couldn't keep says so. From espforge's
// tools/webtest/tests/screen.spec.js, in this page's words.
const { test, expect, state } = require('./fixtures');

const calls = async (request, url) => (await state(request)).log.filter(e => e.method === 'POST' && e.url === url);

test('calibrate shows "Calibrating", then the background noise it measured', async ({ page, request }) => {
  await page.goto('/');
  await expect(page.locator('#pState')).toHaveText('Active');
  await page.locator('#pCal').click();
  await expect(page.locator('#pmsg')).toHaveText('Calibrating: stay quiet for 5 seconds…');
  await expect(page.locator('#pState')).toContainText('Calibrating… ');
  await expect(page.locator('#pCal')).toBeDisabled();
  expect((await calls(request, '/api/calibrate')).map(e => e.body)).toEqual([{ seconds: 5 }]);
  // The mock ends it after 5 polls with cal "ok" and a baseline of -66 dBFS
  await expect(page.locator('#pmsg')).toHaveText('Background noise measured: -66 dBFS.', { timeout: 15000 });
  await expect(page.locator('#pmsg')).toHaveClass('ok');
  await expect(page.locator('#pState')).toHaveText('Active');
  await expect(page.locator('#pCal')).toBeEnabled();
});

test('a calibration in a noisy room says the previous level was kept', async ({ page, request }) => {
  await page.goto('/');
  await expect(page.locator('#pState')).toHaveText('Active');
  await page.locator('#pCal').click();
  await expect(page.locator('#pState')).toContainText('Calibrating… ');
  await request.post('/__presence', { data: { calibrating: false, calib_left_s: 0, cal: 'noisy', cal_spread_db: 18 } });
  await expect(page.locator('#pmsg')).toHaveText(
    "The room wasn't quiet enough: the previous level (-60 dBFS) was kept. Try again in silence.", { timeout: 10000 });
  await expect(page.locator('#pmsg')).toHaveClass('err');
  await expect(page.locator('#pCal')).toBeEnabled();
  expect((await state(request)).presence.baseline_db).toBe(-60);
});

test("an earlier calibration's verdict is not shown on opening the page", async ({ page, request }) => {
  await request.post('/__presence', { data: { cal: 'noisy' } });
  await page.goto('/');
  await expect(page.locator('#pState')).toHaveText('Active');
  await page.waitForTimeout(1600);                                      // two more polls
  await expect(page.locator('#pmsg')).toBeEmpty();
});

test('no microphone: calibrate is refused with a message', async ({ page, request }) => {
  await page.goto('/');
  await expect(page.locator('#pState')).toHaveText('Active');
  await request.post('/__presence', { data: { mic_ok: false } });
  await page.locator('#pCal').click();
  await expect(page.locator('#pmsg')).toHaveText('Calibration unavailable.');
  await expect(page.locator('#pmsg')).toHaveClass('err');
  await expect(page.locator('#pState')).toHaveText('No microphone');
  expect((await calls(request, '/api/calibrate')).length).toBe(1);
});

test('a save the display could not keep says so', async ({ page, request }) => {
  await request.post('/__presence', { data: { not_saved: true } });
  await page.goto('/');
  await expect(page.locator('#pState')).toHaveText('Active');
  await page.getByRole('button', { name: 'Save screen settings' }).click();
  await expect(page.locator('#pmsg')).toHaveText('Save failed.');
  await expect(page.locator('#pmsg')).toHaveClass('err');
});

test('the calibration verdict in French', async ({ page, request }) => {
  await request.post('/api/units', { data: { lang: 'fr' } });
  await page.goto('/');
  await expect(page.locator('#pState')).toHaveText('Actif');
  await page.locator('#pCal').click();
  await expect(page.locator('#pmsg')).toHaveText('Calibration : gardez le silence 5 secondes…');
  await request.post('/__presence', { data: { calibrating: false, calib_left_s: 0, cal: 'noisy' } });
  await expect(page.locator('#pmsg')).toHaveText("La pièce n'était pas assez silencieuse : le niveau précédent " +
    '(-60 dBFS) a été conservé. Réessayez en gardant le silence.', { timeout: 10000 });
  await page.locator('#pCal').click();
  await expect(page.locator('#pmsg')).toHaveText('Bruit de fond mesuré : -66 dBFS.', { timeout: 15000 });
});

test('the calibration verdict in Inuktitut', async ({ page, request }) => {
  await request.post('/api/units', { data: { lang: 'iu' } });
  await page.goto('/');
  await expect(page.locator('#pState')).toHaveText('ᐊᐅᓚᔪᖅ');
  await page.locator('#pCal').click();
  await expect(page.locator('#pmsg')).toHaveText('ᓂᐲᑦ ᐆᒃᑐᖅᓯᐅᕐᓯᒪᔪᑦ: -66 dBFS.', { timeout: 15000 });
});
