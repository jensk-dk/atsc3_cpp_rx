<script setup lang="ts">
// Virtual tuner panel (spec 5.18.2 "Source"): pick a capture, pacing, speed, loop; open/pause/close.
import { onMounted, ref, computed } from 'vue'
import { api } from '../api/client'
import type { Capture, SourceList } from '../api/types'
import { useStackStore } from '../stores/stack'
import { bytes, seconds } from '../util/format'

const stack = useStackStore()
const list = ref<SourceList | null>(null)
const loading = ref(false)
const err = ref('')
const pacing = ref<'realtime' | 'nonrealtime'>('realtime')
const speed = ref(1)
const loop = ref(true)
const selectedPath = ref('')

const current = computed(() => stack.status?.source ?? null)
const byDir = computed(() => {
  const groups = new Map<string, Capture[]>()
  for (const c of list.value?.captures ?? []) {
    const k = c.dir || '.'
    if (!groups.has(k)) groups.set(k, [])
    groups.get(k)!.push(c)
  }
  return [...groups.entries()]
})

async function load() {
  loading.value = true
  err.value = ''
  try {
    list.value = await api.source()
  } catch (e) {
    err.value = (e as Error).message
  } finally {
    loading.value = false
  }
}
async function open(path: string) {
  err.value = ''
  selectedPath.value = path
  try {
    await api.open(path, pacing.value, speed.value, loop.value)
    await stack.refresh()
  } catch (e) {
    err.value = (e as Error).message
  }
}
async function action(a: 'close' | 'pause' | 'resume') {
  try {
    await api.sourceAction(a)
    await stack.refresh()
  } catch (e) {
    err.value = (e as Error).message
  }
}
onMounted(load)
</script>

<template>
  <section>
    <h1>Source</h1>
    <div class="panel">
      <div class="row">
        <label>Pacing
          <select v-model="pacing">
            <option value="realtime">real-time (capture timestamps)</option>
            <option value="nonrealtime">non-real-time (as fast as possible)</option>
          </select>
        </label>
        <label>Speed <input type="number" v-model.number="speed" min="0.1" max="100" step="0.5" style="width: 70px" :disabled="pacing !== 'realtime'" /></label>
        <label><input type="checkbox" v-model="loop" /> loop</label>
        <span class="spacer" />
        <template v-if="current?.open">
          <button v-if="!current.paused" @click="action('pause')">Pause</button>
          <button v-else @click="action('resume')">Resume</button>
          <button @click="action('close')">Close</button>
        </template>
        <button @click="load" :disabled="loading">Rescan</button>
      </div>
      <div v-if="current?.open" class="kv" style="margin-top: 10px">
        <dt>Open</dt><dd class="mono">{{ current.path }}</dd>
        <dt>Format</dt><dd>{{ current.format }} <span class="muted">— {{ current.detection }}</span></dd>
        <dt>Pacing</dt><dd>{{ current.pacing }} × {{ current.speed }} <span v-if="current.loop">(loop)</span></dd>
        <dt>Position</dt><dd class="mono">{{ seconds(current.positionNs) }} <span v-if="current.finished" class="badge">end of capture</span></dd>
      </div>
      <div v-else-if="current?.error" class="banner bad" style="margin-top: 10px">{{ current.error }}</div>
      <div v-if="err" class="banner bad" style="margin-top: 10px">{{ err }}</div>
    </div>

    <h2>Captures <span class="muted" v-if="list">in {{ list.directories.join(', ') }}</span></h2>
    <p v-if="loading" class="muted">Scanning…</p>
    <table v-if="list && list.captures.length">
      <thead><tr><th>Directory</th><th>File</th><th>Size</th><th>Format</th><th>Detection</th><th></th></tr></thead>
      <tbody>
        <template v-for="[dir, caps] in byDir" :key="dir">
          <tr v-for="c in caps" :key="c.path" :class="{ selected: current?.open && current.path === c.path }">
            <td class="muted">{{ dir }}</td>
            <td><strong>{{ c.name }}</strong><span v-if="c.sidecar" class="badge" title="DekTec .xa3 sidecar found">xa3</span></td>
            <td class="mono">{{ bytes(c.size) }}</td>
            <td><span class="badge" :class="{ ok: c.format !== 'unsupported' && c.format !== 'unreadable', bad: c.format === 'unsupported' || c.format === 'unreadable' }">{{ c.format }}</span></td>
            <td class="muted mono" style="max-width: 520px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap" :title="c.detection">{{ c.detection }}</td>
            <td><button class="primary" :disabled="c.format === 'unsupported' || c.format === 'unreadable'" @click="open(c.path)">Open</button></td>
          </tr>
        </template>
      </tbody>
    </table>
    <p v-else-if="list" class="muted">No pcap files found. Start atsc3d with <code>--captures DIR</code>.</p>
  </section>
</template>
