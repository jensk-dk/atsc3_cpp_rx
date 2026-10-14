<script setup lang="ts">
// EPG (spec 5.18.2): services × time grid from /esg/schedule, programme detail from /esg/content, "now" from
// /time (capture time while a capture is open, else wall clock); clicking a channel selects the SLT service.
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import { api } from '../api/client'
import type { EsgContent, EsgService, EsgSummary, Programme } from '../api/types'
import { useStackStore } from '../stores/stack'
import EpgGrid from '../components/EpgGrid.vue'
import { bytes, prettyXml } from '../util/format'

const stack = useStackStore()
const router = useRouter()

const summary = ref<EsgSummary | null>(null)
const programmes = ref<Programme[]>([])
const err = ref('')
const picked = ref<Programme | null>(null)
const content = ref<EsgContent | null>(null)
const showXml = ref(false)
const showDelivery = ref(false)

// time base: Unix seconds; a capture's own clock when one is open, else the wall clock
const nowBase = ref<number | null>(null)
const nowFetchedAt = ref(0)
const nowFrozen = ref(false)
const tickNow = ref(0)
const now = computed<number | null>(() => {
  if (nowBase.value === null) return null
  if (nowFrozen.value) return nowBase.value
  return nowBase.value + (tickNow.value - nowFetchedAt.value) / 1000
})
const usingCaptureTime = ref(false)

const windowLen = ref(6 * 3600)
const windowStart = ref(0)
const windowEnd = computed(() => windowStart.value + windowLen.value)

function roundDown(t: number, step: number): number {
  return Math.floor(t / step) * step
}
async function loadTime() {
  const t = await api.time()
  nowFetchedAt.value = Date.now()
  tickNow.value = nowFetchedAt.value
  if (t.open && t.captureUnixMs !== null) {
    nowBase.value = t.captureUnixMs / 1000
    nowFrozen.value = t.paused || t.finished
    usingCaptureTime.value = true
  } else {
    nowBase.value = t.wallUnixMs / 1000
    nowFrozen.value = false
    usingCaptureTime.value = false
  }
}
async function loadSummary() {
  summary.value = await api.esg()
}
async function loadSchedule() {
  const r = await api.esgSchedule(windowStart.value, windowEnd.value)
  programmes.value = r.programmes
}
async function reload(all: boolean) {
  try {
    if (all) await Promise.all([loadTime(), loadSummary()])
    if (windowStart.value === 0) centreOnNow()
    await loadSchedule()
    err.value = ''
  } catch (e) {
    err.value = (e as Error).message
  }
}
function centreOnNow() {
  const n = now.value ?? Date.now() / 1000
  windowStart.value = roundDown(n, 1800) - 1800
}
function jumpToGuide() {
  if (summary.value?.spanStart != null) windowStart.value = roundDown(summary.value.spanStart, 1800)
}
function shift(direction: number) {
  windowStart.value += direction * Math.max(1800, windowLen.value / 2)
}
function setLen(len: number) {
  const centre = windowStart.value + windowLen.value / 2
  windowLen.value = len
  windowStart.value = roundDown(centre - len / 2, 1800)
}
watch([windowStart, windowLen], () => void reload(false))
watch(() => stack.status?.esgVersion, () => void reload(true))
watch(() => stack.status?.source.path, () => { windowStart.value = 0; picked.value = null; content.value = null; void reload(true) })

let timer = 0
onMounted(() => {
  void reload(true)
  timer = window.setInterval(() => { tickNow.value = Date.now() }, 1000)
})
onUnmounted(() => window.clearInterval(timer))

async function pick(p: Programme) {
  picked.value = p
  content.value = null
  showXml.value = false
  try {
    content.value = await api.esgContent(p.contentId)
  } catch (e) {
    err.value = (e as Error).message
  }
}
async function onService(s: EsgService) {
  if (s.bsid === null || s.serviceId === null) return
  if (!s.selected) await stack.select(s.bsid, s.serviceId, true)
  void router.push(`/services/${s.bsid}/${s.serviceId}`)
}

