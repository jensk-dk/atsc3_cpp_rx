<script setup lang="ts">
// Shell: navigation + source status + router view (spec 5.18.2).
import { onMounted, onUnmounted } from 'vue'
import { useStackStore } from './stores/stack'
import { seconds } from './util/format'

const stack = useStackStore()
onMounted(() => void stack.start())
onUnmounted(() => stack.stop())
function reload() {
  window.location.reload()
}
</script>

<template>
  <nav class="top">
    <span class="brand">ATSC 3.0 stack</span>
    <router-link to="/source">Source</router-link>
    <router-link to="/services">Services</router-link>
    <router-link to="/signaling">Signaling</router-link>
    <router-link to="/link">Link</router-link>
    <router-link to="/stats">Stats</router-link>
    <router-link to="/player">Player</router-link>
    <router-link to="/epg">EPG</router-link>
    <router-link to="/alerts">Alerts</router-link>
    <span class="spacer" />
    <span class="status">
      <span v-if="!stack.connected" class="badge bad">daemon unreachable</span>
      <template v-else-if="stack.status?.source.open">
        <span class="badge ok">{{ stack.status.source.format }}</span>
        <span>{{ stack.status.source.name }}</span>
        <span class="mono">{{ seconds(stack.status.source.positionNs) }}</span>
        <span v-if="stack.status.source.paused" class="badge">paused</span>
        <span v-if="stack.status.source.finished" class="badge">end</span>
        <span>{{ stack.status.services }} services</span>
      </template>
      <span v-else class="muted">no capture open</span>
    </span>
  </nav>
  <main>
    <div v-if="stack.error" class="banner bad">{{ stack.error }}</div>
    <div v-if="stack.stale" class="banner info">The frontend was rebuilt since this page loaded. <a href="#" @click.prevent="reload">Reload</a> to run the new version.</div>
    <router-view />
  </main>
</template>
