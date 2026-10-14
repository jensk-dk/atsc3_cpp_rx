// A/344 receiver interface (JSON-RPC 2.0 over the "WebSocket" of A/344 8.2) for one Broadcaster Application hosted
// in the service view (spec 5.18.4). The application's page runs the daemon-injected shim (public/a344-shim.js):
// its `new WebSocket(wsURL + '/atscCmd')` becomes a postMessage channel to this host, which answers the org.atsc.*
// queries from the page's own state (the service, the video element that is the RMP, the key router) and the
// daemon's REST API (signaling, alerts), and pushes org.atsc.notify to subscribed sockets. Unsupported methods get
// JSON-RPC -32601 so the application sees a conforming receiver that simply lacks the feature.
import type { Service } from '../api/types'
import { KEY_DEFS, type KeyRouter } from './keys'

export interface RpcLogEntry {
  t: number
  dir: 'in' | 'out' | 'notify' | 'info' | 'error'
  text: string
}

export interface AlertEntry {
  alertingType: 'AEAT' | 'OSN'
  alertingFragment: string
  receiveTime?: string
}
export interface SignalingEntry {
  name: string
  group?: number
  version?: number
  table: string
}

export interface HostDeps {
  service: () => Service
  baseUri: string
  wsUrl: string
  video: () => HTMLVideoElement | null
  keys: KeyRouter
  log: (entry: RpcLogEntry) => void
  /// org.atsc.scale-position: false when the RMP cannot be scaled (no player)
  scalePosition: (scaleFactor: number, xPos: number, yPos: number) => boolean
  /// org.atsc.setRMPURL: null on success, else an A/344 Table 8.5 error code
  setRmpUrl: (operation: string, url: string | undefined) => number | null
  alerting: () => Promise<AlertEntry[]>
  signaling: (names: string[]) => Promise<SignalingEntry[]>
  ccEnabled: () => boolean
  onStateChange?: (open: boolean) => void
}

type JsonRpcId = number | string | null
interface JsonRpcRequest {
  jsonrpc?: string
  id?: JsonRpcId
  method?: string
  params?: Record<string, unknown>
}

export class RpcError extends Error {
  constructor(public readonly code: number, message: string) {
    super(message)
  }
}

/// A/344 9.13.3 playback state of an HTMLMediaElement.
export function playbackState(v: HTMLVideoElement | null): number {
  if (!v || v.readyState === 0) return -1
  if (v.ended) return 2
  if (v.paused || v.readyState < 3) return 1
  return 0
}

export const SUPPORTED_METHODS = [
  'org.atsc.query.service', 'org.atsc.query.baseURI', 'org.atsc.query.deviceInfo', 'org.atsc.query.cc', 'org.atsc.query.alerting',
  'org.atsc.query.languages', 'org.atsc.query.captionDisplay', 'org.atsc.query.ratingLevel', 'org.atsc.query.audioAccessibilityPref',
  'org.atsc.query.signaling', 'org.atsc.query.serviceGuideUrls', 'org.atsc.request.keys', 'org.atsc.relinquish.keys',
  'org.atsc.subscribe', 'org.atsc.unsubscribe', 'org.atsc.scale-position', 'org.atsc.setRMPURL',
  'org.atsc.query.rmpMediaTime', 'org.atsc.query.rmpPlaybackState', 'org.atsc.query.rmpPlaybackRate',
]
const NOTIFICATIONS = [
  'ratingBlock', 'serviceChange', 'captionState', 'langPref', 'captionDisplayPrefs', 'audioAccessibilityPref', 'alertingChange',
  'contentChange', 'serviceGuideChange', 'signalingData', 'dialogEnhancementPrefChange', 'dialogEnhancementLimitChange', 'rfSignalChange',
  'contentRecoveryStateChange', 'displayOverrideChange', 'recoveredComponentInfoChange', 'rmpMediaTimeChange', 'rmpPlaybackStateChange',
  'rmpPlaybackRateChange', 'DRM', 'xlinkResolution',
  'requestKeyTimeout',   // not in Table 9.31 (the receiver sends it on its own) but applications subscribe to it
]

