// Places card: list, editor with map, add / edit / show / delete
const { test, expect, state, addPlace, reviewShot } = require('./fixtures');

const mapReady = page => expect(page.locator('#map .leaflet-marker-icon')).toBeVisible();

test('add a place from the city search', async ({ page, request }) => {
  await page.goto('/');
  await page.getByRole('button', { name: /Add a place/ }).click();
  await expect(page.locator('#edTitle')).toHaveText('New place');
  await mapReady(page);
  await page.locator('#q').fill('Montr');
  await page.locator('#results div').first().click();
  await expect(page.locator('#name')).toHaveValue('Montréal');
  await expect(page.locator('#lat')).toHaveValue('45.5019');
  await page.getByRole('button', { name: 'Save', exact: true }).click();
  await expect(page.locator('#editor')).toBeHidden();
  await expect(page.locator('#places')).toContainText('Montréal');
  await expect(page.locator('#msg')).toContainText('Added Montréal');
  const s = await state(request);
  expect(s.places.map(p => p.name)).toEqual(['Québec', 'Montréal']);
  expect(s.active).toBe(0);                          // adding doesn't change the place shown
});

test('add a place by tapping the map suggests its name', async ({ page, request }) => {
  await page.goto('/');
  await page.getByRole('button', { name: /Add a place/ }).click();
  await mapReady(page);
  const box = await page.locator('#map').boundingBox();
  await page.mouse.click(box.x + box.width * 0.75, box.y + box.height * 0.3);   // north-east of the centre
  await expect(page.locator('#name')).toHaveValue('Lac-Beauport');
  const lat = parseFloat(await page.locator('#lat').inputValue());
  const lon = parseFloat(await page.locator('#lon').inputValue());
  expect(lat).toBeGreaterThan(46.8139);
  expect(lon).toBeGreaterThan(-71.208);
  await page.getByRole('button', { name: 'Save', exact: true }).click();
  await expect(page.locator('#editor')).toBeHidden();               // the save has been answered
  await expect(page.locator('#places')).toContainText('Lac-Beauport');
  const s = await state(request);
  expect(s.places[1]).toMatchObject({ name: 'Lac-Beauport' });
  expect(s.places[1].lat).toBeCloseTo(lat, 3);
});

test('edit a place: tap it, move the pin, keep its name', async ({ page, request }) => {
  await page.goto('/');
  await page.locator('#places .pl').first().click();
  await expect(page.locator('#edTitle')).toHaveText('Edit Québec');
  await expect(page.locator('#delPlace')).toBeHidden();            // the only place can't be deleted
  await mapReady(page);
  const box = await page.locator('#map').boundingBox();
  await page.mouse.click(box.x + box.width * 0.3, box.y + box.height * 0.7);
  await expect(page.locator('#name')).toHaveValue('Québec');       // a place's own name isn't replaced
  await page.getByRole('button', { name: 'Save', exact: true }).click();
  await expect(page.locator('#editor')).toBeHidden();               // the save has been answered
  const s = await state(request);
  expect(s.places).toHaveLength(1);
  expect(s.places[0].name).toBe('Québec');
  expect(s.places[0].lat).toBeLessThan(46.8139);
});

test('cancel leaves the place unchanged', async ({ page, request }) => {
  await page.goto('/');
  await page.locator('#places .pl').first().click();
  await mapReady(page);
  await page.locator('#name').fill('Changed');
  await page.getByRole('button', { name: 'Cancel' }).click();
  await expect(page.locator('#editor')).toBeHidden();
  expect((await state(request)).places[0].name).toBe('Québec');
});

test('show another place, then delete it', async ({ page, request }) => {
  await addPlace(request, 'Cottage', 47.0, -71.5, 1);
  await page.goto('/');
  await page.locator('#places .pl', { hasText: 'Cottage' }).getByRole('button', { name: 'Show' }).click();
  await expect(page.locator('#places .pl', { hasText: 'Cottage' })).toContainText('On screen');
  await expect(page.locator('#editor')).toBeHidden();              // Show doesn't open the editor
  expect((await state(request)).active).toBe(1);
  await page.locator('#places .pl', { hasText: 'Cottage' }).click();
  page.once('dialog', d => d.accept());
  await page.getByRole('button', { name: 'Delete this place' }).click();
  await expect(page.locator('#places')).not.toContainText('Cottage');
  const s = await state(request);
  expect(s.places.map(p => p.name)).toEqual(['Québec']);
  expect(s.active).toBe(0);
});

test('a name too long for the display is refused with a message', async ({ page, request }) => {
  await page.goto('/');
  await page.locator('#addPlace').click();
  await page.locator('#name').fill('ᐃᓄᒃᑎᑐᑦ'.repeat(3));               // 21 syllabics = 63 bytes > 47
  await page.locator('#coords').evaluate(d => { d.open = true; });
  await page.locator('#lat').fill('63.75');
  await page.locator('#lon').fill('-68.52');
  await page.getByRole('button', { name: 'Save', exact: true }).click();
  await expect(page.locator('#msg')).toHaveText('This name is too long for the display. Shorten it.');
  expect((await state(request)).places.length).toBe(1);
});

test('no Add button with 4 places', async ({ page, request }) => {
  for (const [i, n] of ['A', 'B', 'C'].entries()) await addPlace(request, n, 46 + i, -71, i + 1);
  await page.goto('/');
  await expect(page.locator('#places .pl')).toHaveCount(4);
  await expect(page.getByRole('button', { name: /Add a place/ })).toBeHidden();
});

test.describe('phone without internet', () => {
  test.use({ noInternet: true });
  test('the map is replaced by the coordinates', async ({ page, request }) => {
    await page.goto('/');
    await page.getByRole('button', { name: /Add a place/ }).click();
    await expect(page.locator('#mapNote')).toContainText('needs internet');
    await expect(page.locator('#map')).toBeHidden();
    await expect(page.locator('#coords')).toHaveAttribute('open', '');
    await page.locator('#name').fill('Camp');
    await page.locator('#lat').fill('48.1');
    await page.locator('#lon').fill('-70.2');
    await page.getByRole('button', { name: 'Save', exact: true }).click();
    expect((await state(request)).places[1]).toEqual({ name: 'Camp', lat: 48.1, lon: -70.2 });
  });
});

test('review: the Places card, list and editor', async ({ page, request }) => {
  await addPlace(request, 'Chalet', 47.06, -71.35, 1);
  await addPlace(request, 'Bureau', 46.81, -71.22, 2);
  await page.goto('/');
  await expect(page.locator('#places .pl')).toHaveCount(3);
  await reviewShot(page.locator('#locCard'), 'shots/review_places_list.png');
  await page.locator('#places .pl', { hasText: 'Chalet' }).click();
  await mapReady(page);
  await reviewShot(page.locator('#locCard'), 'shots/review_places_editor.png');
});
