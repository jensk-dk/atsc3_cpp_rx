<script setup lang="ts">
// Per-layer counters from /status (spec 5.18.2 "Stats"; charts come with the SSE feed).
import { computed } from 'vue'
import { useStackStore } from '../stores/stack'

const stack = useStackStore()
const layers = computed(() => Object.entries(stack.status?.stats ?? {}))
</script>

<template>
  <section>
    <h1>Statistics</h1>
    <div class="grid">
      <div class="panel" v-for="[layer, counters] in layers" :key="layer">
        <h2 style="margin-top: 0">{{ layer }}</h2>
        <dl class="kv"><template v-for="(v, k) in counters" :key="k"><dt>{{ k }}</dt><dd class="mono">{{ v }}</dd></template></dl>
      </div>
    </div>
    <p v-if="!layers.length" class="muted">No capture open.</p>
  </section>
</template>
