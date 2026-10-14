// MSE push player for MMTP services (spec 5.17.4 "push player" and its HTTP contract): polls the daemon's /media/{bsid}/{sid}/mmt/index.json?since=<seq>, picks one
// video and one audio component the browser can decode, appends their MSE-clean init segment and the styp+moof+mdat
// fragments to one MediaSource in strict seq order, and runs the start gate, autoplay handling, live-edge catch-up,
// gap jumper, buffer trimming and the origin-restart handling a live push stream needs.
//
// Plain TypeScript with no Vue imports: PlayerPanel.vue wraps it, and the mock-origin test page bundles it standalone
// with esbuild. Everything the panel shows comes from the state object handed to the `onState` callback.
//
// MSE facts this code is built on (verified in Firefox and Chrome): bare 4CC codec strings are refused;
// `timestampOffset` in 'segments' mode is added verbatim and may only be set while !updating; play() never starts
// unless currentTime is moved to the buffered start; Chrome never jumps a buffered gap >= 25 ms and Firefox none
// >= 0.75 s; a SourceBuffer is dead after its 'error' event; re-appending a fragment can punch a hole.
//
// Origin facts (service::Engine): every MP-table asset is listed with init:null, codecs "" and mime "" until its MPU
// metadata arrived (the codec verdict is pending then, never "unsupported"); an init gets a new seq only when the
// metadata bytes changed; `generation` changes when the origin restarted its timeline (capture loop wrap, encoder
// restart, source reopen) and seqs may restart with it, which the player answers with a full MediaSource rebuild.

import type { MmtComponent, MmtIndex, MmtInit, PushPlayerSettings } from '../api/types'

export interface PushComponentState {
  packetId: number
  kind: string
  codecs: string
  mime: string                 // full MSE type string, mime; codecs="..."
  supported: boolean | null    // null: verdict pending (no init listed yet) or not probed (captions and data are listed only)
  detail: string               // how the verdict was reached / why the component is not appended
  chosen: boolean
  appended: number             // fragments appended to the SourceBuffer
  bytes: number                // bytes appended (init + fragments)
  bufferAheadS: number         // buffered seconds ahead of currentTime in this SourceBuffer
  lastMpu: number | null       // MPU sequence number of the last appended fragment
  damaged: number              // origin counters (index.json components[])
  dropped: number
  damagedAppended: number      // fragments appended with zero-filled samples
  missing: number              // fragments the origin no longer had (HTTP 404) or that failed to download, skipped
  queued: number               // fragments known but not yet appended
  initSeq: number | null
}

export interface PushPlayerState {
  status: string
  error: string
  components: PushComponentState[]
  currentTime: number
  liveEdgeS: number
  latencyS: number             // liveEdgeS - currentTime
  droppedFrames: number
  totalFrames: number
  width: number
  height: number
  muted: boolean
  needsGesture: boolean        // even muted autoplay was refused: the panel shows the click-to-play overlay
  mutedByPolicy: boolean       // started with sound, the browser refused, retried muted
  playing: boolean
  started: boolean             // the start gate passed and play() was called
  buffered: [number, number][] // video.buffered (intersection of the SourceBuffers)
  readyState: number
  gapJumps: number
  catchups: number
  trims: number
  polls: number
  pollErrors: number
  lastPollMs: number
  nextSeq: number
  cachedBytes: number
  cacheLimit: number
  evicted: number
  selected: boolean
  generation: number           // index.json `generation` the current MediaSource was built from (0 with an older daemon)
  restarts: number             // MediaSource rebuilds after the origin restarted its timeline (generation change, seq reset)
  lastRestart: string          // why the last rebuild happened
}

export interface PushPlayerOptions {
  onState?: (s: PushPlayerState) => void
  pollMs?: number              // index poll period (DESIGN: 500 ms)
  statsMs?: number             // stats callback period (DESIGN: 1 s)
}

interface QueueItem {
  seq: number
  init: boolean
  url: string
  mpu: number | null
  bytes: number
  damaged: boolean
}

interface Track {
  comp: MmtComponent
  state: PushComponentState
  verdictFor: string           // kind + type string the current supported/detail verdict was computed for ('' = none yet)
  sb: SourceBuffer | null
  queue: QueueItem[]           // a queued init first, then fragments by seq; never a fragment seq <= lastAppendedSeq
  highestQueued: number        // highest fragment seq ever queued (dedupe across polls)
  lastAppendedSeq: number      // highest seq appended or skipped: never re-append a seq
  initQueuedSeq: number | null
  fetching: boolean
  ready: { item: QueueItem; data: ArrayBuffer } | null
  inFlight: boolean            // appendBuffer()/remove() issued, waiting for updateend
  pending: QueueItem | null    // the item whose append is in flight
  offsetSet: boolean
  quotaRetried: boolean
  retries: number
}

const SUPPORT_CACHE = new Map<string, Promise<{ supported: boolean; detail: string }>>()