interface Socket {
  id: number
  url: string
  open: boolean
  subscriptions: Set<string>
}

export class A344Host {
  private sockets = new Map<number, Socket>()
  private readonly onMessage = (ev: MessageEvent) => this.message(ev)
  private timer = 0
  private lastState = -2
  private lastRate = 1
  private lastTime = -1
  hello: { wsUrl: string; rev: string } | null = null

  constructor(private readonly frame: HTMLIFrameElement, private readonly deps: HostDeps) {}

  attach(): void {
    window.addEventListener('message', this.onMessage)
    this.timer = window.setInterval(() => this.tick(), 1000)
  }
  detach(): void {
    window.removeEventListener('message', this.onMessage)
    window.clearInterval(this.timer)
    for (const s of this.sockets.values()) if (s.open) this.send(s.id, { type: 'closed', code: 1001, reason: 'application terminated' })
    this.sockets.clear()
  }

  get connections(): number {
    let n = 0
    for (const s of this.sockets.values()) if (s.open) ++n
    return n
  }
  get subscriptions(): string[] {
    const all = new Set<string>()
    for (const s of this.sockets.values()) for (const m of s.subscriptions) all.add(m)
    return [...all].sort()
  }

  /// org.atsc.notify to every socket subscribed to msgType.
  notify(msgType: string, params: Record<string, unknown> = {}): void {
    let sent = 0
    for (const s of this.sockets.values()) {
      if (!s.open || !s.subscriptions.has(msgType)) continue
      this.send(s.id, { type: 'message', data: JSON.stringify({ jsonrpc: '2.0', method: 'org.atsc.notify', params: { msgType, ...params } }) })
      ++sent
    }
    if (sent) this.deps.log({ t: Date.now(), dir: 'notify', text: `${msgType} ${JSON.stringify(params).slice(0, 160)}` })
  }

  private tick() {
    const v = this.deps.video()
    const state = playbackState(v)
    if (state !== this.lastState) {
      this.lastState = state
      this.notify('rmpPlaybackStateChange', { playbackState: state })
    }
    const rate = v?.playbackRate ?? 1
    if (rate !== this.lastRate) {
      this.lastRate = rate
      this.notify('rmpPlaybackRateChange', { playbackRate: rate })
    }
    const t = v?.currentTime ?? 0
    if (v && t !== this.lastTime) {
      this.lastTime = t
      this.notify('rmpMediaTimeChange', { currentTime: t, playbackState: state })
    }
  }

  private send(id: number, msg: Record<string, unknown>) {
    const win = this.frame.contentWindow
    if (!win) return
    try {
      win.postMessage({ a344host: true, id, ...msg }, window.location.origin)
    } catch {
      // frame navigated away
    }
  }

  private message(ev: MessageEvent) {
    if (ev.source !== this.frame.contentWindow) return
    const m = ev.data as { a344?: string; id?: number; url?: string; data?: string; code?: number; reason?: string; level?: string; text?: string; wsUrl?: string; rev?: string }
    if (!m || typeof m.a344 !== 'string') return
    switch (m.a344) {
      case 'hello':
        this.hello = { wsUrl: m.wsUrl ?? '', rev: m.rev ?? '' }
        this.deps.log({ t: Date.now(), dir: 'info', text: `application running, wsURL ${m.wsUrl} rev ${m.rev}` })
        break
      case 'log':
        this.deps.log({ t: Date.now(), dir: m.level === 'error' ? 'error' : 'info', text: `app: ${m.text ?? ''}` })
        break
      case 'open': {
        const id = m.id ?? 0
        const url = m.url ?? ''
        const path = url.slice(this.deps.wsUrl.length)
        if (path !== '/atscCmd') {   // A/344 8.2.1: optional interfaces not provided answer 404 → connection failure
          this.deps.log({ t: Date.now(), dir: 'error', text: `WebSocket ${url}: interface not provided (only atscCmd)` })
          this.send(id, { type: 'error', reason: 'not found' })
          return
        }
        this.sockets.set(id, { id, url, open: true, subscriptions: new Set() })
        this.deps.log({ t: Date.now(), dir: 'info', text: `atscCmd connection ${id} opened` })
        this.send(id, { type: 'opened', protocol: '' })
        this.deps.onStateChange?.(true)
        break
      }
      case 'send': {
        const s = this.sockets.get(m.id ?? -1)
        if (!s || !s.open) return
        void this.request(s, m.data ?? '')
        break
      }
      case 'close': {
        const s = this.sockets.get(m.id ?? -1)
        if (!s) return
        s.open = false
        this.sockets.delete(s.id)
        this.send(s.id, { type: 'closed', code: m.code ?? 1000, reason: m.reason ?? '' })
        this.deps.log({ t: Date.now(), dir: 'info', text: `atscCmd connection ${s.id} closed by the application` })
        this.deps.onStateChange?.(this.connections > 0)
        break
      }
    }
  }

