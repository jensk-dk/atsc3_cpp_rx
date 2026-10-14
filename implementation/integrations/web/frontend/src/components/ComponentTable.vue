<script setup lang="ts">
// Components (tracks) of a service: kind, transport id, codec, parameters and live counters.
import type { Component } from '../api/types'
import { capability, verdictClass, verdictLabel } from '../util/capabilities'
import { bitrate, bytes } from '../util/format'

defineProps<{ components: Component[]; protocol: string }>()

function params(c: Component): string {
  const p: string[] = []
  if (c.width && c.height) p.push(`${c.width}×${c.height}`)
  if (c.frameRate) p.push(`${c.frameRate} fps`)
  if (c.audioSamplingRate) p.push(`${c.audioSamplingRate} Hz`)
  if (c.timescale && c.transport === 'MMTP') p.push(`timescale ${c.timescale}`)
  return p.join(', ')
}
function transportId(c: Component): string {
  if (c.transport === 'MMTP') return c.packetId === null ? '-' : `pid ${c.packetId}`
  return c.tsi === null ? 'MPD only' : `tsi ${c.tsi}`
}
</script>

<template>
  <table>
    <thead>
      <tr><th>Kind</th><th>Transport</th><th>Id</th><th>Codec</th><th>Browser</th><th>Parameters</th><th>Lang / role</th><th>Bitrate</th><th>Received</th><th>Last</th></tr>
    </thead>
    <tbody>
      <tr v-for="(c, i) in components" :key="i">
        <td>
          <span class="badge" :class="'kind-' + c.kind">{{ c.kind }}</span>
          <span v-if="c.protected" class="badge bad" title="ContentProtection / protected component">DRM</span>
          <span v-if="c.defaultAsset" class="badge">default</span>
          <span v-if="!c.realTime && c.transport === 'ROUTE'" class="badge">NRT</span>
        </td>
        <td class="mono">{{ transportId(c) }}<div class="muted">{{ c.session }}</div></td>
        <td class="mono">{{ c.id }}<div v-if="c.name" class="muted">{{ c.name }}</div><div v-if="c.mimeType" class="muted">{{ c.mimeType }}</div></td>
        <td class="mono">{{ c.codecs || '-' }}</td>
        <td>
          <span class="badge" :class="verdictClass(capability(c))" :title="capability(c).detail">{{ verdictLabel(capability(c)) }}</span>
          <span v-if="capability(c).assumedProfile" class="muted" title="MMT asset types carry only the fourcc; a typical profile string was probed">≈</span>
        </td>
        <td>{{ params(c) || '-' }}</td>
        <td>{{ c.lang || '-' }}<span v-for="r in c.roles" :key="r" class="badge">{{ r }}</span></td>
        <td class="mono">{{ bitrate(c.bandwidth) }}</td>
        <td class="mono">
          <template v-if="c.transport === 'MMTP'">{{ c.packets }} pkts, {{ c.objects }} MPUs, {{ bytes(c.bytes) }}<span v-if="c.errors" class="badge bad">{{ c.errors }} gaps</span></template>
          <template v-else>{{ c.objects }} obj, {{ bytes(c.bytes) }}<span v-if="c.errors" class="badge bad">{{ c.errors }} err</span></template>
        </td>
        <td class="mono muted">
          <template v-if="c.transport === 'MMTP'">{{ c.lastMpuSequence === null ? '-' : 'MPU ' + c.lastMpuSequence }}</template>
          <template v-else>{{ c.lastName ?? (c.lastToi === null ? '-' : 'toi ' + c.lastToi) }}</template>
        </td>
      </tr>
      <tr v-if="!components.length"><td colspan="10" class="muted">No components yet (waiting for SLS{{ protocol === 'MMTP' ? ' / MP table' : '' }})</td></tr>
    </tbody>
  </table>
</template>