/// Full MSE type string of a component (`video/mp4; codecs="hev1.2.4.L120.B0"`).
function typeOf(c: MmtComponent): string {
  return `${c.mime ?? ''}; codecs="${c.codecs ?? ''}"`
}

/// Whether the origin has the component's MPU metadata: before that it lists init:null with empty codecs and mime, and
/// nothing can be probed or appended.
function hasInit(c: MmtComponent): boolean {
  // the offset is frozen by the origin at the component's first fragment: before that the init must not be appended,
  // because the offset can only be applied once and before any timed data
  return !!c.init && !!c.codecs && !!c.mime && typeof c.timestampOffsetS === 'number'
}

/// Queue order: a queued init first (fragments decode only after it), then fragments by seq.
function queueOrder(a: QueueItem, b: QueueItem): number {
  return Number(b.init) - Number(a.init) || a.seq - b.seq
}

/// MediaSource.isTypeSupported plus navigator.mediaCapabilities.decodingInfo (type 'media-source') when available;
/// the result is cached per full type string. Mirrors util/capabilities.ts without its Vue dependency.
export function probeSupport(comp: MmtComponent): Promise<{ supported: boolean; detail: string }> {
  const type = typeOf(comp)
  const cached = SUPPORT_CACHE.get(type)
  if (cached) return cached
  const p = (async () => {
    if (!comp.codecs) return { supported: false, detail: 'no codecs string in the init segment' }
    if (typeof MediaSource === 'undefined') return { supported: false, detail: 'MediaSource API unavailable' }
    const typeOk = MediaSource.isTypeSupported(type)
    const parts = [`isTypeSupported(${type}): ${typeOk}`]
    let supported = typeOk
    const mc = navigator.mediaCapabilities
    if (typeOk && mc && typeof mc.decodingInfo === 'function') {
      try {
        const config: MediaDecodingConfiguration =
          comp.kind === 'video'
            ? { type: 'media-source', video: { contentType: type, width: comp.width ?? 1920, height: comp.height ?? 1080, bitrate: 5_000_000, framerate: 30 } }
            : { type: 'media-source', audio: { contentType: type, channels: String(comp.channels ?? 2), samplerate: comp.sampleRate ?? 48000, bitrate: 128_000 } }
        const info = await mc.decodingInfo(config)
        supported = info.supported
        parts.push(`decodingInfo: supported=${info.supported} smooth=${info.smooth} powerEfficient=${info.powerEfficient}`)
      } catch (e) {
        parts.push(`decodingInfo threw (${(e as Error).message}); keeping isTypeSupported`)
      }
    }
    return { supported, detail: parts.join(' · ') }
  })()
  SUPPORT_CACHE.set(type, p)
  return p
}

function hasUserActivation(): boolean {
  return (navigator as Navigator & { userActivation?: { hasBeenActive: boolean } }).userActivation?.hasBeenActive ?? false
}

function ranges(r: TimeRanges): [number, number][] {
  const out: [number, number][] = []
  for (let i = 0; i < r.length; i++) out.push([r.start(i), r.end(i)])
  return out
}

export class MsePushPlayer {
  readonly video: HTMLVideoElement
  readonly settings: PushPlayerSettings
  private readonly onState?: (s: PushPlayerState) => void
  private readonly pollMs: number
  private readonly statsMs: number

  private ms: MediaSource | null = null
  private objectUrl = ''
  private tracks = new Map<number, Track>()
  private order: number[] = []           // packetIds in index order
  private since = 0
  private stopped = true
  private started = false
  private chosenDone = false
  private firstReadyAt = 0
  private pollTimer = 0
  private statsTimer = 0
  private stallTimer = 0
  private abort: AbortController | null = null
  private listeners: [EventTarget, string, EventListener][] = []
  private startAt = 0
  private epoch = 0                      // bumped by every teardown: in-flight polls, probes and fetches of the old MediaSource return quietly
  private generation = -1                // index.json generation of the current MediaSource; -1 until the first index arrived
  private lastNextSeq = -1               // nextSeq of the previous index of this MediaSource; -1 until the first index arrived

  private st: PushPlayerState = {
    status: 'idle', error: '', components: [], currentTime: 0, liveEdgeS: 0, latencyS: 0, droppedFrames: 0, totalFrames: 0,
    width: 0, height: 0, muted: true, needsGesture: false, mutedByPolicy: false, playing: false, started: false, buffered: [],
    readyState: 0, gapJumps: 0, catchups: 0, trims: 0, polls: 0, pollErrors: 0, lastPollMs: 0, nextSeq: 0, cachedBytes: 0,
    cacheLimit: 0, evicted: 0, selected: true, generation: 0, restarts: 0, lastRestart: '',
  }

  constructor(video: HTMLVideoElement, settings: PushPlayerSettings, opts: PushPlayerOptions = {}) {
    this.video = video
    this.settings = settings
    this.onState = opts.onState
    this.pollMs = opts.pollMs ?? 500
    this.statsMs = opts.statsMs ?? 1000
  }

