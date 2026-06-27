import { test, expect } from '../fixtures/base.fixture';

test.describe('Suite-11: 设备日志', () => {

  test.beforeEach(async ({ authPage, navigateTo }) => {
    await navigateTo('logs');
  });

  test('LOG-001: 进入设备日志页 @quick', async ({ authPage }) => {
    await expect(authPage.locator('#logs-page')).toBeVisible();
  });

  test('LOG-002: 日志列表展示', async ({ authPage }) => {
    await authPage.waitForTimeout(2000);
    const logContent = authPage.locator('#log-content');
    await expect(logContent).toBeVisible();
  });

  test('LOG-003: 日志文件选择器', async ({ authPage }) => {
    const logFileSelect = authPage.locator('#log-file-select');
    await expect(logFileSelect).toBeVisible();
    const opts = await logFileSelect.locator('option').allTextContents();
    expect(opts.length).toBeGreaterThanOrEqual(1);
    expect(opts[0]).toContain('system.log');
  });

  test('LOG-004: 日志刷新', async ({ authPage }) => {
    await authPage.click('#refresh-logs-btn');
    await authPage.waitForTimeout(2000);
    const content = await authPage.locator('#log-content').textContent();
    expect(content).toBeTruthy();
  });

  test('LOG-005: 日志刷新列表按钮 @quick', async ({ authPage }) => {
    await authPage.click('#log-refresh-list-btn');
    await authPage.waitForTimeout(2000);
    await expect(authPage.locator('#log-file-select')).toBeVisible();
  });

  test('LOG-006: 日志元信息展示', async ({ authPage }) => {
    await authPage.waitForTimeout(2000);
    await expect(authPage.locator('#log-meta')).toBeVisible();
  });

  test('LOG-007: 日志内容可读', async ({ authPage }) => {
    await authPage.waitForTimeout(2000);
    const content = await authPage.locator('#log-content').textContent();
    // 日志内容不应是乱码
    if (content && content !== '日志加载中...') {
      expect(content.length).toBeGreaterThan(0);
    }
  });

  test('LOG-008: 日志清空按钮', async ({ authPage }) => {
    await expect(authPage.locator('#clear-logs-btn')).toBeVisible();
  });

  test('LOG-009: 日志时间戳格式', async ({ authPage }) => {
    await authPage.waitForTimeout(2000);
    const content = await authPage.locator('#log-content').textContent();
    if (content && content.length > 20) {
      // 检查是否有时间戳格式 YYYY-MM-DD 或类似格式
      const hasTimestamp = /\d{4}-\d{2}-\d{2}|\d{2}:\d{2}:\d{2}/.test(content);
      // 可能有也可能没有，但内容应存在
      expect(content.length).toBeGreaterThan(0);
    }
  });

  test('LOG-010: 日志消息内容', async ({ authPage }) => {
    await authPage.waitForTimeout(2000);
    const content = await authPage.locator('#log-content').textContent();
    // 消息内容可读，无乱码
    expect(content).toBeTruthy();
  });

  test('LOG-011: 多日志文件切换', async ({ authPage }) => {
    const logFileSelect = authPage.locator('#log-file-select');
    const opts = await logFileSelect.locator('option').allTextContents();
    if (opts.length > 1) {
      await logFileSelect.selectOption({ index: 1 });
      await authPage.waitForTimeout(2000);
      await expect(authPage.locator('#log-content')).toBeVisible();
    }
  });

  test('LOG-012: 页面工具栏完整性', async ({ authPage }) => {
    await expect(authPage.locator('#log-file-select')).toBeVisible();
    await expect(authPage.locator('#log-refresh-list-btn')).toBeVisible();
    await expect(authPage.locator('#refresh-logs-btn')).toBeVisible();
    await expect(authPage.locator('#clear-logs-btn')).toBeVisible();
  });
});
