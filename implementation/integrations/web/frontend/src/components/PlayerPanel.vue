<script setup lang="ts">
// A/V playback of a selected service (spec 5.17.3 / 5.17.4 / 5.18.2 "Player"). Two paths, picked by
// /media/{bsid}/{sid}/player.json:
//  - mode "dash" (ROUTE): dash.js against the daemon's local DASH origin (MPD from /media/{bsid}/{sid}/mpd.mpd,
//    UTCTiming -> the capture clock), EME through dash.js's protection controller with a user-supplied license URL;
//  - mode "push" (MMTP): MsePushPlayer polls /media/{bsid}/{sid}/mmt/index.json and appends the reconstructed MPU
//    fragments to a MediaSource itself (spec 5.17.4).
// The startup gate, autoplay handling (muted-first, click-to-play overlay), mute toggle, status badge and the
// Play/Stop/Cancel buttons are shared; each path fills its own stats line and per-component support badges.
// With `apps` (the service view only, spec 5.18.4) the service's broadcaster application runs in AppOverlay: an iframe
// laid into the video's box, the RMP scaled/positioned and switched on the app's request.
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { Debug, MediaPlayer } from 'dashjs'
import type { MediaInfo, MediaPlayerClass } from 'dashjs'
import type { Service } from '../api/types'
import { api } from '../api/client'
import type { AnyPlayerSettings, MediaIndex, PlayerSettings, PushPlayerSettings } from '../api/types'
import { MsePushPlayer } from '../player/MsePushPlayer'
import type { PushPlayerState } from '../player/MsePushPlayer'
import { capability, verdictLabel, verdictClass } from '../util/capabilities'
import { bytes } from '../util/format'
import AppOverlay from './AppOverlay.vue'

const props = defineProps<{ service: Service; apps?: boolean }>()

const video = ref<HTMLVideoElement | null>(null)
const screen = ref<HTMLElement | null>(null)
const videoStyle = ref<Record<string, string>>({})
const rmpSource = ref('')   // an MPD the application put on the RMP (org.atsc.setRMPURL startRmp), '' = the service
let player: MediaPlayerClass | null = null
let push: MsePushPlayer | null = null
// Event names as dash.js's MediaPlayerEvents declares them (the ESM `MediaPlayer` export carries no `events`
// property, so the names are spelled out here instead of read at runtime).
const events = {
  ERROR: 'error',
  PLAYBACK_PLAYING: 'playbackPlaying',
  PLAYBACK_WAITING: 'playbackWaiting',
  PLAYBACK_ENDED: 'playbackEnded',
  MANIFEST_LOADED: 'manifestLoaded',
  STREAM_INITIALIZED: 'streamInitialized',
  KEY_SESSION_CREATED: 'public_keySessionCreated',
  KEY_STATUSES_CHANGED: 'public_keyStatusesChanged',
  KEY_ERROR: 'public_keyError',
  LICENSE_REQUEST_COMPLETE: 'public_licenseRequestComplete',
  PLAYBACK_NOT_ALLOWED: 'playbackNotAllowed',
}
type LooseEvent = { error?: { code?: number; message?: string } | string }
type LoosePlayer = { on(type: string, listener: (e: LooseEvent) => void): void }
let statsTimer = 0

const running = ref(false)
const status = ref('idle')
const error = ref('')
const mode = ref<'dash' | 'push'>('dash')
const licenseUrl = ref(localStorage.getItem('atsc3.licenseUrl') ?? '')
const emeStatus = ref('')
const index = ref<MediaIndex | null>(null)
const settings = ref<AnyPlayerSettings | null>(null)
const stats = ref({ bufferVideo: 0, bufferAudio: 0, droppedFrames: 0, totalFrames: 0, currentTime: 0, liveLatency: 0, videoRep: '', audioRep: '', width: 0, height: 0 })
const pushState = ref<PushPlayerState | null>(null)
const audioTracks = ref<{ index: number; lang: string; codec: string; id: string }[]>([])
const textTracks = ref<{ index: number; lang: string; kind: string }[]>([])
const audioTrack = ref(-1)
const textTrack = ref(-1)
const muted = ref(true)
const needsGesture = ref(false)   // the browser refused even muted autoplay: the user must press play
const mutedByPolicy = ref(false)  // started with sound, the browser refused, retried muted

