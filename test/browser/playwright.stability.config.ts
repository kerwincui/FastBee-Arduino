import { defineConfig, devices } from '@playwright/test';

/**
 * FastBee-Arduino 长期稳定性测试配置
 *
 * 设计目标：
 *   - 跑完几乎所有测试套件（P0-P4 全覆盖），验证设备在长时间压力下的稳定性
 *   - 多 worker 并行提升效率（2 workers + fullyParallel）
 *   - 多轮循环模拟长期运行（默认 5 轮，可通过 STABILITY_ROUNDS 环境变量覆盖）
 *   - 内置设备健康监控：每轮结束后检测崩溃、内存泄漏、uptime 重置
 *
 * 用法:
 *   DEVICE_IP=192.168.1.100 npx playwright test -c playwright.stability.config.ts
 *   STABILITY_ROUNDS=10 DEVICE_IP=192.168.1.100 npx playwright test -c playwright.stability.config.ts
 *   DEVICE_IP=192.168.1.100 DEVICE_AUTO_RESET=1 DEVICE_SERIAL=COM15 npx playwright test -c playwright.stability.config.ts
 *
 * 环境变量:
 *   DEVICE_IP         - 设备 IP 地址（必须）
 *   DEVICE_PORT       - 设备端口（默认 80）
 *   STABILITY_ROUNDS  - 稳定性测试轮数（默认 5）
 *   STABILITY_WORKERS - 并行 worker 数（默认 2）
 *   TEST_DELAY_MS     - 测试间隔延迟（默认 3000ms，稳定性测试适当加大）
 *   DEVICE_SERIAL     - 设备串口端口（用于崩溃自动复位）
 *   DEVICE_AUTO_RESET - 启用崩溃自动复位（设为 1）
 */

const deviceIp = process.env.DEVICE_IP || '192.168.1.1';
const devicePort = process.env.DEVICE_PORT || '80';
const stabilityRounds = parseInt(process.env.STABILITY_ROUNDS || '5', 10);
const stabilityWorkers = parseInt(process.env.STABILITY_WORKERS || '2', 10);

export default defineConfig({
  testDir: './suites',
  outputDir: './test-results/stability',
  fullyParallel: true, // 稳定性测试启用并行，多 worker 同时工作
  forbidOnly: !!process.env.CI,
  retries: 1, // 稳定性测试只重试 1 次（过多重试掩盖真实问题）
  workers: stabilityWorkers,
  timeout: 90_000, // 稳定性测试适当放宽超时（90s）
  // 多轮循环：通过 globalSetup 实现重复执行
  globalSetup: require.resolve('./stability-global-setup'),
  reporter: [
    ['html', { outputFolder: 'reports/stability-html', open: 'never' }],
    ['json', { outputFile: 'reports/stability-results.json' }],
    ['list'],
  ],
  use: {
    baseURL: `http://${deviceIp}:${devicePort}`,
    screenshot: 'on-first-failure', // 仅失败时截图，减少稳定性测试 IO 压力
    trace: 'on-first-retry',
    actionTimeout: 25_000,
    navigationTimeout: 35_000,
    extraHTTPHeaders: {
      'Accept-Language': 'zh-CN',
    },
  },
  projects: [
    // ─── 稳定性全量测试项目 ─────────────────────────

    // stability-all: 覆盖 P0-P4 所有测试套件
    {
      name: 'stability-all',
      testMatch: /01-auth|02-dashboard|03-network|04-device|05-peripheral|06-periph-exec|07-mqtt|08-modbus|09-fullscreen|10-rule-script|11-logs|13-users|14-integration|15-ui-regression|16-net-mqtt-switch|17-perf|18-linkage/,
      use: { ...devices['Desktop Chrome'] },
    },
  ],
});
