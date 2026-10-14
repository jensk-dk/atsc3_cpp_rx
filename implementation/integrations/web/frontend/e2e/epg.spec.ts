import { test, expect } from '@playwright/test'

// Spec 5.18.3 "EPG": with the BWI capture open, the EPG view lists the four A/V services from the guide,
// draws programmes around the capture's own clock and shows the content detail on click.
// Needs atsc3d on :8080 (playwright.config.ts baseURL) and the corpus.
const BWI = 'BWI_20230307_1742.pcap'

test('renders the Baltimore guide and a programme detail', async ({ page, request }) => {
  const src = await request.get('/api/v1/source')
  test.skip(!src.ok(), 'atsc3d not running')
  const captures = (await src.json()).captures as { name: string; path: string }[]
  const bwi = captures.find((c) => c.name === BWI)
  test.skip(!bwi, 'BWI capture not in the capture directories')
  await request.post('/api/v1/source', { form: { path: bwi!.path, pacing: 'realtime', loop: '1' } })
  await page.goto('/epg')
  // the SGDUs carousel every ~35 s; the Service fragments arrive with the first service/schedule unit.
  // A guide row is a link "<major>.<minor> <name>" (the channel number comes from the SLT when it has one).
  for (const name of ['WBFF', 'WBAL', 'WMAR', 'WMPT', 'WNUV']) {
    await expect(page.getByRole('link', { name: new RegExp(`(^|\\s)${name}$`) }).first()).toBeVisible({ timeout: 60_000 })
  }
  await expect(page.getByText('capture time')).toBeVisible()
  // Schedule fragments precede their Content fragments on the carousel; a cell whose content has not arrived yet
  // reads "(content fragment not received)", so pick the first resolved programme.
  const programme = page.locator('.prog').filter({ hasNotText: 'content fragment not received' }).first()
  await expect(programme).toBeVisible({ timeout: 60_000 })
  await programme.click()
  await expect(page.getByText('Content id')).toBeVisible({ timeout: 10_000 })
  await expect(page.getByText('Content fragment XML')).toBeVisible()
})