const mpdUrl = computed(() => `/media/${props.service.bsid}/${props.service.serviceId}/mpd.mpd`)
const mmtIndexUrl = computed(() => `/media/${props.service.bsid}/${props.service.serviceId}/mmt/index.json`)
const isProtected = computed(() => props.service.protected || props.service.components.some((c) => c.protected))
const avComponents = computed(() => props.service.components.filter((c) => c.kind === 'video' || c.kind === 'audio' || c.kind === 'subtitles'))
const isMmt = computed(() => props.service.protocol === 'MMTP')

async function loadIndex() {
  if (mode.value === 'push') return
  try {
    index.value = await api.mediaIndex(props.service.bsid, props.service.serviceId)
  } catch {
    index.value = null
  }
}

let waitTimer = 0
let restarts = 0
let wantRunning = false
let startToken = 0                // bumped by every start(): a continuation of an older start() (Cancel, or Play again) returns
const wantRunningRef = ref(false)

/// Starts the player once the origin holds enough segments: the broadcast delivers segments ahead of their
/// availability time and a receiver has nothing from before it tuned in, so dash.js must not start until the
/// live edge minus the live delay lies inside the cache (otherwise its first requests 404 and it gives up).
/// The push player runs its own gate (minFragments per chosen component) and reports it through its status.
/// Cancel (stop()) while player.json or index.json is in flight clears wantRunning: every await is followed by a
/// check so the continuation does not start a player the user no longer wants.
async function start() {
  if (!video.value || running.value) return
  const token = ++startToken
  const cancelled = () => !wantRunning || token !== startToken || running.value
  wantRunning = true
  wantRunningRef.value = true
  error.value = ''
  emeStatus.value = ''
  window.clearTimeout(waitTimer)
  let s: AnyPlayerSettings
  try {
    s = await api.playerSettings(props.service.bsid, props.service.serviceId)
    if (cancelled()) return
    settings.value = s
  } catch (e) {
    if (cancelled()) return
    status.value = isMmt.value ? 'waiting for the MMT origin' : 'waiting for the MPD'
    waitTimer = window.setTimeout(() => { if (wantRunning && !running.value) void start() }, 1500)
    return
  }
  if (s.mode === 'push') {
    mode.value = 'push'
    try {
      startPush(s)
    } catch (e) {
      error.value = 'push player setup failed: ' + (e as Error).message
      status.value = 'error'
      stop()
    }
    return
  }
  mode.value = 'dash'
  await loadIndex()
  if (cancelled()) return
  const reps = (index.value?.representations ?? []).filter((r) => r.kind === 'video' || r.kind === 'audio')
  const have = reps.length ? Math.min(...reps.map((r) => (r.init ? r.cachedSegments : 0))) : 0
  if (!index.value?.selected || !reps.length || have < s.minSegments) {
    status.value = `waiting for segments (${have}/${s.minSegments})`
    waitTimer = window.setTimeout(() => { if (wantRunning && !running.value) void start() }, 1000)
    return
  }
  try {
    startPlayer(s)
  } catch (e) {
    error.value = 'player setup failed: ' + (e as Error).message
    status.value = 'error'
    stop()
  }
}

