<script setup lang="ts">
// Player view (spec 5.18.2): one PlayerPanel per selected A/V service: dash.js for ROUTE (spec 5.17.3 local DASH
// origin), the MSE push player for MMTP (spec 5.17.4, polled mmt/index.json).
import { computed } from 'vue'
import { useStackStore } from '../stores/stack'
import PlayerPanel from '../components/PlayerPanel.vue'
import { channel } from '../util/format'

const stack = useStackStore()
const playable = computed(() => stack.selectedServices.filter((s) => (s.protocol === 'ROUTE' || s.protocol === 'MMTP') && s.components.some((c) => c.kind === 'video' || c.kind === 'audio')))
const others = computed(() => stack.selectedServices.filter((s) => !playable.value.includes(s)))
</script>

<template>
  <section>
    <h1>Player</h1>
    <div v-if="!stack.open" class="banner info">No capture is open. Pick one on the <router-link to="/source">Source</router-link> page.</div>
    <div v-else-if="!stack.selectedServices.length" class="banner info">No service selected. Select an A/V service (ROUTE or MMTP) on the <router-link to="/services">Services</router-link> page.</div>
    <div v-for="s in playable" :key="s.bsid + '/' + s.serviceId">
      <h2><router-link :to="`/services/${s.bsid}/${s.serviceId}`">{{ channel(s.major, s.minor) }} {{ s.shortName || s.usbdName }}</router-link></h2>
      <PlayerPanel :service="s" />
    </div>
    <p v-if="others.length" class="muted">Selected but not playable here: <span v-for="s in others" :key="s.bsid + '/' + s.serviceId">{{ s.shortName || s.serviceId }} ({{ s.protocol }}) </span>— no video or audio component (NRT-only or signaling-only service).</p>
  </section>
</template>
