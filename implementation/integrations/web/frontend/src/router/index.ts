import { createRouter, createWebHistory } from 'vue-router'

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', redirect: '/services' },
    { path: '/services', component: () => import('../views/ServicesView.vue') },
    { path: '/services/:bsid/:sid', component: () => import('../views/ServiceDetailView.vue'), props: true },
    { path: '/player', component: () => import('../views/PlayerView.vue') },
    { path: '/epg', component: () => import('../views/EpgView.vue') },
    { path: '/alerts', component: () => import('../views/AlertsView.vue') },
    { path: '/signaling', component: () => import('../views/SignalingView.vue') },
    { path: '/link', component: () => import('../views/LinkView.vue') },
    { path: '/stats', component: () => import('../views/StatsView.vue') },
    { path: '/source', component: () => import('../views/SourceView.vue') },
  ],
})
