<script setup lang="ts">
defineProps<{ protocol: string }>()
</script>
<template>
  <span class="badge" :class="{ route: protocol === 'ROUTE', mmtp: protocol === 'MMTP' }">{{ protocol }}</span>
</template>
