import { defineConfig, devices } from '@playwright/test';

/**
 * FastBee-Arduino 浏览器自动化测试配置
 *
 * 用法:
 *   DEVICE_IP=192.168.1.100 npx playwright test          # 全部测试
 *   DEVICE_IP=192.168.1.100 npx playwright test --project=smoke  # 仅冒烟测试
 *   DEVICE_IP=192.168.1.100 npx playwright test --project=readonly  # 只读测试（可 2 workers）
 *   DEVICE_IP=192.168.1.100 npx playwright test --project=write  # 写操作测试（必须 1 worker）
 *   npx playwright test --headed                          # 有头模式（可视化）
 *
 * 环境变量:
 *   DEVICE_IP       - 设备 IP 地址（必须）
 *   DEVICE_PORT     - 设备端口（默认 80）
 *   WIFI_SSID       - WiFi SSID（默认 fastbee）
 *   WIFI_PASSWORD   - WiFi 密码（默认 15208747707）
 *   MQTT_BROKER     - MQTT Broker 地址
 *   MQTT_CLIENT_ID  - MQTT 客户端 ID
 *   MQTT_USERNAME   - MQTT 用户名
 *   MQTT_PASSWORD   - MQTT 密码
 *   DEVICE_SERIAL   - 设备串口端口（用于崩溃自动复位）
 *   DEVICE_AUTO_RESET - 启用崩溃自动复位（设为 1）
 *   TEST_DELAY_MS   - 测试间隔延迟（默认 3000ms）
 */

const deviceIp = process.env.DEVICE_IP || '192.168.1.1';
const devicePort = process.env.DEVICE_PORT || '80';

export default defineConfig({
  testDir: './suites',
  outputDir: './test-results',
  fullyParallel: false, // 嵌入式设备串行，避免并发压力
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 1,
  workers: 1, // 单 worker，设备资源有限
  timeout: 120_000, // 嵌入式设备测试需要更长时间（登录+导航+API+等待）
  reporter: [
    ['html', { outputFolder: 'reports/html', open: 'never' }],
    ['json', { outputFile: 'reports/results.json' }],
    ['list'],
  ],
  use: {
    baseURL: `http://${deviceIp}:${devicePort}`,
    screenshot: 'on',
    trace: 'on-first-retry',
    actionTimeout: 30_000,
    navigationTimeout: 60_000,
    // 嵌入式设备 API 响应较慢
    extraHTTPHeaders: {
      'Accept-Language': 'zh-CN',
    },
  },
  projects: [
    // ─── 优先级分层项目（向后兼容） ───────────────

    // P0 - 冒烟测试：每次构建必跑
    {
      name: 'smoke',
      testMatch: /01-auth|02-dashboard/,
      use: { ...devices['Desktop Chrome'] },
    },
    // P1 - 核心功能：网络/设备/MQTT
    {
      name: 'core',
      testMatch: /03-network|04-device|07-mqtt|16-net-mqtt-switch/,
      use: { ...devices['Desktop Chrome'] },
    },
    // P2 - 功能测试：外设/Modbus/规则/性能
    {
      name: 'features',
      testMatch: /05-peripheral|06-periph-exec|08-modbus|10-rule-script|17-perf/,
      use: { ...devices['Desktop Chrome'] },
    },
    // P3 - 辅助功能：大屏/日志/文件/用户/联动
    {
      name: 'auxiliary',
      testMatch: /09-fullscreen|11-logs|12-files|13-users|18-linkage/,
      use: { ...devices['Desktop Chrome'] },
    },
    // P4 - 回归测试：集成/UI
    {
      name: 'regression',
      testMatch: /14-integration|15-ui-regression/,
      use: { ...devices['Desktop Chrome'] },
    },

    // ─── 读写分离项目（优化并发策略） ───────────────

    // 只读测试：仅读取设备状态/信息，不修改配置
    // 理论上可用 2 workers（但嵌入式设备仍建议 1）
    {
      name: 'readonly',
      testMatch: /08-modbus|09-fullscreen|11-logs|12-files|13-users|14-integration|15-ui-regression|17-perf/,
      use: { ...devices['Desktop Chrome'] },
    },
    // 写操作测试：修改设备配置（网络/外设/MQTT/规则/联动）
    // 必须 1 worker，防止并发修改导致设备崩溃
    {
      name: 'write',
      testMatch: /03-network|04-device|05-peripheral|06-periph-exec|07-mqtt|10-rule-script|16-net-mqtt-switch|18-linkage/,
      use: { ...devices['Desktop Chrome'] },
    },

    // ─── 独立项目（专用） ──────────────────────────

    // 性能专用
    {
      name: 'perf',
      testMatch: /17-perf/,
      use: { ...devices['Desktop Chrome'] },
    },
    // 联动专用
    {
      name: 'linkage',
      testMatch: /18-linkage/,
      use: { ...devices['Desktop Chrome'] },
    },
  ],
});
