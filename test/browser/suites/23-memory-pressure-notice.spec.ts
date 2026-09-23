import { test, expect } from '../fixtures/base.fixture';
import type { BrowserContext, Page } from '@playwright/test';

// This test validates the current request-governor bundle. A previous device
// Service Worker must not serve a stale chunk after a filesystem-only update.
test.use({ serviceWorkers: 'block' });

// No-PSRAM targets have a single small TCP/static-response budget. Reusing one
// page keeps the suite from re-downloading the multi-megabyte static bundle for
// every assertion and avoids turning test navigation into artificial pressure.
test.describe.configure({ mode: 'serial' });
let pressureContext: BrowserContext | null = null;
let pressurePage: Page | null = null;

test.beforeAll(async ({ browser }) => {
  pressureContext = await browser.newContext({ serviceWorkers: 'block' });
  pressurePage = await pressureContext.newPage();
  await pressurePage.goto('/', { waitUntil: 'commit' });
});

test.afterAll(async () => {
  await pressureContext?.close().catch(() => {});
  pressureContext = null;
  pressurePage = null;
});

function getPressurePage(): Page {
  if (!pressurePage) throw new Error('pressure test page is not initialized');
  return pressurePage;
}

async function openPressurePage(): Promise<Page> {
  return getPressurePage();
}

/**
 * Suite-23: 受控内存 503 的前端提示
 *
 * 设备在无 PSRAM 或内存压力下会主动拒绝重型 API。这里用浏览器路由
 * 模拟响应，验证这类受控降级不会被显示成泛化的服务故障，同时普通
 * 503 仍保持原有错误提示。
 */
