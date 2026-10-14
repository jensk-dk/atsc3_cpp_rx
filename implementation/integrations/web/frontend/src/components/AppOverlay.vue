<script setup lang="ts">
// Broadcaster Application runtime of the service view (spec 5.18.4, A/344): launches the HELD entry page from the
// daemon's Application Context Cache origin in an iframe laid over the video (RMP model, appRendering=false: the
// app is a transparent graphics plane on top of the receiver's player), hosts the receiver API for it (A344Host via
// the injected shim), routes remote-control keys (KeyRouter, virtual remote + keyboard), and keeps an app console.
// Rendered by PlayerPanel only in the service view: the Player view shows several services at once and it would be
// unclear which one the remote control drives.
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import { api } from '../api/client'
import type { AppsIndex, AppsIndexApp, Service } from '../api/types'
import { A344Host, type AlertEntry, type RpcLogEntry, type SignalingEntry } from '../a344/host'
import { KeyRouter } from '../a344/keys'
import { useStackStore } from '../stores/stack'
import { bytes } from '../util/format'
import RemoteControl from './RemoteControl.vue'

const props = defineProps<{
  service: Service
  screen: HTMLElement | null                 // the element the iframe is laid into (the video's box)
  video: HTMLVideoElement | null
  scalePosition: (scaleFactor: number, xPos: number, yPos: number) => boolean
  setRmpUrl: (operation: string, url: string | undefined) => number | null
  ccEnabled: () => boolean
}>()

const stack = useStackStore()
const vueRouter = useRouter()
const index = ref<AppsIndex | null>(null)
const pollError = ref('')
const state = ref<'waiting' | 'running' | 'terminated'>('waiting')
const hidden = ref(false)
const autoLaunch = ref(true)
const mouseToApp = ref(false)   // TV applications are remote-driven; by default the mouse keeps reaching the video's controls
const log = ref<RpcLogEntry[]>([])
const granted = ref<string[]>([])
const subscriptions = ref<string[]>([])
const connections = ref(0)
const digitBuffer = ref('')
let digitTimer = 0
let frame: HTMLIFrameElement | null = null
let host: A344Host | null = null
let pollTimer = 0
let launchedUrl = ''

const app = computed<AppsIndexApp | null>(() => {
  const apps = index.value?.apps ?? []
  return apps.find((a) => a.ready) ?? apps.find((a) => a.entryUrl) ?? apps[0] ?? null
})
const running = computed(() => state.value === 'running')
/// Entry package reception while it is not ready: fraction and label (A/344 packages ride slow carousels).
const progress = computed(() => {
  const a = app.value
  if (!a || a.ready || !a.packageBytes) return null
  const got = a.packageReceived ?? 0
  return { fraction: Math.min(1, got / a.packageBytes), label: `${bytes(got)} of ${bytes(a.packageBytes)}` }
})

function addLog(e: RpcLogEntry) {
  log.value.push(e)
  if (log.value.length > 200) log.value.splice(0, log.value.length - 200)
}
function info(text: string) {
  addLog({ t: Date.now(), dir: 'info', text })
}

const keys = new KeyRouter({
  frameWindow: () => frame?.contentWindow ?? null,
  receiver: (name) => receiverKey(name),
  log: (text) => {
    info(text)
    granted.value = [...keys.granted]
  },
})

