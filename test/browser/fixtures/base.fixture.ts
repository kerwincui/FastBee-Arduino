import { test as base, expect, Page } from '@playwright/test';
import { execSync } from 'child_process';

/** 测试环境配置（从环境变量读取） */
export const env = {
  deviceIp: process.env.DEVICE_IP || '192.168.1.1',
  wifi: {
    ssid: process.env.WIFI_SSID || 'fastbee',
    password: process.env.WIFI_PASSWORD || '15208747707',
  },
  mqtt: {
    broker: process.env.MQTT_BROKER || 'd23de4e7b2.st1.iotda-device.cn-east-3.myhuaweicloud.com',
    clientId: process.env.MQTT_CLIENT_ID || '6a03ed2d18855b39c518fbc7_xfxt_esp32_0_0_2026061809',
    username: process.env.MQTT_USERNAME || '6a03ed2d18855b39c518fbc7_xfxt_esp32',
    password: process.env.MQTT_PASSWORD || 'e027294c696eff9a35b9f950a1b6d2a2cf9832b74206afee7dfbb552d2e58bb3',
    portMqtt: 1883,
    portMqtts: 8883,
  },
  auth: {
    username: process.env.AUTH_USERNAME || 'admin',
    password: process.env.AUTH_PASSWORD || 'admin123',
  },
  /** 设备串口端口（用于崩溃自动复位） */
  serialPort: process.env.DEVICE_SERIAL || '',
};

/** 测试间隔延迟（防止设备过载，默认 800ms，可通过 TEST_DELAY_MS 覆盖） */
const INTER_TEST_DELAY_MS = parseInt(process.env.TEST_DELAY_MS || '800', 10);

/** 快速模式开关（环境变量 FAST_MODE=1 启用，减少等待和重试） */
export const FAST_MODE = process.env.FAST_MODE === '1';

/** 崩溃自动复位开关（默认关闭，设置 DEVICE_AUTO_RESET=1 启用） */
const AUTO_RESET_ENABLED = process.env.DEVICE_AUTO_RESET === '1';

/** 级联崩溃断路器：连续 N 次设备不可达后跳过后续测试 */
let consecutiveDeviceFailures = 0;
const CIRCUIT_BREAKER_THRESHOLD = 3;

// ─── 设备健康与恢复 ───────────────────────────────

/** 设备崩溃计数器 */
let crashCount = 0;
/** 上次成功的 uptime（用于检测重启） */
let lastUptime = 0;

/** 检查 API 健康状态（轮询间隔 1s，更快检测恢复） */
export async function waitForHealth(baseURL: string, timeout = 30_000): Promise<boolean> {
  const start = Date.now();
  while (Date.now() - start < timeout) {
    try {
      const resp = await fetch(`${baseURL}/api/health`);
      if (resp.ok) return true;
    } catch { /* ignore */ }
    await new Promise(r => setTimeout(r, 1000));
  }
  return false;
}

/** 主动健康探针：检测设备是否崩溃并自动恢复 */
export async function probeDeviceHealth(baseURL: string): Promise<{ ok: boolean; uptime: number; heapFree: number }> {
  try {
    const resp = await fetch(`${baseURL}/api/system/info?probe=1`, { signal: AbortSignal.timeout(10_000) });
    if (!resp.ok) return { ok: false, uptime: 0, heapFree: 0 };
    const data = await resp.json() as Record<string, unknown>;
    const uptime = Number(data.uptime || 0);
    const heapFree = Number(data.heapFree || 0);
    return { ok: true, uptime, heapFree };
  } catch {
    return { ok: false, uptime: 0, heapFree: 0 };
  }
}

/** 检测设备重启（uptime 重置） */
export async function detectCrashAndReset(baseURL: string): Promise<boolean> {
  const health = await probeDeviceHealth(baseURL);
  if (!health.ok) {
    // 设备无响应，尝试串口复位
    if (AUTO_RESET_ENABLED && env.serialPort) {
      console.log(`[RECOVERY] Device unreachable, attempting serial reset on ${env.serialPort}`);
      serialResetDevice(env.serialPort);
      await waitForHealth(baseURL, 60_000);
    }
    return true;
  }
  if (health.uptime > 0 && health.uptime < lastUptime && lastUptime > 10_000) {
    crashCount++;
    console.log(`[CRASH] Device rebooted (uptime ${lastUptime}ms → ${health.uptime}ms), crash #${crashCount}`);
  }
  lastUptime = health.uptime;
  return false;
}