  get state(): PushPlayerState {
    return this.st
  }

  // ---------------------------------------------------------------- lifecycle

  start(): void {
    if (!this.stopped) return
    this.generation = -1
    this.st = { ...this.st, error: '', gapJumps: 0, catchups: 0, trims: 0, polls: 0, pollErrors: 0, generation: 0, restarts: 0, lastRestart: '' }
    this.begin('opening MediaSource')
  }

  stop(): void {
    if (this.stopped) return
    this.teardown()
    if (this.st.status !== 'error') this.setStatus('stopped')
    this.emit()
  }

  /// Click-to-play from the overlay (user gesture): the browser refused even muted autoplay.
  play(): Promise<void> {
    this.st.needsGesture = false
    this.emit()
    return this.video.play().then(() => { this.setStatus('playing') }).catch((e: Error) => { this.st.needsGesture = true; this.setStatus('play refused: ' + e.message) })
  }

  setMuted(m: boolean): void {
    this.video.muted = m
    this.st.muted = m
    this.st.mutedByPolicy = false
    this.emit()
  }

  /// Builds the MediaSource, the listeners, the timers and a fresh fetch state (since=0, no tracks): from start(), and
  /// from restart() after the origin restarted its timeline.
  private begin(status: string): void {
    this.stopped = false
    this.started = false
    this.chosenDone = false
    this.firstReadyAt = 0
    this.since = 0
    this.lastNextSeq = -1
    this.tracks.clear()
    this.order = []
    this.st = { ...this.st, status, components: [], playing: false, started: false, needsGesture: false, mutedByPolicy: false }
    this.abort = new AbortController()
    const ms = new MediaSource()
    this.ms = ms
    this.listen(ms, 'sourceopen', () => {
      // Live stream: an infinite duration keeps the element from reaching 'ended' when the playhead touches the end of
      // the buffered data; the init segment's mehd only sets the duration while it is NaN (MSE "initialization segment
      // received", step 1), so it has to be set before the first append.
      try { ms.duration = Infinity } catch { /* set again after the first updateend */ }
      this.setStatus('polling index')
      void this.poll()
    })
    this.listen(ms, 'sourceended', () => { if (!this.stopped) this.fail('MediaSource ended') })
    this.listen(ms, 'sourceclose', () => { if (!this.stopped) this.fail('MediaSource closed') })
    const v = this.video
    this.listen(v, 'playing', () => { this.st.playing = true; this.st.needsGesture = false; this.st.muted = v.muted; this.setStatus('playing') })
    this.listen(v, 'pause', () => { this.st.playing = false; if (this.started && !this.stopped && this.st.status !== 'error' && !this.st.needsGesture) this.setStatus('paused') })
    this.listen(v, 'waiting', () => { if (this.started && !this.st.needsGesture) { this.setStatus('buffering'); this.jumpGap() } })
    // The start gate may pass while the element still has no metadata (readyState 0): the initial seek is then lost
    // and repeated once the metadata is there. Registered through listen() so a teardown removes it.
    this.listen(v, 'loadedmetadata', () => { if (this.started && v.currentTime < this.startAt - 0.5) this.seekTo(this.startAt) })
    this.listen(v, 'volumechange', () => { this.st.muted = v.muted; this.emit() })
    this.listen(v, 'error', () => { this.fail(this.mediaError() || 'media element error') })
    this.objectUrl = URL.createObjectURL(ms)
    v.src = this.objectUrl
    this.statsTimer = window.setInterval(() => this.tick(), this.statsMs)
    this.stallTimer = window.setInterval(() => { if (this.started && !this.video.paused && this.video.readyState <= 2) this.jumpGap() }, 500)
    this.emit()
  }

  /// Tears down the MediaSource, the SourceBuffers, the timers, the listeners and the fetch state. Everything async that
  /// is still running (polls, probes, fragment fetches) sees the epoch change and returns without touching the player.
  private teardown(): void {
    this.stopped = true
    this.epoch += 1
    this.abort?.abort()
    this.abort = null
    window.clearTimeout(this.pollTimer)
    window.clearInterval(this.statsTimer)
    window.clearInterval(this.stallTimer)
    for (const [t, n, l] of this.listeners) t.removeEventListener(n, l)
    this.listeners = []
    const ms = this.ms
    if (ms && ms.readyState === 'open') {
      for (const t of this.tracks.values()) {
        try { if (t.sb && t.sb.updating) t.sb.abort() } catch { /* already detached */ }
      }
    }
    try { this.video.pause() } catch { /* ignore */ }
    this.video.removeAttribute('src')
    try { this.video.load() } catch { /* ignore */ }
    if (this.objectUrl) URL.revokeObjectURL(this.objectUrl)
    this.objectUrl = ''
    this.ms = null
    for (const t of this.tracks.values()) { t.sb = null; t.ready = null; t.queue = []; t.pending = null; t.inFlight = false }
    this.st.playing = false
  }

