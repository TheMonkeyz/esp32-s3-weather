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

// Inuktitut (draft, docs/translations/): every text translated, and nothing wider than its box
test('the page in Inuktitut fits', async ({ page, request }) => {
  await request.post('/api/units', { data: { lang: 'iu' } });
  await page.setViewportSize({ width: 390, height: 844 });            // a phone
  await page.goto('/');
  await expect(page.locator('#uLang')).toHaveValue('iu');
  await expect(page.locator('h1')).toHaveText('ᓯᓚᒧᑦ ᓴᖅᑭᔭᐅᑦ');
  await expect(page.locator('#pState')).toHaveText('ᐊᐅᓚᔪᖅ');           // the status lines come from the 0.7 s poll
  await expect(page.locator('#pMvTxt')).not.toContainText('Movement');
  const report = await page.evaluate(() => {
    const out = { latin: [], overflow: [] };
    for (const el of document.querySelectorAll('[data-i18n], [data-i18n-html], button, label, option, h1, h2, #presCard div, #presCard span')) {
      const t = (el.textContent || '').trim();
      // English left over: a Latin word of 4+ letters, other than names kept on purpose
      const w = t.replace(/Wi-Fi|Radar|OK|Android|iPhone|Easy Connect|Face ID|Settings|Password|Copy|Share|Beta|Latitude|Longitude|Orange|internet|PM|dB|iOS|QR/g, '');
      if (/[A-Za-z]{4,}/.test(w) && !el.closest('#places, #uLang') && !el.children.length) out.latin.push(t.slice(0, 60));
      if (el.tagName !== 'OPTION' && el.offsetParent && el.scrollWidth > el.clientWidth + 1 && getComputedStyle(el).overflow !== 'visible')
        out.overflow.push(`${el.tagName}#${el.id} "${t.slice(0, 40)}"`);
    }
    const doc = document.documentElement;
    out.pageWider = doc.scrollWidth > doc.clientWidth;
    return out;
  });
  console.log(JSON.stringify(report, null, 1));
  expect(report.latin).toEqual([]);
  expect(report.overflow).toEqual([]);
  expect(report.pageWider).toBe(false);
  await page.screenshot({ path: 'shots/review_iu_page.png', fullPage: true });
});