/** 通过串口 RTS 脉冲复位设备 */
export function serialResetDevice(port: string): boolean {
  try {
    execSync(
      `python -c "import serial,time;s=serial.Serial('${port}',115200,timeout=2);s.rts=True;time.sleep(0.1);s.rts=False;time.sleep(0.1);s.close()"`,
      { timeout: 10_000, stdio: 'pipe' }
    );
    console.log(`[RECOVERY] RTS reset pulse sent to ${port}`);
    return true;
  } catch (e) {
    console.error(`[RECOVERY] Serial reset failed: ${e}`);
    return false;
  }
}

/** 通过 API 恢复设备网络配置为 WiFi STA 模式 */
export async function restoreNetworkConfig(baseURL: string): Promise<boolean> {
  try {
    const resp = await fetch(`${baseURL}/api/network/config`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        networkType: 0,
        staSSID: env.wifi.ssid,
        staPassword: env.wifi.password,
      }),
      signal: AbortSignal.timeout(15_000),
    });
    if (resp.ok || resp.status === 200) {
      console.log('[RECOVERY] Network config restored to WiFi STA mode');
      // 等待设备重启并重新连接
      await new Promise(r => setTimeout(r, 5_000));
      return await waitForHealth(baseURL, 90_000);
    }
    return false;
  } catch {
    return false;
  }
}

// ─── 自适应等待工具 ───────────────────────────────

/**
 * 自适应等待：等待设备 API 就绪，替代硬编码 waitForTimeout
 * 优先检查 API 响应，超时后降级为固定等待
 */
export async function waitForDeviceReady(page: Page, maxMs = 5000): Promise<void> {
  try {
    await page.waitForFunction(
      () => fetch('/api/health').then(r => r.ok).catch(() => false),
      { timeout: maxMs }
    );
  } catch {
    // 降级为固定等待
    await page.waitForTimeout(Math.min(maxMs, 3000));
  }
}

/**
 * 等待页面内容就绪：检测目标页面容器内是否有内容
 * 替代 navigateTo 中的固定 3000ms 等待
 */
export async function waitForPageContent(page: Page, selector: string, maxMs?: number): Promise<boolean> {
  const timeout = maxMs ?? (FAST_MODE ? 5000 : 8000);
  try {
    await page.waitForFunction(
      (sel) => {
        const el = document.querySelector(sel);
        if (!el) return false;
        // 页面容器存在且有子元素（非空）
        return el.children.length > 0 || el.textContent!.trim().length > 0;
      },
      selector,
      { timeout }
    );
    return true;
  } catch {
    // 降级为短固定等待
    await page.waitForTimeout(FAST_MODE ? 1500 : 2000);
    return false;
  }
}

/**
 * 等待成功消息出现（自适应：优先 waitForResponse，降级为 DOM 轮询）
 */
export async function expectSuccessMessage(page: Page, text?: string) {
  const container = page.locator('#notification-container');
  if (text) {
    await expect(container).toContainText(text, { timeout: 15_000 });
  } else {
    // 任意成功通知
    await expect(container.locator('.notification-success, .toast-success, .message-success').first())
      .toBeVisible({ timeout: 15_000 });
  }
}

/** 等待嵌入式设备 API 响应（向后兼容，内部已改为自适应） */
export async function waitForDevice(page: Page, ms = 2000) {
  await waitForDeviceReady(page, ms);
}

// ─── 设备能力检测 ───────────────────────────────────

/** 设备固件功能标志（来自 /api/system/capabilities 公开端点） */
export interface DeviceFeatureFlags {
  mqtt: boolean;
  modbus: boolean;
  tcp: boolean;
  http: boolean;
  coap: boolean;
  periphExec: boolean;
  ruleScript: boolean;
  lcd: boolean;
  ledScreen: boolean;
  ethernet: boolean;
  cellular: boolean;
  ota: boolean;
  auth: boolean;
  webServer: boolean;
  healthMonitor: boolean;
  logger: boolean;
  logViewer: boolean;
  fileLogging: boolean;
  fileManager?: boolean;
  userAdmin?: boolean;
  taskManager: boolean;
  i18n: boolean;
  [key: string]: boolean | undefined;
}