  /// Origin restart (generation change or seq reset): the cached timeline the MediaSource was built from is gone, so
  /// everything is rebuilt from since=0 (re-choose, re-append the inits, start gate again). Counters stay cumulative.
  private restart(reason: string, generation: number): void {
    if (this.stopped) return
    this.teardown()
    // The tracks are rebuilt from the fresh index: their pipeline state (SourceBuffer offset applied, highest queued /
    // appended seq, queued init) belongs to the MediaSource that was just torn down. Cumulative counters stay in `st`.
    this.tracks.clear()
    this.chosenDone = false
    this.since = 0
    this.generation = generation
    this.st.generation = generation
    this.st.restarts += 1
    this.st.lastRestart = reason
    this.begin(reason + ': rebuilding the MediaSource')
  }

  // ---------------------------------------------------------------- index polling

  private async poll(): Promise<void> {
    if (this.stopped) return
    const epoch = this.epoch
    const since = this.since
    const url = this.settings.indexUrl + (this.settings.indexUrl.includes('?') ? '&' : '?') + 'since=' + since
    let delay = this.pollMs
    try {
      const r = await fetch(url, { cache: 'no-store', signal: this.abort?.signal })
      if (epoch !== this.epoch) return
      this.st.polls += 1
      this.st.lastPollMs = Date.now()
      if (r.status === 404) {
        if (!this.started) this.setStatus('waiting for the index (HTTP 404: no MPU metadata yet)')
        delay = 1000
      } else if (!r.ok) {
        throw new Error(`index: HTTP ${r.status}`)
      } else {
        const idx = (await r.json()) as MmtIndex
        if (epoch !== this.epoch) return
        if (this.originRestarted(idx, since)) return   // restart() runs its own poll loop on the new MediaSource
        await this.handleIndex(idx, epoch)
      }
    } catch (e) {
      if (epoch !== this.epoch) return                 // stop()/restart() aborted the fetch
      this.st.pollErrors += 1
      if (!this.started) this.setStatus('index poll failed: ' + (e as Error).message)
      delay = 1500
    }
    if (epoch === this.epoch && !this.stopped) this.pollTimer = window.setTimeout(() => void this.poll(), delay)
  }

  /// A different `generation` (capture loop wrap, encoder restart, source reopen; an older daemon without the field
  /// counts as 0) or a nextSeq below the since we sent (seqs restarted) means the origin restarted its timeline.
  /// The seq rule also requires the origin's own counter to have gone backwards (nextSeq below the previous index's),
  /// which is the same thing for a consistent origin (since never exceeds its nextSeq) but keeps an inconsistent index
  /// (an init seq above nextSeq) from triggering a rebuild at every poll.
  private originRestarted(idx: MmtIndex, since: number): boolean {
    const gen = idx.generation ?? 0
    if (this.generation < 0) {
      this.generation = gen
      this.st.generation = gen
    } else if (gen !== this.generation) {
      this.restart(`origin restarted (generation ${this.generation} -> ${gen})`, gen)
      return true
    }
    const next = idx.nextSeq ?? 0
    const prev = this.lastNextSeq
    this.lastNextSeq = next
    if (since > 0 && next < since && prev >= 0 && next < prev) {
      this.restart(`origin restarted (nextSeq ${next} below since ${since})`, gen)
      return true
    }
    return false
  }

  private async handleIndex(idx: MmtIndex, epoch: number): Promise<void> {
    this.st.liveEdgeS = idx.liveEdgeS ?? 0
    this.st.nextSeq = idx.nextSeq ?? 0
    this.st.cachedBytes = idx.cachedBytes ?? 0
    this.st.cacheLimit = idx.cacheLimit ?? 0
    this.st.evicted = idx.evicted ?? 0
    this.st.selected = idx.selected !== false
    for (const c of idx.components ?? []) {
      let t = this.tracks.get(c.packetId)
      if (!t) {
        t = this.newTrack(c)
        this.tracks.set(c.packetId, t)
        this.order.push(c.packetId)
      }
      t.comp = c
      t.state.kind = c.kind ?? ''
      t.state.codecs = c.codecs ?? ''
      t.state.mime = typeOf(c)
      t.state.damaged = c.damaged ?? 0
      t.state.dropped = c.dropped ?? 0
      if (c.init) this.since = Math.max(this.since, c.init.seq)
      // A chosen component whose init changed (new init seq): queue the new init ahead of the fragments that need it.
      if (t.sb && c.init && t.initQueuedSeq !== c.init.seq) this.queueInit(t, c.init)
    }
    for (const f of idx.fragments ?? []) {
      this.since = Math.max(this.since, f.seq)
      const t = this.tracks.get(f.packetId)
      if (!t) continue   // fragment of a component the index does not describe: nothing to append it to
      if (f.seq <= t.highestQueued) continue
      t.highestQueued = f.seq
      if (!t.sb) {
        if (this.chosenDone) continue                 // not chosen: nothing to append
        if (t.queue.length >= 64) t.queue.shift()     // not chosen yet: keep a bounded window for choose()
      }
      t.queue.push({ seq: f.seq, init: false, url: f.url, mpu: f.mpu, bytes: f.bytes, damaged: !!f.damaged })
    }
    for (const t of this.tracks.values()) if (t.queue.length > 1) t.queue.sort(queueOrder)
    await this.probeAll()
    if (epoch !== this.epoch) return
    if (!this.chosenDone) this.choose()
    for (const t of this.tracks.values()) this.pump(t)
    this.updateWaitingStatus()
    this.emit()
  }

