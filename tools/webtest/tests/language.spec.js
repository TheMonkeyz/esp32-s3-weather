// Language: the page follows the display's language and changes it
const { test, expect, state } = require('./fixtures');

test('switching to French translates the page and the display', async ({ page, request }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Weather display');
  await page.locator('#uLang').selectOption('fr');
  await expect(page.locator('h1')).toHaveText('Afficheur météo');
  await expect(page.locator('#addPlace')).toHaveText('＋ Ajouter un endroit');
  await expect(page.locator('#places')).toContainText("À l'écran");
  await expect(page.locator('#umsg')).toHaveText('Enregistré');
  expect((await state(request)).units.lang).toBe('fr');
  await expect(page.locator('#pMvTxt')).toHaveText('Mouvement 0,01 g · réveil à 0,10 g');   // decimal comma
  await page.locator('#presCard').screenshot({ path: 'shots/review_fr_presence.png' });
});

test('a French display opens the page in French', async ({ page, request }) => {
  await request.post('/api/units', { data: { lang: 'fr' } });
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Afficheur météo');
  await expect(page.locator('#uLang')).toHaveValue('fr');
  await page.getByRole('button', { name: /Ajouter un endroit/ }).click();
  await expect(page.locator('#edTitle')).toHaveText('Nouvel endroit');
  await expect(page.locator('#name')).toHaveAttribute('placeholder', 'ex. : Chalet');
  await expect(page.locator('#map .leaflet-marker-icon')).toBeVisible();
  await page.locator('#locCard').screenshot({ path: 'shots/review_fr_editor.png' });
  await page.getByRole('button', { name: 'Annuler' }).click();
  await page.locator('#fwCard').scrollIntoViewIfNeeded();
  await expect(page.locator('#fwState')).toContainText('À jour');
});
