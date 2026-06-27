import { defineConfig } from '@playwright/test';

/**
 * FastBee-Arduino 快速浏览器测试配置
 *
 * 只运行带 @quick 标签的核心测试用例，适用于：
 *   - 开发阶段快速验证
 *   - CI/CD 流水线快速回归
 *   - 固件更新后冒烟检查
 *
 * 用法:
 *   cd test/browser
 *   DEVICE_IP=192.168.5.116 npx playwright test -c playwright.quick.config.ts
 *
 * 对比全量配置:
 *   | 指标       | 全量 (273)  | 快速 (~62) |
 *   |------------|-------------|-----------|
 *   | 测试数量    | 273         | ~62       |
 *   | 预估时间    | 30-37min    | 8-12min   |
 *   | screenshot | only-on-fail| only-on-fail|
 *   | trace      | off         | off       |
 *   | retries    | 1           | 0         |
 *   | 测试间延迟  | 800ms       | 500ms     |
 */

const deviceIp = process.env.DEVICE_IP || '192.168.1.1';
const devicePort = process.env.DEVICE_PORT || '80';
const interTestDelay = parseInt(process.env.INTER_TEST_DELAY_MS || '500', 10);

export default defineConfig({
  testDir: './suites',
  outputDir: './test-results/quick',
  fullyParallel: false,
  forbidOnly: !!process.env.CI,
  retries: 0,         // 快速模式不重试，失败即报告
  workers: 1,
  timeout: 45_000,    // 快速模式缩短超时
  globalSetup: require.resolve('./stability-global-setup'),
  reporter: [
    ['list'],                                        // 实时进度
    ['json', { outputFile: 'reports/quick-results.json' }],
    ['html', { outputFolder: 'reports/quick-html', open: 'never' }],
  ],
  use: {
    baseURL: `http://${deviceIp}:${devicePort}`,
    screenshot: 'only-on-failure',
    trace: 'off',
    actionTimeout: 15_000,
    navigationTimeout: 20_000,
    extraHTTPHeaders: {
      'Accept-Language': 'zh-CN',
    },
  },
  projects: [
    {
      name: 'quick',
      // 匹配所有 spec 文件，通过 --grep @quick 过滤
      testMatch: /\.spec\.ts$/,
      grep: /@quick/,
    },
  ],
});