  private newTrack(c: MmtComponent): Track {
    return {
      comp: c,
      state: {
        packetId: c.packetId, kind: c.kind ?? '', codecs: c.codecs ?? '', mime: typeOf(c), supported: null, detail: '',
        chosen: false, appended: 0, bytes: 0, bufferAheadS: 0, lastMpu: null, damaged: c.damaged ?? 0, dropped: c.dropped ?? 0,
        damagedAppended: 0, missing: 0, queued: 0, initSeq: null,
      },
      verdictFor: '', sb: null, queue: [], highestQueued: 0, lastAppendedSeq: 0, initQueuedSeq: null, fetching: false, ready: null,
      inFlight: false, pending: null, offsetSet: false, quotaRetried: false, retries: 0,
    }
  }

  /// Codec verdict of every video/audio component, keyed to its kind and full type string: a component listed before
  /// its init (init null, codecs "" or mime "") is pending, never unsupported, and gets probed once the metadata is
  /// there; a changed type or kind resets the verdict. Captions and data are listed only.
  private async probeAll(): Promise<void> {
    const todo: Promise<void>[] = []
    for (const t of this.tracks.values()) {
      const key = `${t.state.kind}|${t.state.mime}`
      if (t.verdictFor === key) continue
      t.verdictFor = key
      if (t.sb) {
        t.state.detail += ` · listed as ${t.state.kind} ${t.state.mime} after the SourceBuffer was added`
        continue
      }
      t.state.supported = null
      if (t.state.kind !== 'video' && t.state.kind !== 'audio') {
        t.state.detail = t.state.kind === 'subtitles' ? 'caption component: not appended (no MSE text tracks), listed only' : 'not a media component'
        continue
      }
      if (!hasInit(t.comp)) {
        // The origin lists the init before the component's offset is frozen (its first fragment): the kind and type
        // will not change when the offset appears, so forget the key and look again at the next poll.
        t.verdictFor = ''
        t.state.detail = 'waiting for the MPU metadata (no init segment listed yet): codec verdict pending'
        continue
      }
      t.state.detail = 'probing ' + t.state.mime
      todo.push(probeSupport(t.comp).then((r) => {
        if (t.verdictFor !== key) return               // the type changed again while probing: a newer probe is on its way
        t.state.supported = r.supported
        t.state.detail = r.detail
      }))
    }
    if (todo.length) {
      if (!this.started) this.setStatus('probing codecs')
      await Promise.all(todo)
    }
  }

  private av(): Track[] {
    return this.order.map((p) => this.tracks.get(p)!).filter((t) => t.state.kind === 'video' || t.state.kind === 'audio')
  }

  /// One video and one audio component (prefer defaultAsset, then index order) whose type the browser accepts, chosen
  /// once all listed A/V components have an init and a verdict, or 3 s after the first ready one (an asset whose
  /// packets never arrive must not block the start). Nothing chosen: keep trying at every poll.
  private choose(): void {
    const ms = this.ms
    if (!ms || ms.readyState !== 'open' || this.started) return
    const av = this.av()
    const ready = av.filter((t) => hasInit(t.comp) && t.state.supported !== null)
    if (!ready.length) return
    if (!this.firstReadyAt) this.firstReadyAt = Date.now()
    const allReady = ready.length === av.length
    if (!allReady && Date.now() - this.firstReadyAt < 3000) return
    for (const kind of ['video', 'audio']) {
      if (av.some((t) => t.sb && t.state.kind === kind)) continue
      const cands = ready.filter((t) => t.state.kind === kind && t.state.supported === true && !t.sb)
      cands.sort((a, b) => Number(!!b.comp.defaultAsset) - Number(!!a.comp.defaultAsset))
      for (const t of cands) {
        try {
          const sb = ms.addSourceBuffer(t.state.mime)
          sb.mode = 'segments'
          t.sb = sb
          t.state.chosen = true
          t.state.detail += ' · SourceBuffer added'
          this.listen(sb, 'updateend', () => this.onUpdateEnd(t))
          this.listen(sb, 'error', () => this.fail(`SourceBuffer error on ${t.state.kind} packet ${t.comp.packetId}: ${this.mediaError() || 'append failed'}`))
          this.setOffset(t)
          this.queueInit(t, t.comp.init!)
          break
        } catch (e) {
          t.state.supported = false
          t.state.detail += ` · addSourceBuffer failed: ${(e as Error).message}`
        }
      }
    }
    if (!av.some((t) => t.sb)) return                 // keep trying: the index may list more components or their inits later
    for (const t of av) if (!t.sb && t.state.supported === true) t.state.detail += ' · another component of this kind was chosen'
    this.chosenDone = true
  }

