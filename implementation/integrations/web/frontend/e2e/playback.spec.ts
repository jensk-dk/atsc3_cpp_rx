import { test, expect } from '@playwright/test'

// Spec 5.18.3 "playback" at the MSE-append level: open the UniSoft capture (HEVC + AAC-LC, playable in browsers
// with an HEVC decoder), select service 24/2, and expect dash.js to buffer and advance the clock on the service page.
// Needs atsc3d on :8080 (playwright.config.ts baseURL) and the corpus. Chromium without an HEVC decoder plays audio only,
// which still passes the buffering checks; `@hevc` marks the frame-level assertion for the hardware runner.
const UNI = 'UniSoft-3xROUTE-24-October-2019.pcap'

test('UniSoft usc02 buffers and plays through the local DASH origin', async ({ page, request }) => {
  const src = await request.get('/api/v1/source')
  test.skip(!src.ok(), 'atsc3d not running')
  const captures = (await src.json()).captures as { name: string; path: string }[]
  const uni = captures.find((c) => c.name === UNI)
  test.skip(!uni, 'UniSoft capture not in the capture directories')
  await request.post('/api/v1/source', { form: { path: uni!.path, pacing: 'realtime', loop: '1' } })
  await request.post('/api/v1/services/24/2/select', { form: {} })
  await page.goto('/services/24/2')
  await expect(page.getByText('usc02').first()).toBeVisible({ timeout: 15_000 })
  const video = page.locator('video')
  await expect(video).toBeVisible({ timeout: 15_000 })
  await expect.poll(async () => video.evaluate((v: HTMLVideoElement) => v.readyState), { timeout: 60_000 }).toBeGreaterThanOrEqual(2)
  const t0 = await video.evaluate((v: HTMLVideoElement) => v.currentTime)
  await page.waitForTimeout(4000)
  const t1 = await video.evaluate((v: HTMLVideoElement) => v.currentTime)
  expect(t1).toBeGreaterThan(t0 + 2)
  const buffered = await video.evaluate((v: HTMLVideoElement) => (v.buffered.length ? v.buffered.end(v.buffered.length - 1) - v.buffered.start(0) : 0))
  expect(buffered).toBeGreaterThan(1)
  await expect(page.getByText('manifest loaded').or(page.getByText('playing'))).toBeVisible()
})

test('UniSoft usc02 decodes video frames @hevc', async ({ page, request }) => {
  const src = await request.get('/api/v1/source')
  test.skip(!src.ok(), 'atsc3d not running')
  const hevc = await page.evaluate(() => MediaSource.isTypeSupported('video/mp4; codecs="hev1.2.4.L120.90"'))
  test.skip(!hevc, 'no HEVC decoder in this browser')
  await request.post('/api/v1/services/24/2/select', { form: {} })
  await page.goto('/services/24/2')
  const video = page.locator('video')
  await expect(video).toBeVisible({ timeout: 15_000 })
  await expect.poll(async () => video.evaluate((v: HTMLVideoElement) => v.getVideoPlaybackQuality().totalVideoFrames), { timeout: 60_000 }).toBeGreaterThan(30)
  expect(await video.evaluate((v: HTMLVideoElement) => v.videoWidth)).toBe(1920)
})

// MMT services play through the push path (spec 5.17.4, HTTP variant): the UniSoft MMT capture carries usc03m
// (HEVC + AAC-LC in MPU mode). Needs that capture in the capture directories; Chromium without HEVC plays audio only.
const UNI_MMT = 'UniSoft-2xROUTE+1MMT-5-November-2019.pcap'

test('UniSoft usc03m (MMTP) buffers and plays through the push player', async ({ page, request }) => {
  const src = await request.get('/api/v1/source')
  test.skip(!src.ok(), 'atsc3d not running')
  const captures = (await src.json()).captures as { name: string; path: string }[]
  const uni = captures.find((c) => c.name === UNI_MMT)
  test.skip(!uni, 'UniSoft MMT capture not in the capture directories')
  await request.post('/api/v1/source', { form: { path: uni!.path, pacing: 'realtime', loop: '1' } })
  await request.post('/api/v1/services/24/3/select', { form: {} })
  await page.goto('/services/24/3')
  await expect(page.getByText('usc03m').first()).toBeVisible({ timeout: 15_000 })
  const video = page.locator('video')
  await expect(video).toBeVisible({ timeout: 15_000 })
  await expect.poll(async () => video.evaluate((v: HTMLVideoElement) => v.readyState), { timeout: 60_000 }).toBeGreaterThanOrEqual(2)
  const t0 = await video.evaluate((v: HTMLVideoElement) => v.currentTime)
  await page.waitForTimeout(4000)
  const t1 = await video.evaluate((v: HTMLVideoElement) => v.currentTime)
  expect(t1).toBeGreaterThan(t0 + 2)
  await expect(page.getByText('playing').first()).toBeVisible()
  // the MP table calls the AAC asset ac-4; the panel must show the sample-entry codecs instead
  await expect(page.getByText('mp4a.40.2').first()).toBeVisible()
})

test('UniSoft usc03m (MMTP) decodes video frames @hevc', async ({ page, request }) => {
  const src = await request.get('/api/v1/source')
  test.skip(!src.ok(), 'atsc3d not running')
  const hevc = await page.evaluate(() => MediaSource.isTypeSupported('video/mp4; codecs="hev1.2.4.L120.B0"'))
  test.skip(!hevc, 'no HEVC decoder in this browser')
  await request.post('/api/v1/services/24/3/select', { form: {} })
  await page.goto('/services/24/3')
  const video = page.locator('video')
  await expect(video).toBeVisible({ timeout: 15_000 })
  await expect.poll(async () => video.evaluate((v: HTMLVideoElement) => v.getVideoPlaybackQuality().totalVideoFrames), { timeout: 60_000 }).toBeGreaterThan(30)
  expect(await video.evaluate((v: HTMLVideoElement) => v.videoWidth)).toBe(1920)
})
