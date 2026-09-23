import { defineConfig, devices } from '@playwright/test';
export default defineConfig({
  testDir: './suites', testMatch: /(?:33-peripheral-list-diagnostics|34-protocol-direct-entry)\.spec\.ts/,
  outputDir: './test-results/peripheral-list-diagnostics', workers: 1, retries: 0,
  timeout: 30000, reporter: [['list']], use: { ...devices['Desktop Chrome'] }
});