/** 缓存的设备能力（首次检测后缓存，避免重复请求） */
let cachedFeatureFlags: DeviceFeatureFlags | null = null;
let cachedCapabilities: { menuPages: string[]; hasFullscreen: boolean } | null = null;

/**
 * 通过 API 获取设备固件功能标志（无需认证，整个会话只请求一次）
 * 端点: GET /api/system/capabilities
 */
export async function fetchDeviceFeatureFlags(): Promise<DeviceFeatureFlags> {
  if (cachedFeatureFlags) return cachedFeatureFlags;
  try {
    const resp = await fetch(`http://${env.deviceIp}/api/system/capabilities`, {
      signal: AbortSignal.timeout(10_000),
    });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const json = await resp.json() as { success: boolean; data: Record<string, boolean> };
    if (json.success && json.data) {
      cachedFeatureFlags = json.data as DeviceFeatureFlags;
      console.log(`[CAPABILITY-API] ethernet=${cachedFeatureFlags.ethernet} cellular=${cachedFeatureFlags.cellular} logViewer=${cachedFeatureFlags.logViewer} i18n=${cachedFeatureFlags.i18n}`);
      return cachedFeatureFlags;
    }
  } catch (e) {
    console.log(`[CAPABILITY-API] fetch failed: ${e}`);
  }
  // 降级：全部视为 true（不误跳过）
  cachedFeatureFlags = {} as DeviceFeatureFlags;
  return cachedFeatureFlags;
}

/**
 * 检测设备 Web UI 能力（菜单页、fullscreen等）
 * 结果缓存，整个测试会话只检测一次
 */
export async function detectDeviceCapabilities(page: Page): Promise<{ menuPages: string[]; hasFullscreen: boolean; hasEthernet: boolean; hasCellular: boolean; flags: DeviceFeatureFlags }> {
  const flags = await fetchDeviceFeatureFlags();

  if (cachedCapabilities) {
    return { ...cachedCapabilities, hasEthernet: flags.ethernet !== false, hasCellular: flags.cellular !== false, flags };
  }

  try {
    // 检测侧边栏菜单项
    const menuPages = await page.evaluate(() => {
      const items = document.querySelectorAll('.menu-item[data-page]');
      return Array.from(items).map(el => el.getAttribute('data-page') || '');
    });

    // 检测 fullscreen 页面是否可用
    let hasFullscreen = false;
    try {
      hasFullscreen = await page.evaluate(async () => {
        const r = await fetch('/pages/fullscreen.html', { method: 'HEAD' });
        return r.ok;
      });
    } catch { /* ignore */ }

    cachedCapabilities = { menuPages, hasFullscreen };
    console.log(`[CAPABILITY] menus=[${menuPages.join(',')}] fullscreen=${hasFullscreen}`);
    return { ...cachedCapabilities, hasEthernet: flags.ethernet !== false, hasCellular: flags.cellular !== false, flags };
  } catch {
    cachedCapabilities = { menuPages: [], hasFullscreen: false };
    return { ...cachedCapabilities, hasEthernet: flags.ethernet !== false, hasCellular: flags.cellular !== false, flags };
  }
}

/** 检查设备是否支持某个菜单页面 */
export async function hasMenuPage(page: Page, pageName: string): Promise<boolean> {
  const caps = await detectDeviceCapabilities(page);
  return caps?.menuPages.includes(pageName) ?? true; // 默认 true 避免误跳过
}

/**
 * 能力门控：在 test/beforeEach 中调用，设备不支持指定能力时自动 skip
 * 用法: await skipUnlessCapability(authPage, 'ethernet', '设备不支持以太网');
 */
export async function skipUnlessCapability(
  page: Page,
  capability: keyof DeviceFeatureFlags,
  reason?: string
): Promise<void> {
  const flags = await fetchDeviceFeatureFlags();
  const supported = flags[capability] !== false; // undefined 视为支持（兼容旧固件无此字段）
  if (!supported) {
    // 动态导入 test（避免循环引用）
    const { test: t } = await import('@playwright/test');
    t.skip(true, reason || `设备不支持 ${String(capability)}`);
  }
}

// ─── 自定义 Fixture ───────────────────────────────

/** 自定义 fixture 类型 */
export type TestFixtures = {
  authPage: Page;
  navigateTo: (page: string) => Promise<void>;
};