  private async request(s: Socket, text: string) {
    let parsed: unknown
    try {
      parsed = JSON.parse(text)
    } catch {
      this.deps.log({ t: Date.now(), dir: 'error', text: `unparsable request: ${text.slice(0, 120)}` })
      this.reply(s, { jsonrpc: '2.0', id: null, error: { code: -32700, message: 'Parse error' } })
      return
    }
    if (Array.isArray(parsed)) {   // batch
      const out = []
      for (const r of parsed) out.push(await this.handle(s, r as JsonRpcRequest))
      this.reply(s, out.filter((r) => r !== null))
      return
    }
    const res = await this.handle(s, parsed as JsonRpcRequest)
    if (res !== null) this.reply(s, res)
    this.deps.onStateChange?.(this.connections > 0)   // subscriptions and grants may have changed
  }

  private reply(s: Socket, res: unknown) {
    const data = JSON.stringify(res)
    this.deps.log({ t: Date.now(), dir: 'out', text: data.slice(0, 240) })
    this.send(s.id, { type: 'message', data })
  }

  private async handle(s: Socket, r: JsonRpcRequest): Promise<Record<string, unknown> | null> {
    const id = r.id ?? null
    const method = r.method ?? ''
    this.deps.log({ t: Date.now(), dir: 'in', text: `${method} ${r.params ? JSON.stringify(r.params).slice(0, 160) : ''}` })
    if (r.jsonrpc !== '2.0' || !method) return { jsonrpc: '2.0', id, error: { code: -32600, message: 'Invalid Request' } }
    try {
      const result = await this.dispatch(s, method, r.params ?? {})
      if (id === null) return null   // notification: no response
      return { jsonrpc: '2.0', id, result }
    } catch (e) {
      const err = e instanceof RpcError ? e : new RpcError(-32603, (e as Error).message || 'Internal error')
      if (id === null) {
        this.deps.log({ t: Date.now(), dir: 'error', text: `${method}: ${err.message}` })
        return null
      }
      return { jsonrpc: '2.0', id, error: { code: err.code, message: err.message } }
    }
  }

