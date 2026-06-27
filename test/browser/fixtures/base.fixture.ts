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

/** 测试间隔延迟（防止设备过载，默认 1500ms，可通过 TEST_DELAY_MS 覆盖） */
const INTER_TEST_DELAY_MS = parseInt(process.env.TEST_DELAY_MS || '1500', 10);

/** 崩溃自动复位开关（默认关闭，设置 DEVICE_AUTO_RESET=1 启用） */
const AUTO_RESET_ENABLED = process.env.DEVICE_AUTO_RESET === '1';

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
export async function waitForPageContent(page: Page, selector: string, maxMs = 8000): Promise<void> {
  try {
    await page.waitForFunction(
      (sel) => {
        const el = document.querySelector(sel);
        if (!el) return false;
        // 页面容器存在且有子元素（非空）
        return el.children.length > 0 || el.textContent!.trim().length > 0;
      },
      selector,
      { timeout: maxMs }
    );
  } catch {
    // 降级为短固定等待
    await page.waitForTimeout(2000);
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

  // 全局 dialog 自动接受
  page.on('dialog', async (dialog) => {
    await dialog.accept();
  });

  // 导航到登录页
  await page.goto('/');
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

    // 全局 dialog 自动接受
    page.on('dialog', async (dialog) => {
      await dialog.accept();
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

    // 缓存未命中或失效：执行完整登录
    if (!authenticated) {
      await performLoginAndCapture(page);
      lastFullHealthCheck = Date.now();
    }

    await use(page);

    // 测试结束后：自适应等待设备恢复
    await waitForDeviceReady(page, INTER_TEST_DELAY_MS);
  },

  /** 导航到指定菜单页面（自适应等待内容加载） */
  navigateTo: async ({ authPage: page }, use) => {
    await use(async (pageName: string) => {
      await page.click(`.menu-item[data-page="${pageName}"]`);
      // 等待页面框架加载
      await page.waitForLoadState('domcontentloaded', { timeout: 20_000 });
      // 自适应等待：检测目标页面容器有内容
      await waitForPageContent(page, `#${pageName}-page`, 8000);
    });
  },
});

export { expect };
