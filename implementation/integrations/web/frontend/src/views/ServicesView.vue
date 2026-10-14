<script setup lang="ts">
// Service list from all LLS groups (spec 5.18.2 "Services"). Click a row for details; the checkbox selects.
import { computed, ref } from 'vue'
import { useRouter } from 'vue-router'
import { useStackStore } from '../stores/stack'
import ProtocolBadge from '../components/ProtocolBadge.vue'
import { channel, bytes } from '../util/format'
import type { Service } from '../api/types'

const stack = useStackStore()
const router = useRouter()
const showHidden = ref(true)
const services = computed(() => stack.services.filter((s) => showHidden.value || !s.hidden))

function tracks(s: Service): string {
  const counts = new Map<string, number>()
  for (const c of s.components) counts.set(c.kind, (counts.get(c.kind) ?? 0) + 1)
  return [...counts.entries()].map(([k, n]) => (n > 1 ? `${n} ${k}` : k)).join(', ')
}
function codecs(s: Service): string {
  return [...new Set(s.components.map((c) => c.codecs).filter(Boolean))].join(', ')
}
function openDetail(s: Service) {
  void router.push(`/services/${s.bsid}/${s.serviceId}`)
}
async function toggle(s: Service, ev: Event) {
  ev.stopPropagation()
  await stack.select(s.bsid, s.serviceId, !s.selected)
}
</script>

<template>
  <section>
    <div class="row" style="margin-bottom: 10px">
      <h1 style="margin: 0">Services</h1>
      <span class="spacer" />
      <label><input type="checkbox" v-model="showHidden" /> show hidden</label>
    </div>
    <div v-if="!stack.open" class="banner info">No capture is open. Pick one on the <router-link to="/source">Source</router-link> page.</div>
    <table>
      <thead>
        <tr><th></th><th>Ch</th><th>Name</th><th>bsid / id</th><th>Category</th><th>Protocol</th><th>SLS</th><th>Tracks</th><th>Codecs</th><th>Flags</th><th>Received</th></tr>
      </thead>
      <tbody>
        <tr v-for="s in services" :key="s.bsid + '/' + s.serviceId" class="clickable" :class="{ selected: s.selected }" @click="openDetail(s)">
          <td><input type="checkbox" :checked="s.selected" @click="toggle(s, $event)" title="select (open media sessions)" /></td>
          <td class="mono">{{ channel(s.major, s.minor) }}</td>
          <td><strong>{{ s.shortName || s.usbdName || '(unnamed)' }}</strong><div v-if="s.usbdName && s.usbdName !== s.shortName" class="muted">{{ s.usbdName }}</div></td>
          <td class="mono">{{ s.bsid }} / {{ s.serviceId }}<div class="muted">group {{ s.llsGroup }}</div></td>
          <td>{{ s.categoryName }}</td>
          <td><ProtocolBadge :protocol="s.protocol" /><div class="muted mono">{{ s.slsAddress }}</div></td>
          <td><span class="badge" :class="s.slsReceived ? 'ok' : ''">{{ s.slsReceived ? 'received' : 'waiting' }}</span><div class="muted">{{ s.slsUpdates }} updates</div></td>
          <td>{{ tracks(s) || '-' }}</td>
          <td class="mono">{{ codecs(s) || '-' }}</td>
          <td>
            <span v-if="s.protected" class="badge bad">protected</span>
            <span v-if="s.hidden" class="badge">hidden</span>
            <span v-if="s.hideInGuide" class="badge">hide in guide</span>
            <span v-if="s.essential" class="badge">essential</span>
            <span v-if="s.broadbandRequired" class="badge">broadband</span>
            <span v-if="s.mmtPackageId" class="badge mmtp">{{ s.mmtPackageId }}</span>
          </td>
          <td class="mono">
            <template v-if="s.protocol === 'MMTP'">{{ s.mmtpPackets }} pkts</template>
            <template v-else>{{ s.objects }} obj, {{ bytes(s.bytes) }}</template>
          </td>
        </tr>
        <tr v-if="!services.length"><td colspan="11" class="muted">No services yet — waiting for an SLT.</td></tr>
      </tbody>
    </table>
  </section>
</template>