/// Keys the application does not hold: the receiver's own behaviour (A/344 9.11, 9.12).
function receiverKey(name: string) {
  if (name === 'BAAppear') {   // the application's key: launch it, or bring it back; the router also delivers it
    if (!running.value) {
      if (app.value?.ready) launch()
      info(`receiver: BAAppear → ${app.value?.ready ? 'launch' : 'no application ready'}`)
    } else if (hidden.value) {
      hidden.value = false
      applyVisibility()
      info('receiver: BAAppear → application shown')
    }
    return
  }
  if (name === 'Back') {
    if (running.value && !hidden.value) {
      hidden.value = true
      applyVisibility()
      info('receiver: Back → application hidden')
    } else {
      info('receiver: Back (nothing to do)')
    }
    return
  }
  if (name === 'ChannelUp' || name === 'ChannelDown') {
    const list = stack.services.filter((s) => (s.protocol === 'ROUTE' || s.protocol === 'MMTP') && !s.hidden && s.category !== 4)
    const i = list.findIndex((s) => s.bsid === props.service.bsid && s.serviceId === props.service.serviceId)
    if (i < 0 || list.length < 2) {
      info(`receiver: ${name} (no other service)`)
      return
    }
    const next = list[(i + (name === 'ChannelUp' ? 1 : list.length - 1)) % list.length]
    info(`receiver: ${name} → ${next.major ?? '?'}.${next.minor ?? '?'} ${next.shortName}`)
    void tune(next)
    return
  }
  if (/^[0-9]$/.test(name)) {
    digitBuffer.value += name
    window.clearTimeout(digitTimer)
    digitTimer = window.setTimeout(() => {
      const major = Number(digitBuffer.value)
      digitBuffer.value = ''
      const target = stack.services.find((s) => s.major === major && (s.protocol === 'ROUTE' || s.protocol === 'MMTP'))
      if (!target) {
        info(`receiver: channel ${major}: no such service`)
        return
      }
      if (target.bsid === props.service.bsid && target.serviceId === props.service.serviceId) {
        info(`receiver: channel ${major}: already selected`)
        return
      }
      info(`receiver: channel ${major} → ${target.shortName}`)
      void tune(target)
    }, 1500)
    return
  }
  info(`receiver: ${name} (not granted to the application, no receiver action)`)
}

async function tune(target: Service) {
  await stack.select(props.service.bsid, props.service.serviceId, false)
  await stack.select(target.bsid, target.serviceId, true)
  await vueRouter.push(`/services/${target.bsid}/${target.serviceId}`)
}

async function poll() {
  try {
    index.value = await api.appsIndex(props.service.bsid, props.service.serviceId)
    pollError.value = ''
  } catch (e) {
    pollError.value = (e as Error).message
    return
  }
  if (autoLaunch.value && state.value === 'waiting' && app.value?.ready && app.value.entryUrl && props.screen) launch()
}

function applyVisibility() {
  if (!frame) return
  frame.style.visibility = hidden.value ? 'hidden' : 'visible'
  // A/344 4.2.1: the application is told it lost / regained focus through the usual W3C events
  try {
    frame.contentWindow?.dispatchEvent(new Event(hidden.value ? 'blur' : 'focus'))
  } catch {
    // cross-origin frame
  }
}

function launch() {
  const a = app.value
  if (!a || !a.entryUrl || !props.screen) return
  terminate(true)
  const f = document.createElement('iframe')
  f.className = 'app-frame'
  f.title = `Broadcaster application ${a.appContextId}`
  f.setAttribute('sandbox', 'allow-scripts allow-same-origin allow-forms allow-modals')
  f.setAttribute('allowtransparency', 'true')
  f.src = a.entryUrl
  f.style.cssText = 'position:absolute;left:0;top:0;width:100%;height:100%;border:0;background:transparent;'
  f.style.pointerEvents = mouseToApp.value ? 'auto' : 'none'
  props.screen.appendChild(f)
  frame = f
  launchedUrl = a.entryUrl
  const ws = index.value?.wsUrl ?? ''
  host = new A344Host(f, {
    service: () => props.service,
    baseUri: new URL(a.baseUrl, window.location.href).href,
    wsUrl: ws,
    video: () => props.video,
    keys,
    log: addLog,
    scalePosition: props.scalePosition,
    setRmpUrl: props.setRmpUrl,
    alerting,
    signaling,
    ccEnabled: props.ccEnabled,
    onStateChange: () => {
      connections.value = host?.connections ?? 0
      subscriptions.value = host?.subscriptions ?? []
      granted.value = [...keys.granted]
    },
  })
  host.attach()
  f.addEventListener('load', () => {
    keys.attachFrame(f.contentWindow)
    info(`entry page loaded from ${a.entryUrl}`)
  })
  keys.attachHost()
  state.value = 'running'
  hidden.value = false
  info(`launched ${a.appContextId}: ${a.bcastEntryPageUrl} from ${a.bcastEntryPackageUrl}`)
}

function terminate(silent = false) {
  if (host) {
    host.detach()
    host = null
  }
  keys.detachFrame()
  keys.detachHost()
  keys.reset()
  granted.value = []
  subscriptions.value = []
  connections.value = 0
  if (frame) {
    frame.remove()
    frame = null
  }
  props.scalePosition(100, 0, 0)
  if (state.value === 'running' && !silent) info('application terminated')
  state.value = silent ? state.value : 'terminated'
  hidden.value = false
}

function relaunch() {
  state.value = 'waiting'
  launch()
}

