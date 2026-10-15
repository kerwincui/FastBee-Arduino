import { defineConfig, devices, type ReporterDescription } from '@playwright/test';
import path from 'node:path';

/**
 * FastBee-Arduino 浏览器自动化测试配置
 *
 * 用法:
 *   DEVICE_IP=192.168.1.100 npx playwright test          # 全部测试
 *   DEVICE_IP=192.168.1.100 npx playwright test --project=smoke  # 仅冒烟测试
 *   DEVICE_IP=192.168.1.100 npx playwright test --project=core   # 仅核心功能
 *   npx playwright test --headed                          # 有头模式（可视化）
 *
 * 环境变量:
 *   DEVICE_IP       - 设备 IP 地址（必须）
 *   DEVICE_PORT     - 设备端口（默认 80）
 *   WIFI_SSID       - WiFi SSID（网络写入测试必须显式提供）
 *   WIFI_PASSWORD   - WiFi 密码（网络写入测试必须显式提供）
 *   MQTT_BROKER     - MQTT Broker 地址
 *   MQTT_CLIENT_ID  - MQTT 客户端 ID
 *   MQTT_USERNAME   - MQTT 用户名
 *   MQTT_PASSWORD   - MQTT 密码
 *   DEVICE_SERIAL   - 设备串口端口（用于崩溃自动复位）
 *   DEVICE_AUTO_RESET - 启用崩溃自动复位（设为 1）
 *   TEST_DELAY_MS   - 测试间隔延迟（默认 1500ms）
 *
 * 性能优化:
 *   - storageState 登录态复用：首次登录缓存后，后续测试跳过登录表单
 *   - 健康检查节流：30s 内跳过完整健康检查，仅做 5s 快速探针
 *   - 自适应等待：检测设备 API 就绪而非固定等待
 */

const deviceIp = process.env.DEVICE_IP || '192.168.1.1';
const devicePort = process.env.DEVICE_PORT || '80';
const externalOutputDir = process.env.FASTBEE_PLAYWRIGHT_OUTPUT_DIR?.trim();
const externalReportDir = process.env.FASTBEE_PLAYWRIGHT_REPORT_DIR?.trim();
const reporters: ReporterDescription[] = externalReportDir
  ? [
      ['html', { outputFolder: path.join(externalReportDir, 'html'), open: 'never' }],
      ['json', { outputFile: path.join(externalReportDir, 'results.json') }],
      ['list'],
    ]
  : [
      ['html', { outputFolder: 'reports/html', open: 'never' }],
      ['json', { outputFile: 'reports/results.json' }],
      ['list'],
    ];

export default defineConfig({
  testDir: './suites',
  outputDir: externalOutputDir || '../../artifacts/test-results',
  fullyParallel: false, // 嵌入式设备串行，避免并发压力
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 1,
  workers: 1, // 单 worker，设备资源有限
  timeout: 90_000, // 嵌入式设备首次认证/刷新可能需要更长时间
  reporter: reporters,
  use: {
    baseURL: `http://${deviceIp}:${devicePort}`,
    screenshot: 'only-on-failure',
    trace: 'off',
    actionTimeout: 20_000,
    navigationTimeout: 30_000,
    extraHTTPHeaders: {
      'Accept-Language': 'zh-CN',
    },
  },
  projects: [
    // ─── 优先级分层项目（P0-P4） ─────────────────

    // P0 - 冒烟测试：每次构建必跑
    {
      name: 'smoke',
      testMatch: /01-auth|02-dashboard|22-feature-config|23-memory-pressure-notice|24-web-smooth-access|25-mqtt-status-feedback/,
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
      testMatch: /05-peripheral|06-periph-exec|08-modbus|10-rule-script|17-perf|19-ir-remote|20-rf-remote|22-feature-config/,
      use: { ...devices['Desktop Chrome'] },
    },
    // P3 - 辅助功能：大屏/日志/用户/联动
    {
      name: 'auxiliary',
      testMatch: /09-fullscreen|11-logs|13-users|18-linkage/,
      use: { ...devices['Desktop Chrome'] },
    },
    // P4 - 回归测试：集成/UI/资源加载重试
    {
      name: 'regression',
      testMatch: /14-integration|15-ui-regression|21-asset-retry/,
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
    // 独立网页连通性/静态资源完整性测试
    {
      name: 'web',
      testMatch: /24-web-smooth-access/,
      use: { ...devices['Desktop Chrome'] },
    },
  ],
});
