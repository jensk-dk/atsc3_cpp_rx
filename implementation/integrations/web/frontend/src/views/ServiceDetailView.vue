<script setup lang="ts">
// One service: player (ROUTE via dash.js, MMTP via the MSE push player), components (tracks, codecs), MPD summary,
// SLS fragments (raw XML), recent objects.
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { api } from '../api/client'
import type { ServiceDetail } from '../api/types'
import { useStackStore } from '../stores/stack'
import ComponentTable from '../components/ComponentTable.vue'
import PlayerPanel from '../components/PlayerPanel.vue'
import ProtocolBadge from '../components/ProtocolBadge.vue'
import { bytes, channel, dateFromNs, prettyXml } from '../util/format'

const props = defineProps<{ bsid: string; sid: string }>()
const stack = useStackStore()
const detail = ref<ServiceDetail | null>(null)
const err = ref('')
const fragment = ref('')

async function load() {
  try {
    detail.value = await api.service(Number(props.bsid), Number(props.sid))
    err.value = ''
    if (!fragment.value && detail.value.fragments.length) fragment.value = detail.value.fragments[0].name
  } catch (e) {
    err.value = (e as Error).message
  }
}
watch(() => stack.status?.version, () => void load())
onMounted(load)
let timer = 0
onMounted(() => { timer = window.setInterval(load, 2000) })
onUnmounted(() => window.clearInterval(timer))

const svc = computed(() => detail.value?.service ?? null)
const fragmentText = computed(() => {
  const f = detail.value?.fragments.find((x) => x.name === fragment.value)
  if (!f) return ''
  return f.text.trimStart().startsWith('<') ? prettyXml(f.text) : f.text
})
async function toggle() {
  if (!svc.value) return
  await stack.select(svc.value.bsid, svc.value.serviceId, !svc.value.selected)
  await load()
}
</script>