  private async dispatch(s: Socket, method: string, p: Record<string, unknown>): Promise<unknown> {
    const svc = this.deps.service()
    const v = this.deps.video()
    const strings = (key: string): string[] => {
      const a = p[key]
      return Array.isArray(a) ? a.filter((x): x is string => typeof x === 'string') : []
    }
    switch (method) {
      case 'org.atsc.query.service':
        return { service: svc.globalServiceId ?? null }
      case 'org.atsc.query.baseURI':
        return { baseURI: this.deps.baseUri }
      case 'org.atsc.query.deviceInfo': {
        const deviceInput: Record<string, unknown> = {}
        for (const name of ['ArrowUp', 'ArrowDown', 'ArrowRight', 'ArrowLeft', 'Select', 'Back']) deviceInput[name] = KEY_DEFS[name].keyCode
        deviceInput.BAAppear = { label: 'App', keycode: KEY_DEFS.BAAppear.keyCode }
        for (let d = 0; d <= 9; d++) deviceInput[String(d)] = KEY_DEFS[String(d)].keyCode
        return {
          deviceMake: 'ATSC 3.0 stack',
          deviceModel: 'web frontend',
          deviceInput,
          deviceInfo: { numberOfTuners: 1, userAgent: navigator.userAgent },
          deviceSupportedWebSocketAPIs: SUPPORTED_METHODS,
        }
      }
      case 'org.atsc.query.cc':
        return { ccEnabled: this.deps.ccEnabled() }
      case 'org.atsc.query.alerting': {
        const types = strings('alertingTypes')
        const list = await this.deps.alerting()
        return { alertList: types.length ? list.filter((a) => types.includes(a.alertingType)) : list }
      }
      case 'org.atsc.query.languages':
        return { preferredAudioLang: 'en', preferredUiLang: 'en', preferredCaptionSubtitleLang: 'en' }
      case 'org.atsc.query.captionDisplay':
        return {}
      case 'org.atsc.query.ratingLevel':
        return { blocked: false }
      case 'org.atsc.query.audioAccessibilityPref':
        return {}
      case 'org.atsc.query.signaling':
        return { signaling: await this.deps.signaling(strings('names')) }
      case 'org.atsc.query.serviceGuideUrls':
        return { urlList: [] }
      case 'org.atsc.request.keys':
        return { accepted: this.deps.keys.request(strings('keys')) }
      case 'org.atsc.relinquish.keys':
        this.deps.keys.relinquish(strings('keys'))
        return {}
      case 'org.atsc.subscribe': {
        const want = strings('msgType')
        const all = want.includes('All')
        for (const m of NOTIFICATIONS) if (all || want.includes(m)) s.subscriptions.add(m)
        return { msgType: [...s.subscriptions] }
      }
      case 'org.atsc.unsubscribe': {
        const want = strings('msgType')
        if (want.includes('All')) s.subscriptions.clear()
        else for (const m of want) s.subscriptions.delete(m)
        return { msgType: [...s.subscriptions] }
      }
      case 'org.atsc.scale-position': {
        const scale = Number(p.scaleFactor), x = Number(p.xPos), y = Number(p.yPos)
        if (!(scale >= 10 && scale <= 100) || !(x >= 0 && x <= 100 - scale + 1e-6) || !(y >= 0 && y <= 100 - scale + 1e-6)) throw new RpcError(-32602, 'Invalid params')
        if (!this.deps.scalePosition(scale, x, y)) throw new RpcError(-8, 'Video scaling/position failed')
        return {}
      }
      case 'org.atsc.setRMPURL': {
        const op = typeof p.operation === 'string' ? p.operation : ''
        const url = typeof p.rmpurl === 'string' ? p.rmpurl : undefined
        if (op === 'startRmp' && !url) throw new RpcError(-15, 'Illegal URL Format')
        const code = this.deps.setRmpUrl(op, url)
        if (code !== null) throw new RpcError(code, rmpErrorMessage(code))
        return {}
      }
      case 'org.atsc.query.rmpMediaTime': {
        const out: Record<string, unknown> = { currentTime: v?.currentTime ?? 0 }
        const start = (v as (HTMLVideoElement & { getStartDate?: () => Date }) | null)?.getStartDate?.()
        if (start instanceof Date && !Number.isNaN(start.getTime())) out.startDate = start.toISOString()
        return out
      }
      case 'org.atsc.query.rmpPlaybackState':
        return { playbackState: playbackState(v) }
      case 'org.atsc.query.rmpPlaybackRate':
        return { playbackRate: v?.playbackRate ?? 1 }
      default:
        throw new RpcError(-32601, 'Method not found')
    }
  }
}

function rmpErrorMessage(code: number): string {
  switch (code) {
    case -11: return 'The indicated MPD cannot be accessed'
    case -12: return 'The content cannot be played'
    case -15: return 'Illegal URL Format'
    case -21: return 'Changing RMP playback from the current source is not supported'
    case -32601: return 'Method not found'
    default: return 'Request failed'
  }
}
