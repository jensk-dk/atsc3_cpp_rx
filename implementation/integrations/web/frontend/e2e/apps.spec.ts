import { test, expect, type APIRequestContext, type Frame, type Page } from '@playwright/test'

// Spec 5.18.4 "broadcaster applications": the UniSoft usc01 service signals a HELD app (uscapp, a canvas drawing
// "ATSC 3.0") delivered as a multipart package on TSI 4. Selecting the service on its page must launch the app over
// the player, the app must reach the receiver API through the injected WebSocket shim, and the virtual remote must
// route keys: granted keys into the app, the others to the receiver. Needs atsc3d (playwright.config.ts baseURL).
const UNI = 'UniSoft-3xROUTE-24-October-2019.pcap'

async function openUniSoft(page: Page, request: APIRequestContext) {
  const src = await request.get('/api/v1/source')
  test.skip(!src.ok(), 'atsc3d not running')
  const captures = (await src.json()).captures as { name: string; path: string }[]
  const uni = captures.find((c) => c.name === UNI)
  test.skip(!uni, 'UniSoft capture not in the capture directories')
  await request.post('/api/v1/source', { form: { path: uni!.path, pacing: 'realtime', loop: '1' } })
  await request.post('/api/v1/services/24/1/select', { form: {} })
  await page.goto('/services/24/1')
  await expect(page.getByText('usc01').first()).toBeVisible({ timeout: 15_000 })
}

function appFrame(page: Page): Frame {
  const f = page.frames().find((fr) => fr.url().includes('/apps/24/1/0/uscapp/home.html'))
  if (!f) throw new Error('application frame not found')
  return f
}

test('UniSoft usc01 launches its broadcaster application over the player and serves the receiver API', async ({ page, request }) => {
  await openUniSoft(page, request)
  // HELD table and the runtime panel
  await expect(page.getByText('Broadcaster applications (HELD)')).toBeVisible()
  await expect(page.getByRole('cell', { name: 'uscapp', exact: true })).toBeVisible()
  await expect(page.getByText('Broadcaster application', { exact: true })).toBeVisible({ timeout: 15_000 })
  // the package repeats on the carousel many times a second: launched within moments of the selection
  await expect(page.getByText('running', { exact: true })).toBeVisible({ timeout: 30_000 })
  const iframe = page.locator('iframe.app-frame')
  await expect(iframe).toHaveAttribute('src', /\/apps\/24\/1\/0\/uscapp\/home\.html\?wsURL=ws:\/\/[^&]+\/api\/v1\/a344\/24\/1&rev=20240403$/)
  await expect(page.frameLocator('iframe.app-frame').locator('canvas#canvas')).toBeVisible({ timeout: 15_000 })
  await expect(page.getByText(/entry page loaded/).first()).toBeVisible({ timeout: 15_000 })
  // the app's view of the receiver: wsURL + /atscCmd through the shim, JSON-RPC answered by the page
  const frame = appFrame(page)
  const rpc = (method: string, params?: object) =>
    frame.evaluate(
      ([m, p]) =>
        new Promise<unknown>((resolve, reject) => {
          const ws = new WebSocket(new URLSearchParams(location.search).get('wsURL') + '/atscCmd')
          const req: Record<string, unknown> = { jsonrpc: '2.0', id: 7, method: m }
          if (p) req.params = p
          ws.onopen = () => ws.send(JSON.stringify(req))
          ws.onmessage = (e) => { resolve(JSON.parse(e.data as string)); ws.close() }
          ws.onerror = () => reject(new Error('websocket error'))
          setTimeout(() => reject(new Error('timeout')), 5000)
        }),
      [method, params] as const,
    )
  const svc = (await rpc('org.atsc.query.service')) as { id: number; result: { service: string | null } }
  expect(svc.id).toBe(7)
  expect(svc.result).toHaveProperty('service')
  const base = (await rpc('org.atsc.query.baseURI')) as { result: { baseURI: string } }
  expect(base.result.baseURI).toMatch(/\/apps\/24\/1\/0\/$/)
  const info = (await rpc('org.atsc.query.deviceInfo')) as { result: { deviceInput: { ArrowUp: number; Select: number; BAAppear: { keycode: number } } } }
  expect(info.result.deviceInput.ArrowUp).toBe(38)
  expect(info.result.deviceInput.Select).toBe(13)
  const unknown = (await rpc('org.atsc.query.MPDUrl')) as { error: { code: number } }
  expect(unknown.error.code).toBe(-32601)
  const state = (await rpc('org.atsc.query.rmpPlaybackState')) as { result: { playbackState: number } }
  expect([-1, 0, 1, 2]).toContain(state.result.playbackState)
  // an optional interface the receiver does not provide fails like a 404
  const vid = await frame.evaluate(
    () =>
      new Promise<string>((resolve) => {
        const ws = new WebSocket(new URLSearchParams(location.search).get('wsURL') + '/atscVid')
        ws.onopen = () => resolve('open')
        ws.onerror = () => resolve('error')
        ws.onclose = () => resolve('closed')
      }),
  )
  expect(vid).not.toBe('open')
  // the console shows the exchange
  await expect(page.getByText('org.atsc.query.deviceInfo').first()).toBeVisible()
})

