import { defineConfig, devices } from '@playwright/test'

// E2E against atsc3d + pcap virtual tuner (spec 5.18.3). The `hevc` project is meant for a
// self-hosted runner with hardware HEVC decoding (Chrome channel); CI runs `chromium` only.
export default defineConfig({
  testDir: './e2e',
  timeout: 120_000,
  // Every spec opens its own capture on the one daemon it is pointed at; parallel workers would switch the source
  // under each other, so the suite runs serially.
  workers: 1,
  fullyParallel: false,
  use: { baseURL: process.env.ATSC3_WEB_URL ?? 'http://127.0.0.1:8080' },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
    { name: 'firefox', use: { ...devices['Desktop Firefox'] } },
    { name: 'hevc', use: { ...devices['Desktop Chrome'], channel: 'chrome' }, grep: /@hevc/ },
  ],
})
