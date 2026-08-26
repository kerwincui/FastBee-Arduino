// @ts-check
const { defineConfig, devices } = require('@playwright/test');

// 系统代理污染防护：本机可能配置了 HTTP(S)_PROXY，request fixture（Node 层 HTTP）
// 会把 192.168.5.x 局域网请求发往代理导致连接失败。浏览器侧已有 --no-proxy-server，
// 这里在配置加载时统一清理 Node 侧代理变量，避免每个运行脚本都要手动清 env。
for (const key of ['HTTP_PROXY', 'HTTPS_PROXY', 'http_proxy', 'https_proxy', 'ALL_PROXY', 'all_proxy']) {
  delete process.env[key];
}
process.env.NO_PROXY = '*';
process.env.no_proxy = '*';

/**
 * Read environment variables for device configuration
 * Example: DEVICES="192.168.5.31,192.168.5.14" npm run test:e2e
 * 缺省自动发现子网设备（DHCP 漂移免疫，不再硬编码兜底 IP）
 */
const { resolveDevices } = require('./test/e2e/helpers/fastbee-helpers');
const devicesToTest = resolveDevices();
// 回写 env：worker 进程继承该变量后走同一设备列表，
// 避免主/worker 二次扫描结果不一致导致 "Test not found in worker"（弱链路设备瞬间掉线会使两次扫描结果不同）
if (devicesToTest.length && !process.env.DEVICES) {
  process.env.DEVICES = devicesToTest.join(',');
}

console.log('=== FastBee E2E Test Configuration ===');
console.log(`Testing devices: ${devicesToTest.join(', ')}`);
console.log('=====================================\n');

module.exports = defineConfig({
  testDir: './test/e2e',
  
  /* Run tests in files in parallel */
  fullyParallel: false, // Test one device at a time
  
  /* Fail the build on CI if you accidentally left test.only in the source code. */
  forbidOnly: !!process.env.CI,
  
  /* Retry: CI 2 次；本地 1 次——弱射频链路设备实测存在分钟级通断窗口，
     整测重试一次可消除大量瞬时掉线导致的假性失败（2026-08-21 实测） */
  retries: process.env.CI ? 2 : 1,
  
  /* Opt out of parallel tests on CI. */
  workers: process.env.CI ? 1 : undefined,
  
  /* Reporter to use. See https://playwright.dev/docs/test-reporters */
  reporter: [
    ['html', { outputFolder: 'playwright-report' }],
    ['list'],
    ['json', { outputFile: 'test-results/e2e-results.json' }]
  ],
  
  /* Shared settings for all the projects below. See https://playwright.dev/docs/api/class-testoptions. */
  timeout: 30 * 1000, // 30 seconds per test step
  expect: {
    /**
     * Maximum time expect() should wait for the condition to be met.
     * For slow-loading web pages, increase this value.
     */
    timeout: 15000
  },
  
  /* Use all available browsers */
  use: {
    /* Base URL for tests */
    baseURL: `http://${devicesToTest[0] || '127.0.0.1'}`,
    
    /* Launch args: ignore system proxy so LAN device IPs (192.168.5.x) are reachable */
    launchOptions: {
      args: ['--no-proxy-server']
    },
    
    /* Collect trace when retrying the failed test. See https://playwright.dev/docs/trace-viewer */
    trace: 'on-first-retry',
    
    /* Screenshot on failure */
    screenshot: 'only-on-failure',
    
    /* Video recording on failure */
    video: 'retain-on-failure',
    
    /* Maximum time each action can take */
    actionTimeout: 15000,
    
    /* Operating system for testing */
    headless: true, // Run in headless mode (no browser UI)
  },
  
  /* Configure projects for different browsers */
  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
    {
      name: 'firefox',
      use: { ...devices['Desktop Firefox'] },
    },
    // Uncomment for Safari testing if needed
    // {
    //   name: 'webkit',
    //   use: { ...devices['Desktop Safari'] },
    // },
  ],
  
  /* Folder for test artifacts such as downloads, screenshots, logs and traces */
  outputDir: 'test-results/',
});
