import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// Dev server proxies the stack's web server (spec 5.17.1 CORS note) so `npm run dev` works.
export default defineConfig({
  plugins: [vue()],
  server: {
    port: 5173,
    proxy: {
      '/api': 'http://127.0.0.1:8080',
      '/media': 'http://127.0.0.1:8080',
      '/ws': { target: 'ws://127.0.0.1:8080', ws: true },
    },
  },
  build: { outDir: 'dist', emptyOutDir: true },
})