<template>
  <section v-if="svc">
    <div class="row" style="margin-bottom: 10px">
      <router-link to="/services">← Services</router-link>
      <h1 style="margin: 0">{{ channel(svc.major, svc.minor) }} {{ svc.shortName || svc.usbdName }}</h1>
      <ProtocolBadge :protocol="svc.protocol" />
      <span v-if="svc.protected" class="badge bad">protected{{ svc.drmSystemId ? ' · ' + svc.drmSystemId : '' }}</span>
      <span class="spacer" />
      <button :class="{ primary: !svc.selected }" @click="toggle">{{ svc.selected ? 'Deselect' : 'Select service' }}</button>
    </div>
    <div v-if="err" class="banner bad">{{ err }}</div>

    <PlayerPanel v-if="svc.selected && (svc.protocol === 'ROUTE' || svc.protocol === 'MMTP') && svc.components.some((c) => c.kind === 'video' || c.kind === 'audio')" :key="svc.bsid + '/' + svc.serviceId" :service="svc" :apps="true" />
    <div v-else-if="!svc.selected && (svc.protocol === 'ROUTE' || svc.protocol === 'MMTP') && svc.components.some((c) => c.kind === 'video' || c.kind === 'audio')" class="banner info">Select the service to open its media sessions and play it here.</div>

    <div class="grid">
      <div class="panel">
        <h2 style="margin-top: 0">Service</h2>
        <dl class="kv">
          <dt>bsid / serviceId</dt><dd class="mono">{{ svc.bsid }} / {{ svc.serviceId }} (LLS group {{ svc.llsGroup }})</dd>
          <dt>Global id</dt><dd class="mono">{{ svc.globalServiceId ?? '-' }}</dd>
          <dt>Category</dt><dd>{{ svc.categoryName }} ({{ svc.category }})</dd>
          <dt>SLS</dt><dd class="mono">{{ svc.slsAddress }} — {{ svc.slsReceived ? `received, ${svc.slsUpdates} updates` : 'waiting' }}<span v-if="svc.slsToi"> · TOI 0x{{ svc.slsToi.toString(16) }}</span></dd>
          <dt>USBD name</dt><dd>{{ svc.usbdName || '-' }} <span class="muted" v-if="svc.languages.length">[{{ svc.languages.join(', ') }}]</span></dd>
          <dt v-if="svc.mmtPackageId">MMT package</dt><dd v-if="svc.mmtPackageId" class="mono">{{ svc.mmtPackageId }}</dd>
          <dt>Flags</dt><dd>
            <span v-if="svc.hidden" class="badge">hidden</span><span v-if="svc.hideInGuide" class="badge">hide in guide</span>
            <span v-if="svc.essential" class="badge">essential</span><span v-if="svc.broadbandRequired" class="badge">broadband required</span>
            <span v-if="!svc.hidden && !svc.hideInGuide && !svc.essential && !svc.broadbandRequired" class="muted">none</span>
          </dd>
          <dt>Selected</dt><dd>{{ svc.selected ? 'yes — media and NRT sessions open' : 'no — SLS only' }}</dd>
          <dt>Received</dt><dd class="mono">
            <template v-if="svc.protocol === 'MMTP'">{{ svc.mmtpPackets }} MMTP packets</template>
            <template v-else>{{ svc.objects }} objects, {{ bytes(svc.bytes) }}<span v-if="svc.objectErrors" class="badge bad">{{ svc.objectErrors }} errors</span></template>
          </dd>
        </dl>
      </div>
      <div class="panel" v-if="svc.mpd.profiles">
        <h2 style="margin-top: 0">MPD</h2>
        <dl class="kv">
          <dt>Profiles</dt><dd class="mono">{{ svc.mpd.profiles }}</dd>
          <dt>Type</dt><dd>{{ svc.mpd.type }}</dd>
          <dt>minBufferTime</dt><dd>{{ svc.mpd.minBufferTime ?? '-' }} s</dd>
          <dt>maxSegmentDuration</dt><dd>{{ svc.mpd.maxSegmentDuration ?? '-' }} s</dd>
          <dt>timeShiftBufferDepth</dt><dd>{{ svc.mpd.timeShiftBufferDepth ?? '-' }} s</dd>
          <dt>publishTime</dt><dd class="mono">{{ dateFromNs(svc.mpd.publishTimeNs) }}</dd>
          <dt>availabilityStartTime</dt><dd class="mono">{{ dateFromNs(svc.mpd.availabilityStartNs) }}</dd>
        </dl>
      </div>
    </div>

    <h2>Components</h2>
    <ComponentTable :components="svc.components" :protocol="svc.protocol" />
    <p v-if="svc.warnings.length" class="banner bad" style="margin-top: 10px"><span v-for="w in svc.warnings" :key="w">{{ w }}<br /></span></p>

    <template v-if="(svc.apps ?? []).length">
      <h2>Broadcaster applications (HELD)</h2>
      <table>
        <thead><tr><th>appContextId</th><th>Entry page</th><th>Package</th><th>Broadband entry</th><th>Cache</th><th>Flags</th></tr></thead>
        <tbody>
          <tr v-for="a in svc.apps" :key="a.appContextId">
            <td class="mono">{{ a.appContextId }}<span v-if="a.appId" class="muted"> ({{ a.appId }})</span></td>
            <td class="mono">{{ a.bcastEntryPageUrl ?? '-' }}</td>
            <td class="mono">{{ a.bcastEntryPackageUrl ?? '-' }}</td>
            <td class="mono"><a v-if="a.bbandEntryPageUrl" :href="a.bbandEntryPageUrl" target="_blank" rel="noreferrer">{{ a.bbandEntryPageUrl }}</a><span v-else>-</span></td>
            <td>
              <span class="badge" :class="a.ready ? 'ok' : ''">{{ a.ready ? 'entry page cached' : svc.selected ? (a.bcastEntryPageUrl ? (a.packageBytes && a.packageReceived !== null ? `receiving ${Math.round((100 * a.packageReceived) / a.packageBytes)}%` : 'waiting for the package') : 'broadband only') : 'select to receive' }}</span>
              <span class="muted">{{ a.files }} files, {{ bytes(a.bytes) }}<template v-if="a.packageBytes"> · package {{ bytes(a.packageBytes) }}<template v-if="a.packageTsi !== null"> on TSI {{ a.packageTsi }}</template></template></span>
            </td>
            <td>
              <span v-if="a.appRendering" class="badge">appRendering</span>
              <span v-if="a.coupledServices.length" class="badge" :title="'terminated when the user leaves these services'">coupled {{ a.coupledServices.join(' ') }}</span>
              <span v-if="a.requiredCapabilities" class="badge mono">{{ a.requiredCapabilities }}</span>
              <span v-if="a.validFromNs || a.validUntilNs" class="muted">{{ dateFromNs(a.validFromNs) }} – {{ dateFromNs(a.validUntilNs) }}</span>
            </td>
          </tr>
        </tbody>
      </table>
      <p class="muted" v-if="!svc.selected">The application runs over the player on this page once the service is selected (spec 5.18.4).</p>
    </template>

    <h2>Signaling fragments</h2>
    <div class="panel">
      <div class="row">
        <button v-for="f in detail!.fragments" :key="f.name" :class="{ primary: fragment === f.name }" @click="fragment = f.name">{{ f.name }} <span class="muted">({{ bytes(f.text.length) }})</span></button>
      </div>
      <pre class="xml" v-if="fragmentText">{{ fragmentText }}</pre>
      <p v-else class="muted">No SLS fragment received yet.</p>
    </div>

    <h2>Recent objects</h2>
    <div class="panel">
      <pre class="mono" v-if="detail!.recentObjects.length">{{ detail!.recentObjects.join('\n') }}</pre>
      <p v-else class="muted">{{ svc.selected ? 'No objects yet.' : 'Select the service to receive media and NRT objects.' }}</p>
    </div>
  </section>
  <section v-else>
    <router-link to="/services">← Services</router-link>
    <p class="muted" v-if="err">{{ err }}</p>
    <p class="muted" v-else>Loading…</p>
  </section>
</template>
