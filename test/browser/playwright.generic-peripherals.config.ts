import { defineConfig, devices } from '@playwright/test';

// Mount the real modules and markup without rebuilding the shared data/www tree.
export default defineConfig({
  testDir: './suites',
  testMatch: /28-generic-peripherals\.spec\.ts/,
  outputDir: './test-results/generic-peripherals',
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 45_000,
  reporter: [['list']],
  use: { ...devices['Desktop Chrome'], screenshot: 'only-on-failure', trace: 'retain-on-failure' },
});