// --- data the receiver API needs from the daemon ---
let lastAeat = ''
async function alerting(): Promise<AlertEntry[]> {
  const tables = await api.lls()
  const out: AlertEntry[] = []
  for (const t of tables) {
    if (t.name === 'AEAT') out.push({ alertingType: 'AEAT', alertingFragment: t.xml })
    if (t.name === 'OMN' || t.name === 'OSN') out.push({ alertingType: 'OSN', alertingFragment: t.xml, receiveTime: new Date(t.timeNs / 1e6).toISOString() })
  }
  return out
}
async function signaling(names: string[]): Promise<SignalingEntry[]> {
  const want = new Set(names.map((n) => n.toUpperCase()))
  const any = want.size === 0
  const out: SignalingEntry[] = []
  const tables = await api.lls()
  for (const t of tables) {
    const n = t.name.toUpperCase()
    if (any || want.has(n) || (n === 'STT' && want.has('SYSTEMTIME'))) out.push({ name: t.name, group: t.group, version: t.version, table: t.xml })
  }
  const detail = await api.service(props.service.bsid, props.service.serviceId)
  const kind = (fragName: string): string => {
    const f = fragName.toLowerCase()
    if (f.includes('usbd') || f.includes('usd')) return 'USBD'
    if (f.includes('stsid') || f.includes('s-tsid')) return 'S-TSID'
    if (f.includes('mpd')) return 'MPD'
    if (f.includes('held')) return 'HELD'
    if (f.includes('dwd')) return 'DWD'
    if (f.includes('apd')) return 'APD'
    if (f.includes('rsat')) return 'RSAT'
    if (f.includes('mpt') || f.includes('mp table')) return 'MPT'
    return fragName
  }
  for (const f of detail.fragments) {
    const n = kind(f.name)
    if (n === 'SLT entry') continue
    if (any || want.has(n.toUpperCase())) out.push({ name: n, table: f.text })
  }
  return out
}
/// alertingChange when the AEAT changes while the app is running and subscribed
watch(() => stack.status?.version, async () => {
  if (!host || !host.subscriptions.includes('alertingChange')) return
  try {
    const list = await alerting()
    const key = JSON.stringify(list)
    if (key !== lastAeat && lastAeat !== '') host.notify('alertingChange', { alertList: list })
    lastAeat = key
  } catch {
    // daemon unreachable: the next status change retries
  }
})

watch(() => props.screen, () => { if (props.screen && autoLaunch.value && state.value === 'waiting' && app.value?.ready) launch() })
watch(mouseToApp, (on) => { if (frame) frame.style.pointerEvents = on ? 'auto' : 'none' })
watch(() => props.service.selected, (sel) => { if (!sel) terminate() })
onMounted(() => {
  void poll()
  pollTimer = window.setInterval(poll, 2000)
})
onBeforeUnmount(() => {
  window.clearInterval(pollTimer)
  window.clearTimeout(digitTimer)
  terminate(true)
})

function dirMark(d: RpcLogEntry['dir']): string {
  switch (d) {
    case 'in': return '→'
    case 'out': return '←'
    case 'notify': return '⇐'
    case 'error': return '✖'
    default: return '·'
  }
}
function stamp(t: number): string {
  return new Date(t).toISOString().slice(11, 23)
}
const consoleEl = ref<HTMLElement | null>(null)
watch(() => log.value.length, () => {
  const el = consoleEl.value
  if (el) requestAnimationFrame(() => { el.scrollTop = el.scrollHeight })
})
defineExpose({ launch, terminate, keys })
</script>