function startPlayer(s: PlayerSettings) {
  if (!video.value) return
  player = MediaPlayer().create()
  player.updateSettings({
    streaming: {
      delay: { liveDelay: s.liveDelay },
      liveCatchup: { enabled: false },
      abr: { autoSwitchBitrate: { video: false, audio: false } },
      capabilities: { useMediaCapabilitiesApi: true, filterUnsupportedEssentialProperties: true },
      utcSynchronization: { enabled: true, defaultTimingSource: { scheme: 'urn:mpeg:dash:utc:http-xsdate:2014', value: s.timeUrl } },
      buffer: { fastSwitchEnabled: false },
      text: { defaultEnabled: false },
    },
    debug: { logLevel: Debug.LOG_LEVEL_WARNING },
  })
  if (isProtected.value) {
    if (licenseUrl.value) {
      player.setProtectionData({ 'com.widevine.alpha': { serverURL: licenseUrl.value }, 'com.microsoft.playready': { serverURL: licenseUrl.value } })
      emeStatus.value = 'license server configured; waiting for the key session'
    } else {
      emeStatus.value = 'protected service and no license server configured: playback will stop at the first encrypted sample'
    }
  }
  const loose = player as unknown as LoosePlayer
  loose.on(events.ERROR, (e) => {
    const code = typeof e.error === 'string' ? undefined : e.error?.code
    const err = typeof e.error === 'string' ? e.error : `${e.error?.code ?? ''} ${e.error?.message ?? ''}`.trim()
    error.value = err || 'dash.js error'
    status.value = 'error'
    // 27 = segment download failed for good (cache gap or eviction): rebuild the player at the current live edge
    if (code === 27 && restarts < 5) {
      restarts += 1
      error.value += ` — restarting (${restarts}/5)`
      stop()
      wantRunning = true
      wantRunningRef.value = true
      waitTimer = window.setTimeout(() => { if (wantRunning) void start() }, 2000)
    }
  })
  loose.on(events.PLAYBACK_PLAYING, () => {
    status.value = 'playing'
    needsGesture.value = false
    muted.value = video.value?.muted ?? true
  })
  // Autoplay policy (Firefox "block audio" / "block audio and video", Chrome without prior interaction):
  // dash.js's play() was refused. Retry muted once; if that is refused too, wait for a click on the overlay.
  loose.on(events.PLAYBACK_NOT_ALLOWED, () => {
    const v = video.value
    if (!v) return
    if (!v.muted) {
      v.muted = true
      muted.value = true
      mutedByPolicy.value = true
      status.value = 'autoplay refused with sound, retrying muted'
      v.play().catch(() => { needsGesture.value = true; status.value = 'autoplay blocked by the browser' })
    } else {
      needsGesture.value = true
      status.value = 'autoplay blocked by the browser'
    }
  })
  loose.on(events.PLAYBACK_WAITING, () => { if (!needsGesture.value) status.value = 'buffering' })
  loose.on(events.PLAYBACK_ENDED, () => { status.value = 'ended' })
  loose.on(events.MANIFEST_LOADED, () => { if (!needsGesture.value) status.value = 'manifest loaded' })
  loose.on(events.STREAM_INITIALIZED, () => {
    if (!needsGesture.value) status.value = 'stream initialized'
    refreshTracks()
  })
  loose.on(events.KEY_SESSION_CREATED, () => { emeStatus.value = 'key session created' })
  loose.on(events.KEY_STATUSES_CHANGED, () => { emeStatus.value = 'key statuses changed' })
  loose.on(events.KEY_ERROR, (e) => { emeStatus.value = 'key error: ' + (typeof e.error === 'string' ? e.error : e.error?.message ?? '') })
  loose.on(events.LICENSE_REQUEST_COMPLETE, (e) => { emeStatus.value = e.error ? 'license request failed' : 'license received' })
  player.initialize(video.value, new URL(mpdUrl.value, window.location.href).href, true)   // absolute: dash.js's CMCD reporter rejects relative URLs
  // Sound when the page already has user activation (the user clicked somewhere in the app, e.g. Play or a
  // navigation link); otherwise muted, which browsers allow without a gesture. PLAYBACK_NOT_ALLOWED handles the rest.
  const activated = (navigator as Navigator & { userActivation?: { hasBeenActive: boolean } }).userActivation?.hasBeenActive ?? false
  video.value.muted = !activated
  muted.value = video.value.muted
  needsGesture.value = false
  mutedByPolicy.value = false
  running.value = true
  status.value = 'starting'
  statsTimer = window.setInterval(updateStats, 1000)
}

/// Push path (MMTP): the player does the polling, codec probing, start gate and autoplay itself; the panel mirrors
/// its state object. On an error the player stops itself and the Play button offers a restart.
function startPush(s: PushPlayerSettings) {
  if (!video.value) return
  if (isProtected.value || s.protected) emeStatus.value = 'protected MMT service: the push player has no EME path, encrypted samples will not decode'
  push = new MsePushPlayer(video.value, s, {
    onState: (st) => {
      pushState.value = st
      status.value = st.status
      error.value = st.error
      needsGesture.value = st.needsGesture
      mutedByPolicy.value = st.mutedByPolicy
      muted.value = st.muted
      if (st.status === 'error' && running.value) {
        running.value = false
        wantRunning = false
        wantRunningRef.value = false
      }
    },
  })
  needsGesture.value = false
  mutedByPolicy.value = false
  running.value = true
  status.value = 'starting'
  push.start()
}

function stop() {
  wantRunning = false
  wantRunningRef.value = false
  window.clearTimeout(waitTimer)
  window.clearInterval(statsTimer)
  if (player) {
    try { player.reset() } catch { /* already reset */ }
    player = null
  }
  if (push) {
    const p = push
    push = null
    try { p.stop() } catch { /* already stopped */ }
  }
  running.value = false
  status.value = 'stopped'
}

