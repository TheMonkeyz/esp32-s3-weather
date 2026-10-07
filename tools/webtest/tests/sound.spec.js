// Sound card: alert chime level, volume, quiet hours, test
const { test, expect, state, reviewShot } = require('./fixtures');

test('sound settings are saved and the test chime is requested', async ({ page, request }) => {
  await page.goto('/');
  await expect(page.locator('#sLevel')).toHaveValue('2');
  await expect(page.locator('#sFrom')).toHaveValue('22:00');
  await page.locator('#sLevel').selectOption('1');
  await expect(page.locator('#smsg')).toHaveText('Saved');
  await page.locator('#sFrom').fill('21:30');
  await page.locator('#sFrom').dispatchEvent('change');
  await page.getByRole('button', { name: /Test the sound/ }).click();
  await expect.poll(async () => (await state(request)).sound.tests).toBe(1);
  const s = (await state(request)).sound;
  expect(s.level).toBe(1);
  expect(s.quiet_from).toBe('21:30');
  await reviewShot(page.locator('#soundCard'), 'shots/review_sound_card.png');
});

test('the sound card in French', async ({ page, request }) => {
  await request.post('/api/units', { data: { lang: 'fr' } });
  await page.goto('/');
  await expect(page.locator('#soundCard h2')).toHaveText('Son');
  await expect(page.locator('#sLevel option[value="2"]')).toHaveText('Alertes orange et rouges');
  await reviewShot(page.locator('#soundCard'), 'shots/review_sound_card_fr.png');
});