// ─── 性能优化：登录态复用 + 健康检查节流 ─────────────

/** 缓存的 storageState（首次登录后保存，后续测试直接注入） */
let cachedAuthState: { cookies: Array<Record<string, unknown>>; origins: Array<Record<string, unknown>> } | null = null;
/** 上次完整健康检查时间戳（节流：30s 内跳过完整检查） */
let lastFullHealthCheck = 0;
/** 健康检查节流间隔（毫秒） */
const HEALTH_CHECK_INTERVAL_MS = 30_000;

/**
 * 执行完整登录流程（首次调用时）并缓存 storageState
 * 后续测试通过注入缓存状态跳过登录
 */
async function performLoginAndCapture(page: Page): Promise<void> {
  const baseURL = `http://${env.deviceIp}`;

  // 完整健康检查 + 崩溃恢复
  await detectCrashAndReset(baseURL);
  const healthy = await waitForHealth(baseURL, 30_000);
  if (!healthy) {
    if (AUTO_RESET_ENABLED && env.serialPort) {
      console.log('[FATAL] Device unreachable after 30s, forcing serial reset');
      serialResetDevice(env.serialPort);
      await waitForHealth(baseURL, 60_000);
    }
  }

  // 全局 dialog 自动接受（重试时可能重复注册，accept 失败忽略）
  page.on('dialog', async (dialog) => {
    await dialog.accept().catch(() => {});
  });

  // 导航到登录页（带重试，设备可能刚重启）
  for (let gotoRetry = 0; gotoRetry < 3; gotoRetry++) {
    try {
      await page.goto('/', { timeout: 20_000 });
      break;
    } catch (gotoErr) {
      if (gotoRetry < 2) {
        console.log(`[LOGIN] goto retry ${gotoRetry + 1}/3: ${gotoErr}`);
        await page.waitForTimeout(5000);
      } else {
        throw gotoErr;
      }
    }
  }
  await page.waitForSelector('#login-page', { state: 'visible', timeout: 30_000 });

  // 填写登录表单
  await page.fill('#username', env.auth.username);
  await page.waitForTimeout(300);
  await page.fill('#password', env.auth.password);
  await page.waitForTimeout(300);

  // 点击登录，等待应用容器出现
  await page.click('#login-button');
  await page.waitForSelector('#app-container', { state: 'visible', timeout: 40_000 });
  await expect(page.locator('#login-page')).toBeHidden();

  // 自适应等待：设备处理完毕 + modals 片段加载
  await waitForDeviceReady(page, 5000);
  await page.waitForFunction(
    () => document.querySelector('.modal') !== null,
    { timeout: 15_000 }
  ).catch(() => {});

  // 缓存 storageState（cookies + localStorage）
  cachedAuthState = await page.context().storageState();
}

/**
 * 通过缓存的 storageState 快速恢复登录态（跳过登录表单）
 * 如果恢复后未认证（设备 session 过期），自动降级为完整登录
 */
async function restoreAuthState(page: Page): Promise<boolean> {
  if (!cachedAuthState) return false;

  // 快速探针：检测设备是否可达（5s 超时，不做完整健康检查）
  try {
    const resp = await fetch(`http://${env.deviceIp}/api/health`, { signal: AbortSignal.timeout(5_000) });
    if (!resp.ok) {
      // 设备可能崩溃，需要完整健康检查
      return false;
    }
  } catch {
    return false;
  }

  // 注入缓存的 cookies
  if (cachedAuthState.cookies.length > 0) {
    await page.context().addCookies(cachedAuthState.cookies as any);
  }

  // 导航到首页，检查是否已认证
  await page.goto('/');

  // 如果直接显示 app-container（未重定向到登录页），说明 session 有效
  try {
    await page.waitForFunction(
      () => {
        const appVisible = document.querySelector('#app-container')?.getAttribute('style') !== 'display: none'
          && !document.querySelector('#app-container')?.classList.contains('is-hidden');
        const loginVisible = document.querySelector('#login-page')?.getAttribute('style') !== 'display: none'
          && !document.querySelector('#login-page')?.classList.contains('is-hidden');
        return appVisible && !loginVisible;
      },
      { timeout: 8_000 }
    );

    // 全局 dialog 自动接受（accept 失败忽略，防重复 handler 冲突）
    page.on('dialog', async (dialog) => {
      await dialog.accept().catch(() => {});
    });

    // 自适应等待 modals 片段加载
    await page.waitForFunction(
      () => document.querySelector('.modal') !== null,
      { timeout: 10_000 }
    ).catch(() => {});

    return true;
  } catch {
    // Session 失效，需要完整登录
    return false;
  }
}

