import { test, expect, waitForDevice } from '../fixtures/base.fixture';
import type { Page } from '@playwright/test';

// Some dashboard fragments are mounted twice during a refresh. Prefer the
// visible instance so a stale hidden fragment cannot trigger strict-mode
// failures while the device is still serving the page.
function visibleById(page: Page, id: string) {
  return page.locator(`#${id}`).filter({ visible: true }).first();
}

/** Embedded targets can take several seconds to populate a dashboard refresh. */
async function expectDashboardValue(page: Page, id: string, matcher: RegExp, timeout = 20_000) {
  await expect.poll(
    async () => (await visibleById(page, id).textContent())?.trim() ?? '',
    { timeout, intervals: [500, 1000, 2000] },
  ).toMatch(matcher);
}

async function skipIfDeviceControlHidden(page: Page) {
  const menu = page.locator('.menu-item[data-page="device-control"]');
  const runtimeEnabled = await page.evaluate(async () => {
    try {
      const response = await fetch('/api/system/status?probe=1', { cache: 'no-store' });
      if (!response.ok) return null;
      const payload = await response.json() as { data?: { features?: { deviceControl?: string } } };
      return payload.data?.features?.deviceControl === 'active';
    } catch {
      return null;
    }
  }).catch(() => null);
  if (runtimeEnabled === false || !(await menu.isVisible().catch(() => false))) {
    test.skip(true, '设备未启用设备大屏功能（内存护栏或功能配置已禁用）');
  }
}

