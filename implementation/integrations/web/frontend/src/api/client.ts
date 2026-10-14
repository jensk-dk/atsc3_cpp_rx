import type { AnyPlayerSettings, AppsIndex, EsgContent, EsgSchedule, EsgService, EsgSummary, LinkInfo, LlsTable, MediaIndex, MmtIndex, ServiceDetail, Service, SourceList, Status, TimeInfo } from './types'

const base = '/api/v1'

async function get<T>(path: string, root = base): Promise<T> {
  const r = await fetch(root + path, { cache: 'no-store' })
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`)
  return (await r.json()) as T
}

async function send<T>(method: string, path: string, params?: Record<string, string>): Promise<T> {
  // Always send a (possibly empty) form body: the server reads the request body before routing.
  const body = params ? new URLSearchParams(params).toString() : ''
  const r = await fetch(base + path, {
    method,
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body,
  })
  const data = (await r.json().catch(() => ({}))) as T & { error?: string }
  if (!r.ok) throw new Error(data.error ?? `${path}: HTTP ${r.status}`)
  return data
}

export const api = {
  status: (since?: number, waitMs = 0) =>
    get<Status>(since === undefined ? '/status' : `/status?since=${since}&wait=${waitMs}`),
  services: () => get<Service[]>('/services'),
  service: (bsid: number, sid: number) => get<ServiceDetail>(`/services/${bsid}/${sid}`),
  select: (bsid: number, sid: number, on: boolean) =>
    send<{ selected: boolean }>(on ? 'POST' : 'DELETE', `/services/${bsid}/${sid}/select`),
  lls: () => get<LlsTable[]>('/lls'),
  link: () => get<LinkInfo>('/link'),
  source: () => get<SourceList>('/source'),
  open: (path: string, pacing: string, speed: number, loop: boolean) =>
    send<Status>('POST', '/source', { path, pacing, speed: String(speed), loop: loop ? '1' : '0' }),
  sourceAction: (action: 'close' | 'pause' | 'resume') => send<Status>('POST', '/source', { action }),
  time: () => get<TimeInfo>('/time'),
  esg: () => get<EsgSummary>('/esg'),
  esgServices: () => get<EsgService[]>('/esg/services'),
  esgSchedule: (from?: number, to?: number, service?: string) => {
    const q = new URLSearchParams()
    if (from !== undefined) q.set('from', String(Math.floor(from)))
    if (to !== undefined) q.set('to', String(Math.ceil(to)))
    if (service) q.set('service', service)
    const qs = q.toString()
    return get<EsgSchedule>('/esg/schedule' + (qs ? '?' + qs : ''))
  },
  esgContent: (id: string) => get<EsgContent>(`/esg/content/${encodeURIComponent(id)}`),
  // media origin (not under /api: dash.js fetches the MPD and segments from /media)
  mediaIndex: (bsid: number, sid: number, since = 0) => get<MediaIndex>(`/${bsid}/${sid}/index.json?since=${since}`, '/media'),
  // player.json: mode "dash" (ROUTE: MPD + dash.js) or "push" (MMTP: MsePushPlayer polling mmt/index.json); a missing
  // mode means "dash"
  playerSettings: (bsid: number, sid: number) => get<AnyPlayerSettings>(`/${bsid}/${sid}/player.json`, '/media'),
  // MMT push origin index (spec 5.17.4): fragments with seq > since, in arrival order
  mmtIndex: (bsid: number, sid: number, since = 0) => get<MmtIndex>(`/${bsid}/${sid}/mmt/index.json?since=${since}`, '/media'),
  // Application Context Cache origin (spec 5.18.4): HELD apps with readiness, cached files, the entry URLs
  appsIndex: (bsid: number, sid: number) => get<AppsIndex>(`/${bsid}/${sid}/index.json`, '/apps'),
}