  private setOffset(t: Track): boolean {
    if (!t.sb || t.offsetSet) return true
    try {
      if (t.sb.updating) return false
      if (typeof t.comp.timestampOffsetS !== 'number') return false   // not frozen by the origin yet
      t.sb.timestampOffset = t.comp.timestampOffsetS
      t.offsetSet = true
      return true
    } catch {
      return false   // InvalidStateError while updating: retried from the next pump
    }
  }

  /// The init always heads the per-track queue (fragments decode only after it). On a track that has not started yet
  /// (no init appended) the queued fragments with a seq below the init's are dropped: the origin produced them under a
  /// previous init it no longer lists (the daemon assigns a new init seq only when the MPU metadata changed).
  private queueInit(t: Track, init: MmtInit): void {
    t.initQueuedSeq = init.seq
    const started = t.state.initSeq !== null
    const rest: QueueItem[] = []
    let dropped = 0
    for (const q of t.queue) {
      if (q.init) continue                            // at most one queued init: the newest
      if (!started && q.seq < init.seq) { dropped += 1; continue }
      rest.push(q)
    }
    if (dropped) {
      t.lastAppendedSeq = Math.max(t.lastAppendedSeq, init.seq)
      t.state.detail += ` · ${dropped} cached fragment(s) older than init seq ${init.seq} skipped`
    }
    t.queue = [{ seq: init.seq, init: true, url: init.url, mpu: null, bytes: init.bytes, damaged: false }, ...rest]
    t.state.queued = t.queue.length + (t.ready ? 1 : 0)
  }

  // ---------------------------------------------------------------- append pipeline (one fetch ahead, one append in flight)

  private pump(t: Track): void {
    if (this.stopped || !t.sb) return
    if (!t.fetching && !t.ready && t.queue.length) void this.fetchNext(t)
    if (t.ready && !t.inFlight && !t.sb.updating) this.append(t)
    t.state.queued = t.queue.length + (t.ready ? 1 : 0)
  }

  private async fetchNext(t: Track): Promise<void> {
    t.fetching = true
    const epoch = this.epoch
    try {
      while (epoch === this.epoch && t.sb && t.queue.length) {
        const item = t.queue[0]
        if (!item.init && item.seq <= t.lastAppendedSeq) { this.dequeue(t, item); continue }   // never re-append a seq
        let data: ArrayBuffer
        try {
          const r = await fetch(item.url, { signal: this.abort?.signal })
          if (epoch !== this.epoch) return
          if (r.status === 404) {
            // evicted or never completed at the origin: skip and count, the gap jumper crosses the hole
            this.skip(t, item, 'HTTP 404')
            continue
          }
          if (!r.ok) throw new Error(`HTTP ${r.status}`)
          data = await r.arrayBuffer()                // inside the try: an abort from teardown() ends here quietly, a
        } catch (e) {                                 // truncated body goes through the retries like a failed request
          if (epoch !== this.epoch) return
          if (++t.retries <= 2) { await new Promise((res) => setTimeout(res, 300)); continue }
          t.retries = 0
          this.skip(t, item, (e as Error).message || 'download failed')
          continue
        }
        if (epoch !== this.epoch) return
        t.retries = 0
        this.dequeue(t, item)
        t.ready = { item, data }
        break
      }
    } finally {
      t.fetching = false
    }
    if (epoch === this.epoch) this.pump(t)
  }

  private dequeue(t: Track, item: QueueItem): void {
    const i = t.queue.indexOf(item)
    if (i >= 0) t.queue.splice(i, 1)
  }

  /// A fragment the origin no longer has (404) or that failed to download after the retries is skipped and counted;
  /// the gap jumper crosses the hole. A failed init leaves nothing to decode its fragments with: they are dropped too
  /// and the init is queued again when the next poll still lists it.
  private skip(t: Track, item: QueueItem, why: string): void {
    this.dequeue(t, item)
    t.state.missing += 1
    t.lastAppendedSeq = Math.max(t.lastAppendedSeq, item.seq)
    if (item.init) {
      t.state.missing += t.queue.length
      t.queue = []
      t.initQueuedSeq = null
      t.state.detail += ` · init seq ${item.seq} unavailable (${why}), waiting for the origin to list it again`
    }
  }

