<script setup lang="ts">
// LLS tables per group with raw XML (spec 5.18.2 "Signaling"). SLS fragments live on the service page.
import { onMounted, onUnmounted, ref, watch } from 'vue'
import { api } from '../api/client'
import type { LlsTable } from '../api/types'
import { useStackStore } from '../stores/stack'
import { bytes, prettyXml } from '../util/format'

const stack = useStackStore()
const tables = ref<LlsTable[]>([])
const current = ref<LlsTable | null>(null)
async function load() {
  tables.value = await api.lls()
  if (current.value) current.value = tables.value.find((t) => t.group === current.value!.group && t.tableId === current.value!.tableId) ?? null
  if (!current.value && tables.value.length) current.value = tables.value[0]
}
watch(() => stack.status?.version, () => void load())
let timer = 0
onMounted(() => { void load(); timer = window.setInterval(load, 3000) })
onUnmounted(() => window.clearInterval(timer))
</script>

<template>
  <section>
    <h1>Signaling — LLS</h1>
    <table>
      <thead><tr><th>Group</th><th>Table</th><th>Version</th><th>Source</th><th>Namespace</th><th>Size</th></tr></thead>
      <tbody>
        <tr v-for="t in tables" :key="t.group + '/' + t.tableId" class="clickable" :class="{ selected: current === t }" @click="current = t">
          <td class="mono">{{ t.group }}</td><td><strong>{{ t.name }}</strong> <span class="muted mono">0x{{ t.tableId.toString(16).padStart(2, '0') }}</span></td>
          <td class="mono">{{ t.version }}</td><td class="mono">{{ t.sourceIp }}</td><td class="mono muted">{{ t.rootNamespace || '-' }}</td><td class="mono">{{ bytes(t.bytes) }}</td>
        </tr>
        <tr v-if="!tables.length"><td colspan="6" class="muted">No LLS tables yet.</td></tr>
      </tbody>
    </table>
    <h2 v-if="current">{{ current.name }} (group {{ current.group }}, version {{ current.version }})</h2>
    <pre class="xml" v-if="current && current.xml">{{ prettyXml(current.xml) }}</pre>
    <p v-else-if="current" class="muted">Binary table ({{ bytes(current.bytes) }}).</p>
    <p class="muted">Service-layer signaling (USBD, S-TSID, MPD, HELD, MMT MP table) is shown on each service's page.</p>
  </section>
</template>