function refreshTracks() {
  if (!player) return
  try {
    const audio = player.getTracksFor('audio')
    audioTracks.value = audio.map((t: MediaInfo, i: number) => ({ index: i, lang: t.lang ?? '', codec: t.codec ?? '', id: String(t.id ?? '') }))
    const cur = player.getCurrentTrackFor('audio')
    audioTrack.value = cur ? audio.indexOf(cur) : -1
    const text = player.getTracksFor('text')
    textTracks.value = text.map((t: MediaInfo, i: number) => ({ index: i, lang: t.lang ?? '', kind: String((t as unknown as { kind?: string }).kind ?? '') }))
    textTrack.value = player.getCurrentTextTrackIndex()
  } catch {
    // tracks not ready yet
  }
}
function selectAudio(i: number) {
  if (!player) return
  const tracks = player.getTracksFor('audio')
  if (tracks[i]) player.setCurrentTrack(tracks[i])
  audioTrack.value = i
}
function selectText(i: number) {
  if (!player) return
  player.setTextTrack(i)
  textTrack.value = i
}

function updateStats() {
  if (!player || !video.value) return
  try {
    const metrics = player.getDashMetrics()
    stats.value.bufferVideo = metrics.getCurrentBufferLevel('video')
    stats.value.bufferAudio = metrics.getCurrentBufferLevel('audio')
    stats.value.currentTime = video.value.currentTime
    stats.value.liveLatency = player.getCurrentLiveLatency()
    const q = video.value.getVideoPlaybackQuality?.()
    if (q) {
      stats.value.droppedFrames = q.droppedVideoFrames
      stats.value.totalFrames = q.totalVideoFrames
    }
    stats.value.width = video.value.videoWidth
    stats.value.height = video.value.videoHeight
    const v = player.getCurrentTrackFor('video')
    const a = player.getCurrentTrackFor('audio')
    stats.value.videoRep = v ? String(v.id ?? '') : ''
    stats.value.audioRep = a ? String(a.id ?? '') : ''
  } catch {
    // metrics unavailable before the stream is initialized
  }
  void loadIndex()
}

function gesturePlay() {
  const v = video.value
  if (!v) return
  needsGesture.value = false
  if (push) {
    void push.play()
    return
  }
  v.play().then(() => { status.value = 'playing' }).catch((e: Error) => { status.value = 'play refused: ' + e.message })
}
function toggleMute() {
  const v = video.value
  if (!v) return
  if (push) {
    push.setMuted(!v.muted)
  } else {
    v.muted = !v.muted
  }
  muted.value = v.muted
  mutedByPolicy.value = false
}

function saveLicenseUrl() {
  try { localStorage.setItem('atsc3.licenseUrl', licenseUrl.value) } catch { /* private mode */ }
}

/// A/344 9.7.2: the RMP video scaled to scaleFactor % of the screen, its top-left corner at (xPos, yPos) % of the
/// screen; the transform starts from the full-screen box every time (scale first, then position).
function scalePosition(scaleFactor: number, xPos: number, yPos: number): boolean {
  if (!video.value) return false
  if (scaleFactor >= 100 && xPos === 0 && yPos === 0) videoStyle.value = {}
  else videoStyle.value = { transformOrigin: '0 0', transform: `translate(${xPos}%, ${yPos}%) scale(${scaleFactor / 100})` }
  return true
}
/// A/344 9.7.3 on the dash.js path: startRmp attaches the application's MPD, resumeService goes back to the service's
/// origin MPD; pause/resume/stop act on the element. The push player (MMT) cannot change source (-21). null = ok.
function setRmpUrl(operation: string, url: string | undefined): number | null {
  const v = video.value
  if (!v) return -12
  switch (operation) {
    case 'startRmp': {
      if (!url) return -15
      if (mode.value === 'push' || !player) return -21
      try {
        player.attachSource(new URL(url, window.location.href).href)
        rmpSource.value = url
        return null
      } catch {
        return -11
      }
    }
    case 'resumeService':
      if (rmpSource.value && player) {
        player.attachSource(new URL(mpdUrl.value, window.location.href).href)
        rmpSource.value = ''
      }
      void v.play().catch(() => undefined)
      return null
    case 'stopRmp':
      v.pause()
      return null
    case 'pauseRmp':
      v.pause()
      return null
    case 'resumeRmp':
      void v.play().catch(() => undefined)
      return null
    case '':
      return null   // cancel a pending request: nothing is pending
    default:
      return -32601
  }
}
function ccEnabled(): boolean {
  return textTrack.value >= 0
}