  private append(t: Track): void {
    const sb = t.sb
    if (!sb || !t.ready || t.inFlight || sb.updating) return
    if (!this.setOffset(t)) return
    const { item, data } = t.ready
    try {
      sb.appendBuffer(data)
    } catch (e) {
      const err = e as DOMException
      if (err.name === 'QuotaExceededError' && !t.quotaRetried) {
        // Full buffer: drop everything older than 10 s behind the playhead, then retry this append once.
        t.quotaRetried = true
        const cut = this.video.currentTime - 10
        if (cut > 0 && sb.buffered.length && sb.buffered.start(0) < cut) {
          try { t.inFlight = true; sb.remove(0, cut); this.st.trims += 1; return } catch { t.inFlight = false }
        }
      }
      if (err.name === 'QuotaExceededError') {
        t.ready = null; t.state.missing += 1; t.lastAppendedSeq = Math.max(t.lastAppendedSeq, item.seq); t.quotaRetried = false
        this.pump(t)
        return
      }
      this.fail(`appendBuffer failed on ${t.state.kind}: ${err.message || err.name}`)
      return
    }
    t.ready = null
    t.inFlight = true
    t.pending = item
  }

  private onUpdateEnd(t: Track): void {
    t.inFlight = false
    const item = t.pending
    t.pending = null
    if (item) {
      t.quotaRetried = false
      t.state.bytes += item.bytes || 0
      if (item.init) {
        t.state.initSeq = item.seq
      } else {
        t.state.appended += 1
        t.state.lastMpu = item.mpu
        t.lastAppendedSeq = Math.max(t.lastAppendedSeq, item.seq)
        if (item.damaged) t.state.damagedAppended += 1
      }
    }
    if (this.stopped) return
    const ms = this.ms
    if (ms && ms.readyState === 'open' && ms.duration !== Infinity && ![...this.tracks.values()].some((x) => x.sb?.updating)) {
      try { ms.duration = Infinity } catch { /* next time */ }
    }
    this.maybeStart()
    this.pump(t)
    this.updateWaitingStatus()
  }

  // ---------------------------------------------------------------- start gate and autoplay

  private chosen(): Track[] {
    return this.order.map((p) => this.tracks.get(p)!).filter((t) => t.sb)
  }

  private updateWaitingStatus(): void {
    if (this.started || this.stopped || this.st.status === 'error') return
    const ch = this.chosen()
    if (ch.length) {
      if (!this.st.selected) { this.setStatus('service not selected at the origin'); return }
      this.setStatus('waiting for fragments (' + ch.map((t) => `${t.state.kind} ${t.state.appended}/${this.settings.minFragments}`).join(', ') + ')')
      return
    }
    const av = this.av()
    const pending = av.filter((t) => t.state.supported === null)
    if (pending.length) {
      this.setStatus('waiting for the MPU metadata of ' + pending.map((t) => `${t.state.kind} ${t.comp.packetId}`).join(', '))
    } else if (av.length) {
      this.setStatus('no playable component: ' + av.map((t) => `${t.state.kind} ${t.state.codecs || '?'}`).join(', '))
    } else if (this.st.polls) {
      this.setStatus('waiting for components (the index lists none)')
    }
  }

  private maybeStart(): void {
    if (this.started || this.stopped) return
    const ch = this.chosen()
    if (!ch.length || !this.chosenDone) return
    const min = Math.max(1, this.settings.minFragments || 1)
    if (!ch.every((t) => t.state.appended >= min)) return
    let startAt = -Infinity
    for (const t of ch) {
      const b = t.sb!.buffered
      if (!b.length) return
      startAt = Math.max(startAt, b.start(0))
    }
    this.started = true
    this.st.started = true
    this.startAt = startAt + 0.05
    this.seekTo(this.startAt)   // repeated from 'loadedmetadata' when the element had no metadata yet
    this.setStatus('starting')
    this.attemptPlay()
  }

  private seekTo(t: number): void {
    try { this.video.currentTime = t } catch { /* not seekable yet */ }
  }

  /// Autoplay policy, same as the dash path: sound when the page has user activation, else muted; a refused play()
  /// with sound is retried muted; a refused muted play() needs a click on the overlay.
  private attemptPlay(): void {
    const v = this.video
    v.muted = !hasUserActivation()
    this.st.muted = v.muted
    this.st.needsGesture = false
    this.st.mutedByPolicy = false
    v.play().then(() => { this.setStatus('playing') }).catch((e: Error) => {
      if (this.stopped) return
      if (!v.muted) {
        v.muted = true
        this.st.muted = true
        this.st.mutedByPolicy = true
        this.setStatus('autoplay refused with sound, retrying muted')
        v.play().then(() => { this.setStatus('playing') }).catch(() => { this.st.needsGesture = true; this.setStatus('autoplay blocked by the browser') })
      } else {
        this.st.needsGesture = true
        this.setStatus('autoplay blocked by the browser (' + e.name + ')')
      }
    })
  }

  // ---------------------------------------------------------------- live edge, gaps, trimming

