import { defineConfig } from '@playwright/test';
import { constrainedDevicePolicy } from './constrained-device-policy';

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
 * 用例数量会随功能变化，不在源码中维护易过期的固定数字。运行下面的
 * 命令可查看当前快速集，不会连接设备：
 *   npx playwright test -c playwright.quick.config.ts --list
 */

const deviceIp = process.env.DEVICE_IP || '192.168.1.1';
const devicePort = process.env.DEVICE_PORT || '80';
const interTestDelay = parseInt(process.env.INTER_TEST_DELAY_MS || '500', 10);

export default defineConfig({
  testDir: './suites',
  outputDir: './test-results/quick',
  ...constrainedDevicePolicy,
  forbidOnly: !!process.env.CI,
  timeout: 60_000,    // 45s下beforeEach/fixture偶发超限（设备清理恢复期负载），放宽到60s
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