function pushComponentClass(c: { supported: boolean | null; chosen: boolean }): string {
  if (c.chosen) return 'ok'
  if (c.supported === false) return 'bad'
  if (c.supported === true) return 'warn'
  return ''
}
function pushComponentLabel(c: { supported: boolean | null; chosen: boolean; kind: string }): string {
  if (c.chosen) return 'playing'
  if (c.supported === false) return 'no decoder'
  if (c.supported === true) return 'not chosen'
  if (c.kind === 'video' || c.kind === 'audio') return 'awaiting init'   // listed by the MP table, MPU metadata not seen yet
  return c.kind === 'subtitles' ? 'listed only' : '…'
}

onMounted(async () => {
  if (!isMmt.value) await loadIndex()
  if (props.service.selected) await start()
})
onBeforeUnmount(stop)
watch(() => props.service.selected, (sel) => { if (!sel) stop() })
</script>

<template>
  <div class="panel player">
    <div class="row" style="margin-bottom: 8px">
      <h2 style="margin: 0">Player</h2>
      <span class="badge" :class="status === 'playing' ? 'ok' : status === 'error' ? 'bad' : ''">{{ status }}</span>
      <span v-if="running || pushState" class="badge" :class="mode === 'push' ? 'mmtp' : 'route'" :title="mode === 'push' ? 'MMTP: MSE push player polling mmt/index.json' : 'ROUTE: dash.js on the local DASH origin'">{{ mode === 'push' ? 'push' : 'dash.js' }}</span>
      <span class="spacer" />
      <template v-if="isProtected">
        <input type="text" v-model="licenseUrl" placeholder="Widevine / PlayReady license URL" style="min-width: 280px" @change="saveLicenseUrl" />
      </template>
      <button v-if="!running && !wantRunningRef" class="primary" @click="start">Play</button>
      <button v-else-if="running" @click="stop">Stop</button>
      <button v-else @click="stop">Cancel</button>
    </div>
    <div v-if="error" class="banner bad">{{ error }}<span v-if="mode === 'push' && !running"> — press Play to restart</span></div>
    <div v-if="emeStatus" class="banner info">EME: {{ emeStatus }}</div>
    <div class="stage">
      <div class="screen" ref="screen">
        <video ref="video" controls playsinline class="video" :style="videoStyle" @volumechange="muted = ($event.target as HTMLVideoElement).muted" />
        <button v-if="needsGesture" class="overlay" @click="gesturePlay" title="The browser blocks automatic playback on this page; a click is required">▶ Play</button>
      </div>
    </div>
    <div v-if="rmpSource" class="banner info" style="margin-top: 6px">The application switched the player to <span class="mono">{{ rmpSource }}</span> (setRMPURL)</div>
    <div class="row" style="margin-top: 8px">
      <button v-if="running" @click="toggleMute" :title="mutedByPolicy ? 'The browser refused autoplay with sound; click to unmute' : ''">{{ muted ? 'Unmute' : 'Mute' }}</button>
      <span v-if="running && mutedByPolicy && muted" class="badge warn">muted by the autoplay policy</span>
      <label v-if="audioTracks.length > 1">Audio
        <select :value="audioTrack" @change="selectAudio(Number(($event.target as HTMLSelectElement).value))">
          <option v-for="t in audioTracks" :key="t.index" :value="t.index">{{ t.lang || t.id }} {{ t.codec }}</option>
        </select>
      </label>
      <label v-if="textTracks.length">Captions
        <select :value="textTrack" @change="selectText(Number(($event.target as HTMLSelectElement).value))">
          <option :value="-1">off</option>
          <option v-for="t in textTracks" :key="t.index" :value="t.index">{{ t.lang || t.kind || 'track ' + t.index }}</option>
        </select>
      </label>
      <span class="muted mono" v-if="running && mode === 'dash'">
        {{ stats.width }}×{{ stats.height }} · video {{ stats.videoRep }} buffer {{ stats.bufferVideo.toFixed(1) }} s · audio {{ stats.audioRep || '-' }} buffer {{ stats.bufferAudio.toFixed(1) }} s
        · latency {{ Number.isFinite(stats.liveLatency) ? stats.liveLatency.toFixed(1) : '-' }} s · dropped {{ stats.droppedFrames }}/{{ stats.totalFrames }}
      </span>
      <span class="muted mono" v-if="mode === 'push' && pushState">
        {{ pushState.width }}×{{ pushState.height }} · t {{ pushState.currentTime.toFixed(2) }} s · live edge {{ pushState.liveEdgeS.toFixed(2) }} s · latency {{ pushState.latencyS.toFixed(1) }} s
        · dropped {{ pushState.droppedFrames }}/{{ pushState.totalFrames }} · readyState {{ pushState.readyState }}
        <template v-if="pushState.gapJumps || pushState.catchups || pushState.trims"> · gap jumps {{ pushState.gapJumps }}, catch-ups {{ pushState.catchups }}, trims {{ pushState.trims }}</template>
        <template v-if="pushState.restarts"> · <span :title="pushState.lastRestart">origin restarts {{ pushState.restarts }}</span></template>
      </span>
    </div>
    <div class="row" style="margin-top: 6px; gap: 6px" v-if="mode === 'push' && pushState && pushState.components.length">
      <span v-for="c in pushState.components" :key="c.packetId" class="badge" :class="pushComponentClass(c)" :title="c.detail">
        {{ c.kind }} {{ c.codecs }} (pid {{ c.packetId }}): {{ pushComponentLabel(c) }}<template v-if="c.chosen"> · {{ c.appended }} frag, {{ bytes(c.bytes) }}, ahead {{ c.bufferAheadS.toFixed(1) }} s<template v-if="c.lastMpu !== null">, mpu {{ c.lastMpu }}</template></template><template v-if="c.damaged || c.dropped || c.missing"> · damaged {{ c.damaged }}, dropped {{ c.dropped }}, missing {{ c.missing }}</template>
      </span>
    </div>
    <div class="row" style="margin-top: 6px; gap: 6px" v-else>
      <span v-for="(c, i) in avComponents" :key="i" class="badge" :class="verdictClass(capability(c))" :title="capability(c).detail">{{ c.kind }} {{ c.codecs }}: {{ verdictLabel(capability(c)) }}</span>
    </div>
    <div v-if="mode === 'push' && pushState" class="muted" style="margin-top: 6px">
      MMT origin: {{ bytes(pushState.cachedBytes) }} cached, {{ pushState.evicted }} evicted, next seq {{ pushState.nextSeq }}, generation {{ pushState.generation }} · polls {{ pushState.polls }}<template v-if="pushState.pollErrors"> ({{ pushState.pollErrors }} failed)</template> ·
      <a :href="mmtIndexUrl" target="_blank">index.json</a>
    </div>
    <div v-else-if="index" class="muted" style="margin-top: 6px">
      Origin cache: {{ index.segments.length }} segments ({{ bytes(index.cachedBytes) }}), {{ index.evicted }} evicted ·
      <span v-if="index.liveEdgeMs !== null" :title="'the newest cached segment becomes available at this MPD time; dash.js takes it as now'">player clock {{ new Date(index.liveEdgeMs).toISOString().slice(11, 19) }}, capture clock {{ new Date(index.captureMs).toISOString().slice(11, 19) }} ·</span>
      <span v-for="r in index.representations" :key="r.id" class="mono">{{ r.id }}: {{ r.init ? 'init' : 'no init' }}, {{ r.cachedSegments }} seg<template v-if="r.lastKey !== null"> (last {{ r.lastKey }})</template>; </span>
      <a :href="mpdUrl" target="_blank">mpd.mpd</a> · <a :href="`/media/${service.bsid}/${service.serviceId}/offline.mpd`" target="_blank">offline.mpd</a>
    </div>
    <AppOverlay v-if="apps && (service.apps ?? []).length" :service="service" :screen="screen" :video="video" :scale-position="scalePosition" :set-rmp-url="setRmpUrl" :cc-enabled="ccEnabled" />
  </div>
</template>

<style scoped>
.stage { position: relative; }
.screen { position: relative; overflow: hidden; border-radius: 6px; background: #000; }
.video { width: 100%; max-height: 60vh; background: #000; border-radius: 6px; display: block; }
.overlay { position: absolute; left: 50%; top: 50%; transform: translate(-50%, -50%); font-size: 22px; padding: 14px 28px; background: rgba(0,0,0,.65); color: #fff; border: 2px solid #fff; border-radius: 10px; }
</style>
