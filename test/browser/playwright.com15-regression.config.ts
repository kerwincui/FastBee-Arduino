import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './suites',
  testMatch: /(?:35-device-control-layout|36-device-control-measurements|37-modbus-relay-status|38-modbus-relay-admission|39-device-ota-config|40-device-control-actions|41-device-config-functions|44-ota-failure-lifecycle)\.spec\.ts/,
  outputDir: './test-results/com15-regression',
  workers: 1,
  retries: 0,
  timeout: 30000,
  reporter: [['list']],
  use: { browserName: 'chromium', headless: true },
});
