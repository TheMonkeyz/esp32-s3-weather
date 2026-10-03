// Firmware card: an offered update with release notes, Install, the download, a language switch meanwhile
const { test, expect, state } = require('./fixtures');

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
  await page.locator('#fwCard').screenshot({ path: 'shots/review_update_card.png' });
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
  await page.locator('#fwCard').screenshot({ path: 'shots/review_update_card_fr.png' });
});
