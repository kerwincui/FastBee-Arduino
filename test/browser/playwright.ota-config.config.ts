import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './suites', testMatch: /39-device-ota-config\.spec\.ts/,
  outputDir: '../../artifacts/test-results/ota-config', workers: 1, retries: 0,
  reporter: [['list']], use: { browserName: 'chromium', headless: true },
});