test.describe('Suite-23: 内存压力 503 提示', () => {
  test('MEM-001: 内存护栏 503 显示受控提示，普通 503 不误报 @quick', async () => {
    const page = await openPressurePage();
    await page.route('**/api/memory-pressure-probe', async (route) => {
      await route.fulfill({
        status: 503,
        contentType: 'application/json',
        body: JSON.stringify({
          success: false,
          code: 503,
          message: 'Server low memory, request rejected',
          retryAfter: 5,
        }),
      });
    });
    await page.route('**/api/busy-probe', async (route) => {
      await route.fulfill({
        status: 503,
        contentType: 'application/json',
        body: JSON.stringify({
          success: false,
          code: 503,
          message: 'Server busy, please retry',
        }),
      });
    });

    // A controlled low-memory response may keep the document body open while
    // the server sheds heavy work. `commit` lets the test observe the boot
    // scripts and their explicit readiness markers without waiting for the
    // full static response to drain.
    await page.waitForFunction(() => {
      const browserWindow = window as unknown as { apiGet?: (path: string) => Promise<unknown> };
      const notification = typeof Notification !== 'undefined'
        ? (Notification as unknown as { container?: HTMLElement | null })
        : null;
      return typeof browserWindow.apiGet === 'function'
        && !!notification?.container
        && !!document.getElementById('notification-container');
    });

    await page.evaluate(async () => {
      const browserWindow = window as unknown as { apiGet: (path: string) => Promise<unknown> };
      try {
        await browserWindow.apiGet('/api/memory-pressure-probe');
      } catch {
        // Expected: the mocked endpoint deliberately returns HTTP 503.
      }
    });

    const memoryNotice = page.locator('#notification-container .notification-warning').filter({
      hasText: '设备内存保护',
    });
    await expect(memoryNotice).toContainText('设备当前内存紧张');
    await expect(memoryNotice).toContainText('约 5 秒后可重试');

    await page.evaluate(async () => {
      const browserWindow = window as unknown as { apiGet: (path: string) => Promise<unknown> };
      try {
        await browserWindow.apiGet('/api/busy-probe');
      } catch {
        // Expected: the mocked endpoint deliberately returns HTTP 503.
      }
    });

    await expect(
      page.locator('#notification-container .notification-error').filter({ hasText: '服务不可用' }),
    ).toBeVisible();
  });

  test('MEM-002: 页面 HTML 受控 503 时保留可见降级页面', async () => {
    const page = await openPressurePage();
    await page.waitForFunction(() => {
      const notification = typeof Notification !== 'undefined'
        ? (Notification as unknown as { container?: HTMLElement | null })
        : null;
      return !!notification?.container && !!document.getElementById('notification-container');
    });
    await page.waitForFunction(() => typeof (window as unknown as {
      PageLoader?: { _renderUnavailablePage?: (moduleName: string, error: { message: string }) => boolean };
    }).PageLoader?._renderUnavailablePage === 'function');

    await page.evaluate(() => {
      const loader = (window as unknown as {
        PageLoader: {
          _renderUnavailablePage: (moduleName: string, error: { message: string }) => boolean;
        };
      }).PageLoader;
      loader._renderUnavailablePage('device-control', { message: 'HTTP 503' });
    });

    await expect(page.locator('#device-control-page')).toBeAttached();
    await expect(page.locator('[data-page-fallback="memory-pressure"]'))
      .toContainText('设备当前内存紧张');
  });

  test('MEM-003: 运行时压力会拉长后台刷新间隔', async () => {
    const page = await openPressurePage();
    await page.waitForFunction(() => {
      const browserWindow = window as unknown as {
        apiReportRuntimePressure?: (data: unknown, source?: string) => void;
        apiGetPressureAwareInterval?: (profile: string, fallback: number) => number;
      };
      return typeof browserWindow.apiReportRuntimePressure === 'function'
        && typeof browserWindow.apiGetPressureAwareInterval === 'function';
    });

    const intervals = await page.evaluate(() => {
      const browserWindow = window as unknown as {
        apiReportRuntimePressure: (data: unknown, source?: string) => void;
        apiGetPressureAwareInterval: (profile: string, fallback: number) => number;
      };
      const read = () => browserWindow.apiGetPressureAwareInterval('mqttDeferred', 2000);
      browserWindow.apiReportRuntimePressure({ memory: { guardLevel: 'NORMAL' } }, 'test');
      const normal = read();
      browserWindow.apiReportRuntimePressure({ memory: { guardLevel: 'SEVERE' } }, 'test');
      const severe = read();
      browserWindow.apiReportRuntimePressure({ memory: { guardLevel: 'CRITICAL' } }, 'test');
      const critical = read();
      return { normal, severe, critical };
    });

    expect(intervals.normal).toBe(2000);
    expect(intervals.severe).toBeGreaterThan(intervals.normal);
    expect(intervals.critical).toBeGreaterThan(intervals.severe);
  });

  test('MEM-004: 运行时压力状态条可见且会在恢复后隐藏', async () => {
    const page = await openPressurePage();
    await page.waitForFunction(() => {
      const browserWindow = window as unknown as {
        apiReportRuntimePressure?: (data: unknown, source?: string) => void;
      };
      return typeof browserWindow.apiReportRuntimePressure === 'function';
    });

    await page.evaluate(() => {
      if (document.querySelector('[data-memory-pressure-notice]')) return;
      const notice = document.createElement('div');
      notice.className = 'message message-warning fb-hidden';
      notice.setAttribute('data-memory-pressure-notice', '');
      document.body.appendChild(notice);
    });

    await page.evaluate(() => {
      const browserWindow = window as unknown as {
        apiReportRuntimePressure: (data: unknown, source?: string) => void;
      };
      browserWindow.apiReportRuntimePressure({ memory: { guardLevel: 'SEVERE' } }, 'test');
    });
    const notice = page.locator('[data-memory-pressure-notice]').first();
    await expect(notice).toBeVisible();
    await expect(notice).toContainText('内存保护状态');

    await page.evaluate(() => {
      const browserWindow = window as unknown as {
        apiReportRuntimePressure: (data: unknown, source?: string) => void;
      };
      browserWindow.apiReportRuntimePressure({ memory: { guardLevel: 'NORMAL' } }, 'test');
    });
    await expect(notice).toBeHidden();
  });
});
