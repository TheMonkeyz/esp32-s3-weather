// Settings page: loads, units
const { test, expect, state, reviewShot } = require('./fixtures');

test('page loads with the display settings', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('#places')).toContainText('Québec');
  await expect(page.locator('#places')).toContainText('On screen');
  await expect(page.locator('#editor')).toBeHidden();
  await expect(page.locator('#uTemp')).toHaveValue('c');
  await expect(page.locator('#fwVer')).toHaveText('v1.5.0-test');
  await expect(page.locator('#curSsid')).toHaveText('HomeNet');
});

test('changing a unit saves it on the display', async ({ page, request }) => {
  await page.goto('/');
  await page.locator('#uTemp').selectOption('f');
  await expect(page.locator('#umsg')).toHaveText('Saved');
  expect((await state(request)).units.temp).toBe('f');
});

test('wake on pick-up can be turned off', async ({ page, request }) => {
  await page.goto('/');
  await expect(page.locator('#pMotion')).toBeChecked();
  await page.locator('#pMotion').uncheck();
  await page.getByRole('button', { name: 'Save screen settings' }).click();
  await expect(page.locator('#pmsg')).toHaveText('Saved.');
  expect((await state(request)).presence.motion_wake).toBe(false);
});

test('no pick-up option without a motion sensor', async ({ page, request }) => {
  await request.post('/__presence', { data: { imu_ok: false } });
  await page.goto('/');
  await expect(page.locator('#pState')).not.toHaveText('…');
  await expect(page.locator('#pMotionBox')).toBeHidden();
});

test('pick-up sensitivity is saved, the meter shows the threshold', async ({ page, request }) => {
  await page.goto('/');
  await expect(page.locator('#pMvTxt')).toHaveText('Movement 0.01 g · wakes at 0.10 g');
  await expect(page.locator('#pMotionSens')).toHaveValue('0.1');
  await page.locator('#pMotionSens').selectOption('0.2');
  await page.getByRole('button', { name: 'Save screen settings' }).click();
  await expect(page.locator('#pmsg')).toHaveText('Saved.');
  expect((await state(request)).presence.motion_thr).toBe(0.2);
  await page.locator('#locCard').scrollIntoViewIfNeeded();
  await reviewShot(page.locator('#presCard'), 'shots/review_presence_card.png');
});

test('the 10-second "Testing" timings only with ?test', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('#pPreset option[value=testing]')).toBeHidden();
  await page.goto('/?test');
  await expect(page.locator('#pPreset option[value=testing]')).not.toHaveAttribute('hidden', '');
});

test('the Beta channel says what it means', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('#fwBeta')).toBeHidden();
  await page.locator('#fwChan').selectOption('beta');
  await expect(page.locator('#fwBeta')).toBeVisible();
  await expect(page.locator('#fwBeta')).toContainText('Release candidates');
});