const services = computed(() => summary.value?.services ?? [])
const esgCarriers = computed(() => summary.value?.esgServices ?? [])
const guideSpan = computed(() => {
  const s = summary.value
  if (!s || s.spanStart == null || s.spanEnd == null) return null
  return { start: s.spanStart, end: s.spanEnd }
})
const nowOutsideGuide = computed(() => {
  const span = guideSpan.value
  const n = now.value
  return span !== null && n !== null && (n < span.start || n >= span.end)
})
function dateTime(unixS: number | null | undefined): string {
  if (unixS == null) return '-'
  const d = new Date(unixS * 1000)
  return d.toLocaleString(undefined, { weekday: 'short', month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' })
}
function dayLabel(unixS: number): string {
  return new Date(unixS * 1000).toLocaleDateString(undefined, { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' })
}
function duration(p: Programme): string {
  const m = Math.round((p.end - p.start) / 60)
  return m >= 60 ? `${Math.floor(m / 60)} h ${m % 60} min` : `${m} min`
}
const pickedService = computed(() => services.value.find((s) => s.id === picked.value?.serviceId) ?? null)
const selectedKey = computed(() => (picked.value ? `${picked.value.contentId}@${picked.value.start}` : null))
</script>

<template>
  <section>
    <div class="row" style="margin-bottom: 10px">
      <h1 style="margin: 0">EPG</h1>
      <span class="spacer" />
      <span v-if="now !== null" class="muted">
        now <span class="mono">{{ dateTime(now) }}</span>
        <span class="badge" :class="usingCaptureTime ? 'ok' : ''">{{ usingCaptureTime ? 'capture time' : 'wall clock' }}</span>
      </span>
    </div>
    <div v-if="!stack.open" class="banner info">No capture is open. Pick one on the <router-link to="/source">Source</router-link> page.</div>
    <div v-if="err" class="banner bad">{{ err }}</div>

    <div class="row" style="margin-bottom: 10px">
      <button @click="shift(-1)" title="earlier">◀</button>
      <button @click="centreOnNow()">Now</button>
      <button @click="shift(1)" title="later">▶</button>
      <select :value="windowLen" @change="setLen(Number(($event.target as HTMLSelectElement).value))">
        <option :value="3 * 3600">3 h</option>
        <option :value="6 * 3600">6 h</option>
        <option :value="12 * 3600">12 h</option>
        <option :value="24 * 3600">24 h</option>
      </select>
      <strong>{{ dayLabel(windowStart) }}</strong>
      <span class="muted mono">{{ dateTime(windowStart) }} – {{ dateTime(windowEnd) }}</span>
      <span class="spacer" />
      <template v-if="guideSpan">
        <span class="muted">guide covers <span class="mono">{{ dateTime(guideSpan.start) }}</span> – <span class="mono">{{ dateTime(guideSpan.end) }}</span></span>
        <button v-if="nowOutsideGuide || !programmes.length" @click="jumpToGuide()">Jump to guide</button>
      </template>
    </div>

    <div v-if="summary && !services.length" class="banner info">
      <template v-if="!esgCarriers.length">No ESG service (serviceCategory 4) in the SLT of this stream.</template>
      <template v-else>
        Waiting for the guide:
        <span v-for="c in esgCarriers" :key="c.bsid + '/' + c.serviceId">
          {{ c.shortName || 'ESG' }} {{ c.bsid }}/{{ c.serviceId }} — SLS {{ c.slsReceived ? 'received' : 'pending' }}, {{ c.objects }} objects ({{ bytes(c.bytes) }});
        </span>
        {{ summary.stats.sgdd }} SGDD, {{ summary.stats.sgdu }} SGDU, {{ summary.stats.fragments }} fragments so far.
      </template>
    </div>

    <EpgGrid :services="services" :programmes="programmes" :window-start="windowStart" :window-len="windowLen" :now="now" :selected-id="selectedKey" @pick="pick" @service="onService" />

    <div v-if="picked" class="panel" style="margin-top: 14px">
      <div class="row">
        <img v-if="content?.icon || picked.icon" :src="content?.icon || picked.icon" alt="" class="poster" />
        <div style="flex: 1; min-width: 0">
          <h2 style="margin: 0 0 4px">{{ content?.title || picked.title || picked.contentId }}</h2>
          <div class="muted">
            <template v-if="pickedService">
              <span class="mono">{{ pickedService.major }}.{{ pickedService.minor }}</span> {{ pickedService.name }} ·
            </template>
            <span class="mono">{{ dateTime(picked.start) }} – {{ dateTime(picked.end) }}</span> · {{ duration(picked) }}
            <span v-if="content?.ratings[0]?.description || picked.rating" class="badge">{{ content?.ratings[0]?.description || picked.rating }}</span>
            <span v-for="g in content?.genres ?? picked.genres" :key="g" class="badge" :title="g">{{ g.split(':').pop() }}</span>
          </div>
          <p style="margin: 8px 0">{{ content?.description || picked.description || '(no description)' }}</p>
          <dl v-if="content" class="kv">
            <dt>Content id</dt><dd class="mono">{{ content.id }} <span class="muted">v{{ content.version }}</span></dd>
            <dt v-if="content.lengthS !== null">Length</dt><dd v-if="content.lengthS !== null">{{ Math.round(content.lengthS / 60) }} min</dd>
            <dt v-if="content.audioLanguages.length">Audio</dt><dd v-if="content.audioLanguages.length">{{ content.audioLanguages.join(', ') }}</dd>
            <dt v-if="content.textLanguages.length">Text</dt><dd v-if="content.textLanguages.length">{{ content.textLanguages.join(', ') }}</dd>
            <dt v-if="content.components.length">Components</dt>
            <dd v-if="content.components.length"><span v-for="(c, i) in content.components" :key="i" class="badge" :class="'kind-' + c.kind">{{ c.kind }} {{ c.language }} {{ c.text }}</span></dd>
            <dt v-if="content.ratings.length">Ratings</dt>
            <dd v-if="content.ratings.length">
              <div v-for="(r, i) in content.ratings" :key="i">
                {{ r.description }} <span class="muted">{{ r.scheme || (r.region !== null ? 'region ' + r.region : 'RRT') }}</span>
                <span v-for="d in r.dimensions" :key="d.dimension" class="badge">dim {{ d.dimension }}: {{ d.value }}</span>
              </div>
            </dd>
            <dt>Schedule</dt><dd class="mono">{{ picked.scheduleId }}</dd>
          </dl>
          <details v-if="content" @toggle="showXml = ($event.target as HTMLDetailsElement).open">
            <summary>Content fragment XML</summary>
            <pre v-if="showXml" class="xml">{{ prettyXml(content.xml) }}</pre>
          </details>
        </div>
      </div>
    </div>

    <details v-if="summary" style="margin-top: 14px" @toggle="showDelivery = ($event.target as HTMLDetailsElement).open">
      <summary>Guide delivery (SGDD / SGDU / icons)</summary>
      <div v-if="showDelivery" class="panel" style="margin-top: 8px">
        <dl class="kv">
          <dt>Fragments</dt>
          <dd>{{ summary.stats.services }} Service, {{ summary.stats.schedules }} Schedule, {{ summary.stats.contents }} Content → {{ summary.stats.programmes }} programmes
            <span class="muted">({{ summary.stats.fragmentsNew }} new, {{ summary.stats.fragmentsUpdated }} updated, {{ summary.stats.fragmentsDuplicate }} carousel repeats, {{ summary.stats.failed }} failed, {{ summary.stats.unresolvedContents }} unresolved content refs)</span>
          </dd>
          <dt>Objects</dt>
          <dd>{{ summary.stats.objects }} on ESG services: {{ summary.stats.sgdd }} SGDD, {{ summary.stats.sgdu }} SGDU, {{ summary.stats.files }} files ({{ bytes(summary.stats.fileBytes) }}), {{ summary.stats.ignored }} ignored, {{ summary.stats.parseErrors }} parse errors</dd>
        </dl>
        <table v-if="summary.descriptors.length" style="margin-top: 8px">
          <thead><tr><th>SGDD</th><th>Version</th><th>Carried by</th><th>Entry window</th><th>Transport</th><th>Units</th></tr></thead>
          <tbody>
            <tr v-for="d in summary.descriptors" :key="d.bsid + '/' + d.serviceId + '/' + d.location">
              <td class="mono">{{ d.id }}<div class="muted">{{ d.location }}</div></td>
              <td class="mono">{{ d.version ?? '-' }}</td>
              <td class="mono">{{ d.bsid }}/{{ d.serviceId }} tsi {{ d.tsi }} toi {{ d.toi }}</td>
              <td><div v-for="(e, i) in d.entries" :key="i" class="mono">{{ dateTime(e.start) }} – {{ dateTime(e.end) }}</div></td>
              <td><div v-for="(e, i) in d.entries" :key="i" class="mono">{{ e.ip ? e.ip + ':' + e.port + ' ' : '' }}tsi {{ e.tsi ?? '-' }}</div></td>
              <td>
                <div v-for="(e, i) in d.entries" :key="i">
                  <div v-for="u in e.units" :key="u.contentLocation" class="mono">
                    {{ u.contentLocation }} <span class="muted">toi {{ u.toi ?? '-' }}: {{ u.fragments }} fragments ({{ u.service }} svc, {{ u.schedule }} sch, {{ u.content }} cnt<template v-if="u.other">, {{ u.other }} other</template>)</span>
                  </div>
                </div>
              </td>
            </tr>
          </tbody>
        </table>
        <div v-if="summary.files.length" class="row" style="margin-top: 10px">
          <a v-for="f in summary.files" :key="f.name" :href="f.url" target="_blank" :title="`${f.name} (${bytes(f.bytes)}, tsi ${f.tsi} toi ${f.toi})`">
            <img :src="f.url" :alt="f.name" class="thumb" loading="lazy" />
          </a>
        </div>
      </div>
    </details>
  </section>
</template>

<style scoped>
.poster { width: 120px; max-height: 180px; object-fit: contain; background: #fff; border-radius: 6px; flex: none; }
.thumb { height: 48px; background: #fff; border-radius: 4px; border: 1px solid var(--border); }
</style>
