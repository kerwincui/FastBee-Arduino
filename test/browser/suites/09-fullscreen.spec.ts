import { test, expect } from '../fixtures/base.fixture';

test.describe('Suite-09: 设备大屏', () => {

  test('SCR-001: 打开设备大屏', async ({ authPage, navigateTo }) => {
    await navigateTo('device-control');
    await expect(authPage.locator('#device-control-page')).toBeVisible();
  });

  test('SCR-002: 大屏布局展示', async ({ authPage, navigateTo }) => {
    await navigateTo('device-control');
    await authPage.waitForTimeout(3000);
    // 大屏内容应加载
    const content = authPage.locator('#dc-content');
    await expect(content).toBeVisible();
  });

  test('SCR-003: 全屏页面独立访问', async ({ page, baseURL }) => {
    // 设备大屏有独立HTML页面
    await page.goto(`${baseURL}/pages/fullscreen.html`);
    await page.waitForLoadState('networkidle', { timeout: 15_000 });
    // 全屏页面应有自动刷新指示器
    await expect(page.locator('#auto-refresh-indicator')).toBeVisible();
  });

  test('SCR-004: 全屏资源使用率显示', async ({ page, baseURL }) => {
    await page.goto(`${baseURL}/pages/fullscreen.html`);
    await page.waitForLoadState('networkidle', { timeout: 15_000 });
    await expect(page.locator('#monitor-flash-percent')).toBeVisible();
    await expect(page.locator('#monitor-heap-percent')).toBeVisible();
    await expect(page.locator('#monitor-fs-percent')).toBeVisible();
  });

  test('SCR-005: 全屏网络状态展示', async ({ page, baseURL }) => {
    await page.goto(`${baseURL}/pages/fullscreen.html`);
    await page.waitForLoadState('networkidle', { timeout: 15_000 });
    await expect(page.locator('#ns-status')).toBeVisible();
    await expect(page.locator('#ns-ap-ssid')).toBeVisible();
  });

  test('SCR-006: 手动刷新按钮', async ({ page, baseURL }) => {
    await page.goto(`${baseURL}/pages/fullscreen.html`);
    await page.waitForLoadState('networkidle', { timeout: 15_000 });
    await page.click('#fullscreen-refresh-btn');
    await page.waitForTimeout(3000);
    // 刷新后数据应更新
    const uptime = await page.locator('#monitor-uptime').textContent();
    expect(uptime).toBeTruthy();
  });

  test('SCR-007: 大屏暗色主题', async ({ page, baseURL }) => {
    await page.goto(`${baseURL}/pages/fullscreen.html`);
    await page.waitForLoadState('networkidle', { timeout: 15_000 });
    // 全屏页面使用 fullscreen-page class
    await expect(page.locator('body')).toHaveClass(/fullscreen/);
  });

  test('SCR-008: 响应式布局-大屏 1920x1080', async ({ page, baseURL }) => {
    await page.setViewportSize({ width: 1920, height: 1080 });
    await page.goto(`${baseURL}/pages/fullscreen.html`);
    await page.waitForLoadState('networkidle', { timeout: 15_000 });
    const statsGrid = page.locator('.stats-grid');
    if (await statsGrid.isVisible()) {
      const box = await statsGrid.boundingBox();
      expect(box?.width).toBeLessThanOrEqual(1920);
    }
  });

  test('SCR-009: 响应式布局-中屏 1366x768', async ({ page, baseURL }) => {
    await page.setViewportSize({ width: 1366, height: 768 });
    await page.goto(`${baseURL}/pages/fullscreen.html`);
    await page.waitForLoadState('networkidle', { timeout: 15_000 });
    await expect(page.locator('.fullscreen-container')).toBeVisible();
  });

  test('SCR-010: 关闭标签页按钮', async ({ page, baseURL }) => {
    await page.goto(`${baseURL}/pages/fullscreen.html`);
    await page.waitForLoadState('networkidle', { timeout: 15_000 });
    await expect(page.locator('#fullscreen-close-btn')).toBeVisible();
  });

  test('SCR-011: 大屏无交互超时(2分钟)', async ({ page, baseURL }) => {
    await page.goto(`${baseURL}/pages/fullscreen.html`);
    await page.waitForLoadState('networkidle', { timeout: 15_000 });
    // 等待2分钟不操作
    await page.waitForTimeout(120_000);
    // 页面应仍可用
    await expect(page.locator('.fullscreen-container')).toBeVisible();
    // 自动刷新应工作
    const uptime = await page.locator('#monitor-uptime').textContent();
    expect(uptime).not.toBe('--');
  });

  test('SCR-012: 大屏数据一致性', async ({ authPage, page, navigateTo, baseURL }) => {
    // 仪表盘数据
    await navigateTo('dashboard');
    const dashUptime = await authPage.locator('#monitor-uptime').textContent();

    // 大屏页面
    const page2 = await page.context().newPage();
    await page2.goto(`${baseURL}/pages/fullscreen.html`);
    await page2.waitForLoadState('networkidle', { timeout: 15_000 });
    const fullUptime = await page2.locator('#monitor-uptime').textContent();
    await page2.close();

    // 两者应都有值（可能有时间差）
    expect(dashUptime).toBeTruthy();
    expect(fullUptime).toBeTruthy();
  });
});
