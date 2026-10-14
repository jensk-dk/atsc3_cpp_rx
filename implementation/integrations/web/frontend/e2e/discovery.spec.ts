import { test, expect } from '@playwright/test'

// Spec 5.18.3 "discovery": open the BWI capture through the API, then the Services view lists the six
// Baltimore services within 6 s. Needs atsc3d on :8080 (playwright.config.ts baseURL) and the corpus.
const BWI = 'BWI_20230307_1742.pcap'

test('lists the six Baltimore services', async ({ page, request }) => {
  const src = await request.get('/api/v1/source')
  test.skip(!src.ok(), 'atsc3d not running')
  const captures = (await src.json()).captures as { name: string; path: string }[]
  const bwi = captures.find((c) => c.name === BWI)
  test.skip(!bwi, 'BWI capture not in the capture directories')
  await request.post('/api/v1/source', { form: { path: bwi!.path, pacing: 'realtime', loop: '1' } })
  await page.goto('/services')
  for (const name of ['WBFF', 'WBAL', 'WMAR', 'WMPT', 'WNUV', 'ESG']) {
    await expect(page.getByText(name, { exact: true }).first()).toBeVisible({ timeout: 6000 })
  }
  await expect(page.getByText('MMTP').first()).toBeVisible()
  await page.getByText('WNUV', { exact: true }).first().click()
  await expect(page.getByText('hev1').first()).toBeVisible({ timeout: 6000 })
  await expect(page.getByText('ac-4').first()).toBeVisible()
})
