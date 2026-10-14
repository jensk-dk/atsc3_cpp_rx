// Polls the daemon: long-polls /status (returns when the engine's state version changes or after 1 s),
// then refreshes the service list when the version moved.
import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { api } from '../api/client'
import type { Service, Status } from '../api/types'

export const useStackStore = defineStore('stack', () => {
  const status = ref<Status | null>(null)
  const services = ref<Service[]>([])
  const error = ref('')
  const connected = ref(false)
  const stale = ref(false)   // the served bundle differs from the one this page loaded (frontend rebuilt)
  let running = false
  let lastVersion = -1
  let lastBundleCheck = 0

  // The SPA keeps running the JavaScript it loaded; after `npm run build` the daemon serves a new bundle under a
  // new hash. Compare the entry script in /index.html with ours every 20 s and ask for a reload when it changed.
  async function checkBundle() {
    const now = Date.now()
    if (now - lastBundleCheck < 20_000) return
    lastBundleCheck = now
    try {
      const mine = [...document.querySelectorAll('script[type="module"]')].map((s) => (s as HTMLScriptElement).src.split('/').pop()).find(Boolean)
      if (!mine) return
      const html = await (await fetch('/index.html', { cache: 'no-store' })).text()
      const served = /assets\/(index-[A-Za-z0-9_-]+\.js)/.exec(html)?.[1]
      if (served && served !== mine) stale.value = true
    } catch {
      // dev server or API-only daemon: nothing to compare
    }
  }

  async function tick() {
    try {
      const st = await api.status(status.value?.version, 1000)
      connected.value = true
      error.value = ''
      status.value = st
      if (st.version !== lastVersion) {
        lastVersion = st.version
        services.value = await api.services()
      }
      void checkBundle()
    } catch (e) {
      connected.value = false
      error.value = (e as Error).message
      await new Promise((r) => setTimeout(r, 1500))
    }
  }

  async function start() {
    if (running) return
    running = true
    while (running) await tick()
  }
  function stop() {
    running = false
  }

  async function refresh() {
    services.value = await api.services()
    status.value = await api.status()
  }

  async function select(bsid: number, sid: number, on: boolean) {
    await api.select(bsid, sid, on)
    await refresh()
  }

  const open = computed(() => status.value?.source.open ?? false)
  const selectedServices = computed(() => services.value.filter((s) => s.selected))

  return { status, services, error, connected, stale, open, selectedServices, start, stop, refresh, select }
})