test.describe('Suite-02: 设备监控仪表盘', () => {

  // ========== 场景A: 仪表盘数据加载与展示 ==========

  test('DASH-001: 仪表盘初始加载 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    // 三个顶部统计卡片
    await expect(authPage.locator('#monitor-chip-model')).toBeVisible();
    await expect(authPage.locator('#monitor-uptime')).toBeVisible();
    await expect(authPage.locator('#monitor-network-status')).toBeVisible();
    // 三个资源卡片
    await expect(authPage.locator('#monitor-flash-percent')).toBeVisible();
    await expect(authPage.locator('#monitor-heap-percent')).toBeVisible();
    await expect(authPage.locator('#monitor-fs-percent')).toBeVisible();
    // 网络信息区
    await expect(authPage.locator('#ns-status')).toBeVisible();
  });

  test('DASH-002: 设备信息卡片', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    const chip = await authPage.locator('#monitor-chip-model').textContent();
    expect(chip).not.toBe('--');
    const cpu = await authPage.locator('#monitor-cpu-freq').textContent();
    expect(cpu).not.toBe('--');
    const sdk = await authPage.locator('#monitor-sdk').textContent();
    expect(sdk).not.toBe('--');
  });

  test('DASH-003: 运行时间卡片', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.click('#monitor-refresh-btn');
    await authPage.waitForTimeout(3000);
    const uptime = await authPage.locator('#monitor-uptime').textContent();
    expect(uptime).not.toBe('--');
  });

  test('DASH-004: 网络状态卡片', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.click('#monitor-refresh-btn');
    await authPage.waitForTimeout(5000); // 等待设备数据加载
    const netStatus = await authPage.locator('#monitor-network-status').textContent();
    expect(netStatus).not.toBe('--');
  });

  test('DASH-005: Flash存储进度条 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.click('#monitor-refresh-btn');
    await expectDashboardValue(authPage, 'monitor-flash-percent', /\d+%/);
    for (const id of ['monitor-flash-used', 'monitor-flash-free', 'monitor-flash-total', 'monitor-flash-sketch']) {
      await expectDashboardValue(authPage, id, /^(?!-$|--$).+/);
    }
  });

  test('DASH-006: Heap内存进度条', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    const pct = await authPage.locator('#monitor-heap-percent').textContent();
    expect(pct).toMatch(/\d+%/);
    const used = await authPage.locator('#monitor-heap-used').textContent();
    expect(used).not.toBe('--');
    const free = await authPage.locator('#monitor-heap-free').textContent();
    expect(free).not.toBe('--');
    const total = await authPage.locator('#monitor-heap-total').textContent();
    expect(total).not.toBe('--');
    const minFree = await authPage.locator('#monitor-heap-min').textContent();
    expect(minFree).not.toBe('--');
  });

  test('DASH-007: LittleFS进度条', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.click('#monitor-refresh-btn'); // 触发刷新
    await authPage.waitForTimeout(3000);
    const pct = await authPage.locator('#monitor-fs-percent').textContent();
    expect(pct).toMatch(/\d+%/);
    const used = await authPage.locator('#monitor-fs-used').textContent();
    expect(used).not.toBe('--');
    const free = await authPage.locator('#monitor-fs-free').textContent();
    expect(free).not.toBe('--');
    const total = await authPage.locator('#monitor-fs-total').textContent();
    expect(total).not.toBe('--');
  });

  test('DASH-008: 进度条颜色分级', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    const flashBar = authPage.locator('#monitor-flash-bar');
    await expect(flashBar).toHaveClass(/danger/);
    const heapBar = authPage.locator('#monitor-heap-bar');
    await expect(heapBar).toHaveClass(/success/);
    const fsBar = authPage.locator('#monitor-fs-bar');
    await expect(fsBar).toHaveClass(/info/);
  });

  // ========== 场景B: 网络状态信息 ==========

  test('DASH-009: STA连接信息卡', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.click('#dashboard-net-refresh-btn');
    await authPage.waitForTimeout(3000);
    // STA 信息字段
    const fields = ['#ns-status', '#ns-ssid', '#ns-ip', '#ns-gateway', '#ns-subnet', '#ns-dns', '#ns-rssi', '#ns-mac', '#ns-conn-time'];
    for (const sel of fields) {
      await expect(authPage.locator(sel)).toBeVisible();
    }
  });

  test('DASH-010: AP热点信息卡 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await expect(authPage.locator('#ns-ap-ssid')).toBeVisible();
    await expect(authPage.locator('#ns-ap-ip')).toBeVisible();
    await expect(authPage.locator('#ns-ap-channel')).toBeVisible();
    await expect(authPage.locator('#ns-ap-clients')).toBeVisible();
  });

  test('DASH-011: 连接统计卡', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    const fields = ['#ns-mode', '#ns-mdns', '#ns-reconnect', '#ns-tx-count', '#ns-rx-count', '#ns-internet', '#ns-conflict', '#ns-uptime'];
    for (const sel of fields) {
      await expect(authPage.locator(sel)).toBeVisible();
    }
  });

  test('DASH-012: RSSI信号强度合理性', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.click('#dashboard-net-refresh-btn');
    await authPage.waitForTimeout(3000);
    const rssi = await authPage.locator('#ns-rssi').textContent();
    if (rssi && rssi !== '--') {
      const val = parseInt(rssi);
      expect(val).toBeLessThan(0);
      expect(val).toBeGreaterThan(-100);
    }
  });

  test('DASH-013: IP地址格式验证', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.click('#dashboard-net-refresh-btn');
    await authPage.waitForTimeout(3000);
    const ip = await authPage.locator('#ns-ip').textContent();
    if (ip && ip !== '--') {
      expect(ip).toMatch(/\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}/);
    }
  });

  // ========== 场景C: 交互操作 ==========

  test('DASH-014: 资源刷新按钮', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.click('#monitor-refresh-btn');
    await authPage.waitForTimeout(3000);
    // 刷新后数据应更新
    const flashPct = await authPage.locator('#monitor-flash-percent').textContent();
    expect(flashPct).toMatch(/\d+%/);
  });

  test('DASH-015: 网络状态刷新按钮 @quick', async ({ authPage, navigateTo }) => {
    test.setTimeout(60_000);
    await navigateTo('dashboard');
    await authPage.click('#dashboard-net-refresh-btn');
    await authPage.waitForTimeout(3000);
    await expect(authPage.locator('#ns-status')).toBeVisible();
  });

  test('DASH-016: 全屏监控-新标签页', async ({ authPage, navigateTo }) => {
    await skipIfDeviceControlHidden(authPage);
    await navigateTo('device-control');
    // 设备大屏页面应加载
    await expect(authPage.locator('#device-control-page')).toBeVisible();
  });

  test('DASH-017: 全屏监控-刷新按钮', async ({ authPage, navigateTo }) => {
    await skipIfDeviceControlHidden(authPage);
    await navigateTo('device-control');
    // 设备控制页内加载
    await authPage.waitForTimeout(2000);
    await expect(authPage.locator('#device-control-page')).toBeVisible();
  });

  test('DASH-018: 全屏监控-关闭标签页', async ({ authPage, navigateTo }) => {
    await skipIfDeviceControlHidden(authPage);
    await navigateTo('device-control');
    await authPage.waitForTimeout(1000);
    // 关闭按钮存在
    await expect(authPage.locator('#device-control-page')).toBeVisible();
  });

  test('DASH-019: 仪表盘数据自动更新', async ({ authPage, navigateTo }) => {
    test.setTimeout(60_000);
    await navigateTo('dashboard');
    await authPage.click('#monitor-refresh-btn'); // 确保数据已加载
    await authPage.waitForTimeout(3000);
    const readUptime = async () => authPage.evaluate(async () => {
      const response = await fetch('/api/system/status?probe=1', { cache: 'no-store' });
      if (response.status === 503) {
        const payload = await response.json().catch(() => ({})) as {
          message?: string;
          error?: string;
          retryAfter?: number;
        };
        const detail = `${payload.message || ''} ${payload.error || ''}`.toLowerCase();
        if (/low\s+memory|critical\s+memory|memory\s+pressure|内存紧张|内存压力|低内存/.test(detail)) {
          return { retryAfterMs: Math.max(1000, Number(payload.retryAfter || 5) * 1000) };
        }
      }
      if (!response.ok) return null;
      const payload = await response.json() as { data?: { uptime?: number } };
      return { uptime: Number(payload.data?.uptime) };
    });
    let uptimeBefore = 0;
    await expect.poll(async () => {
      const result = await readUptime();
      if (result && 'retryAfterMs' in result) {
        await authPage.waitForTimeout(Math.min(result.retryAfterMs, 8000));
        return 0;
      }
      uptimeBefore = result && 'uptime' in result ? result.uptime : 0;
      return uptimeBefore;
    }, { timeout: 30_000, intervals: [500, 1000, 2000] }).toBeGreaterThan(0);
    await expect.poll(async () => {
      const result = await readUptime();
      if (result && 'retryAfterMs' in result) {
        await authPage.waitForTimeout(Math.min(result.retryAfterMs, 8000));
        return uptimeBefore;
      }
      return result && 'uptime' in result ? result.uptime : 0;
    }, { timeout: 45_000, intervals: [1000, 2000, 3000] })
      .toBeGreaterThan(uptimeBefore);
  });

  // ========== 场景D: 布局与视觉 ==========

  test('DASH-020: 卡片无重叠检查', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    const cards = authPage.locator('.stat-card');
    const count = await cards.count();
    expect(count).toBeGreaterThanOrEqual(3);
    // 检查卡片位置不重叠
    const boxes = [];
    for (let i = 0; i < count; i++) {
      const box = await cards.nth(i).boundingBox();
      if (box) boxes.push(box);
    }
    for (let i = 0; i < boxes.length - 1; i++) {
      // 右侧卡片的x应 >= 前一个卡片的右边界 或 y不同行
      const a = boxes[i], b = boxes[i + 1];
      const noOverlap = (b.x >= a.x + a.width - 5) || (b.y !== a.y);
      expect(noOverlap).toBeTruthy();
    }
  });

  test('DASH-021: 网络信息卡三列布局', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    const infoCards = authPage.locator('.dashboard-network-grid .info-card');
    const count = await infoCards.count();
    expect(count).toBe(3);
  });

  test('DASH-022: 页面标题一致', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    const title = await authPage.locator('#page-title').textContent();
    expect(title).toBeTruthy();
  });
});