<template>
  <div class="app" v-if="index && index.apps.length">
    <div class="row" style="margin-top: 10px">
      <h3 style="margin: 0">Broadcaster application</h3>
      <span class="badge" :class="running ? (hidden ? 'warn' : 'ok') : state === 'terminated' ? '' : app?.ready ? 'warn' : ''">
        {{ running ? (hidden ? 'running, hidden' : 'running') : state === 'terminated' ? 'terminated' : app?.ready ? 'ready' : app?.entryUrl ? (progress ? `receiving ${app.bcastEntryPackageUrl}` : 'waiting for the package') : 'broadband only' }}
      </span>
      <span v-if="progress" class="progress" :title="`${progress.label} of the entry package received so far; the carousel delivers a full copy about once per cycle`">
        <span class="bar" :style="{ width: `${Math.round(progress.fraction * 100)}%` }" /><span class="pct">{{ Math.round(progress.fraction * 100) }}% · {{ progress.label }}</span>
      </span>
      <span v-if="running" class="badge" :title="'atscCmd connections from the application'">{{ connections }} conn</span>
      <span class="mono muted" :title="app?.appId ? 'appId ' + app.appId : ''">{{ app?.appContextId }}</span>
      <span class="spacer" />
      <label class="muted"><input type="checkbox" v-model="autoLaunch" /> auto-launch</label>
      <label class="muted" title="Hand mouse clicks to the application instead of the player's controls underneath"><input type="checkbox" v-model="mouseToApp" /> mouse to app</label>
      <button v-if="running" @click="hidden = !hidden; applyVisibility()">{{ hidden ? 'Show' : 'Hide' }}</button>
      <button v-if="running" @click="terminate()">Terminate</button>
      <button v-else class="primary" :disabled="!app?.ready || !app?.entryUrl || !screen" @click="relaunch">Launch</button>
    </div>
    <div class="muted" style="margin-top: 4px">
      <template v-if="app?.bcastEntryPageUrl">entry page <span class="mono">{{ app.bcastEntryPageUrl }}</span> from <span class="mono">{{ app.bcastEntryPackageUrl }}</span> · </template>
      <template v-if="app?.bbandEntryPageUrl">broadband <a :href="app.bbandEntryPageUrl" target="_blank" rel="noreferrer">{{ app.bbandEntryPageUrl }}</a> · </template>
      {{ app?.files ?? 0 }} files ({{ bytes(app?.bytes ?? 0) }}) in the application context cache
      <template v-if="app?.appRendering"> · appRendering (the application renders the media itself)</template>
      <template v-if="app?.coupledServices.length"> · coupled to {{ app.coupledServices.join(', ') }}</template>
      <template v-if="index.rejected"> · <span class="badge bad">{{ index.rejected }} objects rejected</span></template>
      <template v-if="index.apps.length > 1"> · {{ index.apps.length }} HTMLEntryPackages, the first ready one is launched</template>
      · <a :href="`/apps/${service.bsid}/${service.serviceId}/index.json`" target="_blank">index.json</a>
    </div>
    <div v-if="pollError" class="banner bad" style="margin-top: 6px">{{ pollError }}</div>
    <div class="row" style="margin-top: 6px; gap: 6px">
      <span class="muted">keys granted to the application:</span>
      <span v-if="!granted.length" class="muted">none</span>
      <span v-for="k in granted" :key="k" class="badge ok">{{ k }}</span>
      <span class="muted" style="margin-left: 8px">subscriptions:</span>
      <span v-if="!subscriptions.length" class="muted">none</span>
      <span v-for="m in subscriptions" :key="m" class="badge">{{ m }}</span>
      <span v-if="digitBuffer" class="badge warn">channel {{ digitBuffer }}…</span>
    </div>
    <div class="approw">
      <RemoteControl :router="keys" :granted="granted" />
      <div class="console" ref="consoleEl">
        <div v-for="(e, i) in log" :key="i" class="line" :class="e.dir">
          <span class="muted">{{ stamp(e.t) }}</span> <span class="mark">{{ dirMark(e.dir) }}</span> {{ e.text }}
        </div>
        <div v-if="!log.length" class="muted">App console: receiver API requests and responses, notifications, key routing, application errors.</div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.app { border-top: 1px solid var(--border); margin-top: 10px; }
.progress { position: relative; display: inline-block; width: 220px; height: 16px; border: 1px solid var(--border); border-radius: 8px; overflow: hidden; background: var(--bg); vertical-align: middle; }
.progress .bar { position: absolute; left: 0; top: 0; bottom: 0; background: rgba(77,163,255,.35); }
.progress .pct { position: relative; font-size: 11px; line-height: 16px; padding: 0 6px; color: var(--muted); white-space: nowrap; }
.approw { display: flex; gap: 12px; margin-top: 8px; align-items: flex-start; }
.console { flex: 1; min-width: 0; background: var(--bg); border: 1px solid var(--border); border-radius: 8px; padding: 8px 10px; height: 300px; overflow: auto; font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 12px; }
.line { white-space: pre-wrap; word-break: break-all; }
.line.in .mark { color: var(--accent); }
.line.out .mark { color: var(--ok); }
.line.notify .mark { color: var(--warn); }
.line.error { color: var(--bad); }
</style>