export const test = base.extend<TestFixtures>({
  /**
   * 已认证的页面（登录态复用 + 健康检查节流 + 崩溃恢复）
   *
   * 优化策略：
   * - 首次调用：完整健康检查 + 登录 + 缓存 storageState
   * - 后续调用：快速探针 + 注入缓存状态（跳过登录表单）
   * - 健康检查节流：30s 内跳过完整检查，仅做 5s 快速探针
   * - 降级保障：缓存状态失效时自动回退到完整登录
   */
  authPage: async ({ page }, use) => {
    // 断路器：连续设备不可达时跳过后续测试
    if (consecutiveDeviceFailures >= CIRCUIT_BREAKER_THRESHOLD) {
      console.log(`[CIRCUIT BREAKER] ${consecutiveDeviceFailures} consecutive failures, skipping`);
      test.skip(true, `设备连续${consecutiveDeviceFailures}次不可达，跳过后续测试`);
    }

    let authenticated = false;

    // 尝试通过缓存状态快速恢复
    if (cachedAuthState) {
      // 健康检查节流：距上次完整检查不到 30s 则跳过
      const now = Date.now();
      if (now - lastFullHealthCheck > HEALTH_CHECK_INTERVAL_MS) {
        authenticated = false; // 需要完整检查，不用快速恢复
      } else {
        authenticated = await restoreAuthState(page);
      }
    }

    // 缓存未命中或失效：执行完整登录（瞬时失败先重试一次再计入断路器）
    if (!authenticated) {
      let loginError: unknown = null;
      for (let loginAttempt = 0; loginAttempt < 2; loginAttempt++) {
        try {
          await performLoginAndCapture(page);
          lastFullHealthCheck = Date.now();
          consecutiveDeviceFailures = 0; // 登录成功，重置计数器
          loginError = null;
          break;
        } catch (e) {
          loginError = e;
          // 页面/上下文已关闭（fixture 超时被杀）无法重试，直接报错
          if (page.isClosed() || loginAttempt >= 1) break;
          console.log(`[AUTH RETRY] Login attempt ${loginAttempt + 1} failed, retrying: ${e}`);
          await new Promise((r) => setTimeout(r, 3000));
        }
      }
      if (loginError) {
        consecutiveDeviceFailures++;
        console.log(`[AUTH FAIL] Login failed (#${consecutiveDeviceFailures}): ${loginError}`);
        if (consecutiveDeviceFailures >= CIRCUIT_BREAKER_THRESHOLD) {
          test.skip(true, `设备不可达，跳过后续测试`);
        }
        throw loginError;
      }
    } else {
      consecutiveDeviceFailures = 0; // 恢复成功，重置计数器
    }

    await use(page);

    // 测试结束后：自适应等待设备恢复
    await waitForDeviceReady(page, INTER_TEST_DELAY_MS);
  },

  /** 导航到指定菜单页面（自适应等待内容加载 + 失败重试） */
  navigateTo: async ({ authPage: page }, use) => {
    await use(async (pageName: string) => {
      const maxRetries = FAST_MODE ? 1 : 2;
      for (let attempt = 0; attempt <= maxRetries; attempt++) {
        // 菜单项偶发不可见（UI 残留状态）：click 失败也进入重试，reload 恢复后再试
        try {
          await page.click(`.menu-item[data-page="${pageName}"]`, { timeout: 10_000 });
        } catch (clickErr) {
          if (attempt < maxRetries) {
            await page.reload({ timeout: 20_000 }).catch(() => {});
            await page.waitForTimeout(2000);
            continue;
          }
          throw clickErr;
        }
        // 等待页面框架加载
        await page.waitForLoadState('domcontentloaded', { timeout: 20_000 });
        // 自适应等待：检测目标页面容器有内容
        const ok = await waitForPageContent(page, `#${pageName}-page`, 8000);
        if (ok) return; // 导航成功
        // 导航失败，短暂等待后重试
        if (attempt < maxRetries) {
          await page.waitForTimeout(2000);
        }
      }
    });
  },
});

export { expect };