  /// Gap jumper: when the playhead sits within 0.25 s of the end of its buffered range (or in a hole) and a later
  /// range exists, move it to the nearest later range's start + 0.01 whatever the size of the hole: a live push stream
  /// never back-fills, so waiting at a hole means waiting forever (Chrome never crosses a gap >= 25 ms by itself).
  private jumpGap(): void {
    if (!this.started || this.stopped || this.video.seeking) return
    const v = this.video
    const b = v.buffered
    const t = v.currentTime
    for (let i = 0; i < b.length; i++) {
      if (b.start(i) <= t && t <= b.end(i) + 0.25) {
        if (t < b.end(i) - 0.25) return   // plenty ahead in this range: a decoder stall, not a gap
        break
      }
    }
    for (let i = 0; i < b.length; i++) {
      if (b.start(i) > t) {
        this.st.gapJumps += 1
        this.seekTo(b.start(i) + 0.01)
        return
      }
    }
  }

  /// Live-edge catch-up: more than liveDelay + 4 s of buffered data beyond the playhead, measured against the overall
  /// buffered end, means the player fell behind the live edge (tuned in with a full cache, paused tab, slow decoder)
  /// or is stuck at the end of a range while newer data keeps arriving: jump to bufferedEnd - liveDelay, landing in a
  /// buffered range (the start of the next range when that point falls into a hole).
  private catchUp(): void {
    if (!this.started || this.stopped || this.video.paused || this.video.seeking) return
    const v = this.video
    const b = v.buffered
    if (!b.length) return
    const t = v.currentTime
    const end = b.end(b.length - 1)
    const d = this.settings.liveDelay ?? 3
    if (t >= end - (d + 4)) return
    let target = end - d
    let inRange = false
    for (let i = 0; i < b.length; i++) if (b.start(i) <= target && target < b.end(i)) { inRange = true; break }
    if (!inRange) for (let i = 0; i < b.length; i++) if (b.start(i) > target) { target = b.start(i) + 0.01; break }
    this.st.catchups += 1
    this.seekTo(target)
  }

  /// Trim: more than 60 s buffered in a SourceBuffer -> remove [0, currentTime - 30).
  private trim(): void {
    if (!this.started || this.stopped) return
    const cut = this.video.currentTime - 30
    for (const t of this.tracks.values()) {
      const sb = t.sb
      if (!sb || t.inFlight || sb.updating) continue
      let b: TimeRanges
      try { b = sb.buffered } catch { continue }
      if (!b.length || b.end(b.length - 1) - b.start(0) <= 60 || cut <= b.start(0)) continue
      try { t.inFlight = true; sb.remove(0, cut); this.st.trims += 1 } catch { t.inFlight = false }
    }
  }

  // ---------------------------------------------------------------- stats

  private tick(): void {
    if (this.stopped) return
    this.catchUp()
    this.trim()
    this.emit()
  }

  private emit(): void {
    const v = this.video
    const t = v.currentTime
    this.st.currentTime = t
    this.st.latencyS = this.st.liveEdgeS ? this.st.liveEdgeS - t : 0
    this.st.readyState = v.readyState
    this.st.width = v.videoWidth
    this.st.height = v.videoHeight
    this.st.muted = v.muted
    try { this.st.buffered = ranges(v.buffered) } catch { this.st.buffered = [] }
    const q = v.getVideoPlaybackQuality?.()
    if (q) { this.st.droppedFrames = q.droppedVideoFrames; this.st.totalFrames = q.totalVideoFrames }
    const comps: PushComponentState[] = []
    for (const p of this.order) {
      const tr = this.tracks.get(p)!
      let ahead = 0
      if (tr.sb) {
        try {
          const b = tr.sb.buffered
          for (let i = 0; i < b.length; i++) if (b.end(i) > t && b.start(i) <= t + 0.5) { ahead = b.end(i) - Math.max(t, b.start(i)); break }
        } catch { /* SourceBuffer removed */ }
      }
      tr.state.bufferAheadS = ahead
      tr.state.queued = tr.queue.length + (tr.ready ? 1 : 0) + (tr.pending ? 1 : 0)
      comps.push({ ...tr.state })
    }
    this.st.components = comps
    this.onState?.({ ...this.st, buffered: this.st.buffered.map((r) => [r[0], r[1]] as [number, number]) })
  }

  private setStatus(s: string): void {
    if (this.st.status === 'error' && s !== 'stopped') return
    this.st.status = s
    this.emit()
  }

  private fail(message: string): void {
    if (this.stopped) return
    this.st.error = message
    this.st.status = 'error'
    this.st.playing = false
    this.emit()
    this.stop()
  }

  private mediaError(): string {
    const e = this.video.error
    return e ? `${e.code} ${e.message}`.trim() : ''
  }

  private listen(target: EventTarget, name: string, fn: EventListener): void {
    target.addEventListener(name, fn)
    this.listeners.push([target, name, fn])
  }
}