test('virtual remote: granted keys reach the application, the others stay with the receiver', async ({ page, request }) => {
  await openUniSoft(page, request)
  await expect(page.getByText('running', { exact: true })).toBeVisible({ timeout: 30_000 })
  await expect(page.frameLocator('iframe.app-frame').locator('canvas#canvas')).toBeVisible({ timeout: 15_000 })
  const frame = appFrame(page)
  // the app records every key it receives, like a navigation library would
  await frame.evaluate(() => {
    const w = window as Window & { __keys?: string[] }
    w.__keys = []
    window.addEventListener('keydown', (e) => w.__keys!.push(`${e.key}:${e.keyCode}`))
  })
  // before any request the app holds nothing: a navigation key is issued to it as BAAppear (A/344 9.11)
  await page.getByRole('button', { name: '▲' }).click()
  await expect.poll(() => frame.evaluate(() => (window as Window & { __keys?: string[] }).__keys)).toEqual(['BAAppear:1001'])
  await expect(page.getByText(/ArrowUp issued as BAAppear/).first()).toBeVisible()
  // the app asks for the arrows, Select and Numeric but not ChannelUp
  const accepted = await frame.evaluate(
    () =>
      new Promise<string[]>((resolve, reject) => {
        const ws = new WebSocket(new URLSearchParams(location.search).get('wsURL') + '/atscCmd')
        ws.onopen = () => ws.send(JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'org.atsc.request.keys', params: { keys: ['ArrowUp', 'ArrowDown', 'Select', 'Numeric', 'ChannelUp'] } }))
        ws.onmessage = (e) => resolve((JSON.parse(e.data as string) as { result: { accepted: string[] } }).result.accepted)
        ws.onerror = () => reject(new Error('websocket error'))
      }),
  )
  expect(accepted).toEqual(['ArrowUp', 'ArrowDown', 'Select', 'Numeric'])
  await expect(page.getByText('ArrowUp', { exact: true }).first()).toBeVisible()
  // granted keys are delivered with the legacy keyCode the device info promised
  await page.getByRole('button', { name: '▲' }).click()
  await page.getByRole('button', { name: 'OK' }).click()
  await page.getByRole('button', { name: '5', exact: true }).click()
  await expect.poll(() => frame.evaluate(() => (window as Window & { __keys?: string[] }).__keys)).toEqual(['BAAppear:1001', 'ArrowUp:38', 'Enter:13', '5:53'])
  // the physical keyboard on the host page takes the same route
  await page.locator('body').click({ position: { x: 5, y: 5 } })
  await page.keyboard.press('ArrowDown')
  await expect.poll(() => frame.evaluate(() => (window as Window & { __keys?: string[] }).__keys)).toContain('ArrowDown:40')
  // not granted: ArrowLeft stays with the receiver, Back hides the application
  await page.getByRole('button', { name: '◀' }).click()
  await expect(page.getByText(/receiver: ArrowLeft/).first()).toBeVisible()
  expect(await frame.evaluate(() => (window as Window & { __keys?: string[] }).__keys)).not.toContain('ArrowLeft:37')
  await page.getByRole('button', { name: 'Back' }).click()
  await expect(page.getByText('running, hidden')).toBeVisible()
  await expect(page.locator('iframe.app-frame')).toBeHidden()
  await page.getByRole('button', { name: 'App', exact: true }).click()
  await expect(page.getByText('running', { exact: true })).toBeVisible()
  await expect(page.locator('iframe.app-frame')).toBeVisible()
  await expect.poll(() => frame.evaluate(() => (window as Window & { __keys?: string[] }).__keys!.filter((k) => k.startsWith('BAAppear')).length)).toBe(2)
  // Terminate removes the frame and resets the grants
  await page.getByRole('button', { name: 'Terminate' }).click()
  await expect(page.locator('iframe.app-frame')).toHaveCount(0)
  await expect(page.getByText('terminated', { exact: true })).toBeVisible()
})
