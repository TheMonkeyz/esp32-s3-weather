// Settings page: loads, units
const { test, expect, state } = require('./fixtures');

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
