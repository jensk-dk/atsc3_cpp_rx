<script setup lang="ts">
// EPG grid (spec 5.18.2 "EPG"): one row per ESG Service fragment, programmes placed on a time axis
// [windowStart, windowStart + windowLen) in Unix seconds; a "now" line when it falls inside the window.
import { computed } from 'vue'
import type { EsgService, Programme } from '../api/types'
import { channel } from '../util/format'

const props = defineProps<{
  services: EsgService[]
  programmes: Programme[]
  windowStart: number
  windowLen: number
  now: number | null
  selectedId: string | null   // contentId + '@' + start of the picked programme
}>()
const emit = defineEmits<{ (e: 'pick', p: Programme): void; (e: 'service', s: EsgService): void }>()

function hhmm(unixS: number): string {
  const d = new Date(unixS * 1000)
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
}

const ticks = computed(() => {
  // one tick per hour (per 30 min under 4 h, per 2 h above 12 h)
  const step = props.windowLen <= 4 * 3600 ? 1800 : props.windowLen > 12 * 3600 ? 7200 : 3600
  const first = Math.ceil(props.windowStart / step) * step
  const out: { t: number; left: number; label: string }[] = []
  for (let t = first; t < props.windowStart + props.windowLen; t += step) {
    out.push({ t, left: ((t - props.windowStart) / props.windowLen) * 100, label: hhmm(t) })
  }
  return out
})

const byService = computed(() => {
  const m = new Map<string, Programme[]>()
  for (const p of props.programmes) {
    const list = m.get(p.serviceId) ?? []
    list.push(p)
    m.set(p.serviceId, list)
  }
  return m
})

function box(p: Programme): { left: string; width: string } {
  const ws = props.windowStart
  const we = ws + props.windowLen
  const s = Math.max(p.start, ws)
  const e = Math.min(p.end, we)
  const left = ((s - ws) / props.windowLen) * 100
  const width = Math.max(((e - s) / props.windowLen) * 100, 0.3)
  return { left: `${left}%`, width: `${width}%` }
}
function isNow(p: Programme): boolean {
  return props.now !== null && p.start <= props.now && props.now < p.end
}
function key(p: Programme): string {
  return `${p.contentId}@${p.start}`
}
const nowLeft = computed(() => {
  if (props.now === null) return null
  if (props.now < props.windowStart || props.now >= props.windowStart + props.windowLen) return null
  return ((props.now - props.windowStart) / props.windowLen) * 100
})
</script>

<template>
  <div class="epg">
    <div class="epg-head">
      <div class="epg-label muted">Channel</div>
      <div class="epg-axis">
        <span v-for="t in ticks" :key="t.t" class="tick" :style="{ left: t.left + '%' }">{{ t.label }}</span>
      </div>
    </div>
    <div v-for="s in services" :key="s.id" class="epg-row">
      <div class="epg-label">
        <img v-if="s.icon" :src="s.icon" :alt="s.name" class="epg-icon" loading="lazy" />
        <div class="epg-label-text">
          <a href="#" class="epg-name" @click.prevent="emit('service', s)" :title="s.id">
            <span class="mono">{{ channel(s.major, s.minor) }}</span> {{ s.name || s.shortName || '(unnamed)' }}
          </a>
          <div class="muted small">
            <span v-if="s.bsid !== null">{{ s.bsid }}/{{ s.serviceId }}</span>
            <span v-else>not in SLT</span>
            <span v-if="s.selected" class="badge ok">selected</span>
          </div>
        </div>
      </div>
      <div class="epg-lane">
        <div
          v-for="p in byService.get(s.id) ?? []"
          :key="key(p)"
          class="prog"
          :class="{ now: isNow(p), picked: key(p) === selectedId, unresolved: !p.title }"
          :style="box(p)"
          :title="`${hhmm(p.start)}–${hhmm(p.end)} ${p.title || p.contentId}`"
          @click="emit('pick', p)"
        >
          <span class="prog-time mono">{{ hhmm(p.start) }}</span>
          <span class="prog-title">{{ p.title || '(content fragment not received)' }}</span>
        </div>
        <div v-if="nowLeft !== null" class="now-line" :style="{ left: nowLeft + '%' }" />
      </div>
    </div>
    <div v-if="!services.length" class="muted" style="padding: 10px">No Service fragments in the guide yet.</div>
  </div>
</template>

<style scoped>
.epg { background: var(--panel); border: 1px solid var(--border); border-radius: 8px; overflow: hidden; }
.epg-head, .epg-row { display: grid; grid-template-columns: 200px 1fr; border-bottom: 1px solid var(--border); }
.epg-row:last-child { border-bottom: none; }
.epg-label { padding: 6px 10px; display: flex; gap: 8px; align-items: center; border-right: 1px solid var(--border); min-height: 56px; overflow: hidden; }
.epg-label-text { min-width: 0; }
.epg-name { color: var(--text); font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; display: block; }
.epg-icon { width: 44px; height: 33px; object-fit: contain; background: #fff; border-radius: 4px; flex: none; }
.small { font-size: 11px; }
.epg-axis { position: relative; height: 24px; }
.tick { position: absolute; top: 4px; font-size: 11px; color: var(--muted); transform: translateX(-50%); }
.tick::before { content: ''; position: absolute; left: 50%; top: 16px; width: 1px; height: 6px; background: var(--border); }
.epg-lane { position: relative; min-height: 56px; overflow: hidden; }
.prog { position: absolute; top: 6px; bottom: 6px; background: rgba(77,163,255,.14); border: 1px solid rgba(77,163,255,.45); border-radius: 5px; padding: 4px 6px; overflow: hidden; cursor: pointer; display: flex; flex-direction: column; gap: 2px; font-size: 12px; box-sizing: border-box; }
.prog:hover { background: rgba(77,163,255,.26); }
.prog.now { border-color: var(--ok); background: rgba(60,207,122,.16); }
.prog.picked { outline: 2px solid var(--accent); }
.prog.unresolved { border-style: dashed; color: var(--muted); }
.prog-time { font-size: 10px; color: var(--muted); }
.prog-title { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.now-line { position: absolute; top: 0; bottom: 0; width: 2px; background: var(--bad); pointer-events: none; }
</style>
