<script setup lang="ts">
// LMT / PLP map / L1 info / unrouted flows (spec 5.18.2 "Link").
import { onMounted, onUnmounted, ref } from 'vue'
import { api } from '../api/client'
import type { LinkInfo } from '../api/types'
import { bytes } from '../util/format'

const link = ref<LinkInfo | null>(null)
async function load() { link.value = await api.link() }
let timer = 0
onMounted(() => { void load(); timer = window.setInterval(load, 2000) })
onUnmounted(() => window.clearInterval(timer))
</script>

<template>
  <section>
    <h1>Link layer</h1>
    <template v-if="link">
      <h2>PLP map ({{ link.plpMap.length }} entries)</h2>
      <table>
        <thead><tr><th>PLP</th><th>Destination</th><th>Source</th><th>Origin</th></tr></thead>
        <tbody>
          <tr v-for="(e, i) in link.plpMap" :key="i"><td class="mono">{{ e.plpId }}</td><td class="mono">{{ e.dst }}:{{ e.dstPort }}</td><td class="mono">{{ e.src }}</td><td>{{ e.origin }}</td></tr>
          <tr v-if="!link.plpMap.length"><td colspan="4" class="muted">No LMT received (raw-IP captures without a sidecar have none).</td></tr>
        </tbody>
      </table>
      <h2>LMTs</h2>
      <div v-for="l in link.lmts" :key="l.carrierPlp" class="panel">
        <strong>LMT on PLP {{ l.carrierPlp }}</strong> version {{ l.version }}, {{ l.plps.length }} PLPs
        <ul class="mono">
          <li v-for="p in l.plps" :key="p.plpId">PLP {{ p.plpId }}: {{ p.multicasts.length }} multicasts</li>
        </ul>
      </div>
      <h2>L1 / PLP configuration</h2>
      <table v-if="link.l1">
        <thead><tr><th>PLP</th><th>LLS</th><th>Modulation</th><th>Code rate</th><th>FEC</th><th>Layer</th></tr></thead>
        <tbody><tr v-for="p in link.l1.plps" :key="p.plpId"><td class="mono">{{ p.plpId }}</td><td>{{ p.llsFlag ? 'yes' : '' }}</td><td>{{ p.modulation }}</td><td>{{ p.codeRate }}</td><td>{{ p.fecCodeLength }} {{ p.fecOuterCode }}</td><td>{{ p.layer }}</td></tr></tbody>
      </table>
      <p v-else class="muted">No L1 information (not an STLTP capture and no sidecar).</p>
      <h2>Unrouted UDP flows</h2>
      <table>
        <thead><tr><th>Destination</th><th>Source</th><th>PLP</th><th>Packets</th><th>Bytes</th></tr></thead>
        <tbody>
          <tr v-for="(u, i) in link.unrouted" :key="i"><td class="mono">{{ u.dst }}:{{ u.dstPort }}</td><td class="mono">{{ u.src }}</td><td class="mono">{{ u.plp === 255 ? '-' : u.plp }}</td><td class="mono">{{ u.packets }}</td><td class="mono">{{ bytes(u.bytes) }}</td></tr>
          <tr v-if="!link.unrouted.length"><td colspan="5" class="muted">None.</td></tr>
        </tbody>
      </table>
    </template>
  </section>
</template>
