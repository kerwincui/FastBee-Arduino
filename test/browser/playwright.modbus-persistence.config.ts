import { defineConfig } from '@playwright/test';
export default defineConfig({
  testDir: './suites', testMatch: /34-modbus-device-persistence\.spec\.ts/,
  outputDir: '../../artifacts/test-results/modbus-persistence', workers: 1, retries: 0,
  reporter: [['list']], use: { browserName: 'chromium', headless: true },
});
