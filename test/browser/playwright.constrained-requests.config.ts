import { defineConfig, devices } from '@playwright/test';

const host = process.env.OFFLINE_UI_HOST || '127.0.0.2';
const port = process.env.OFFLINE_UI_PORT || '41739';
const baseURL = `http://${host}:${port}`;

export default defineConfig({
  testDir: './suites',
  testMatch: /33-constrained-request-contract\.spec\.ts/,
  outputDir: '../../artifacts/test-results/constrained-requests',
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 60_000,
  reporter: [['list']],
  webServer: {
    command: 'node offline-ui-server.mjs',
    url: `${baseURL}/api/health`,
    timeout: 120_000,
    reuseExistingServer: false,
  },
  use: {
    ...devices['Desktop Chrome'],
    baseURL,
    serviceWorkers: 'block',
    screenshot: 'only-on-failure',
    trace: 'retain-on-failure',
  },
});
