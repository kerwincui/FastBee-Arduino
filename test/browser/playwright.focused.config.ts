import { defineConfig } from '@playwright/test';

/**
 * 聚焦失败用例配置 - 只运行上次失败的测试
 * 用法: npx playwright test --config=playwright.focused.config.ts
 */

const deviceIp = process.env.DEVICE_IP || '192.168.1.1';
const devicePort = process.env.DEVICE_PORT || '80';

export default defineConfig({
  testDir: './suites',
  outputDir: './test-results/focused',
  fullyParallel: false,
  forbidOnly: !!process.env.CI,
  retries: 1,
  workers: 1,
  timeout: 90_000,
  globalSetup: require.resolve('./stability-global-setup'),
  reporter: [
    ['json', { outputFile: 'reports/focused-results.json' }],
    ['line'],
  ],
  use: {
    baseURL: `http://${deviceIp}:${devicePort}`,
    screenshot: 'on-first-failure',
    trace: 'off',
    actionTimeout: 15_000,
    navigationTimeout: 30_000,
  },
  projects: [
    {
      name: 'focused',
      testMatch: /04-device|05-peripheral|07-mqtt/,
    },
  ],
});
