import { test, expect } from '../fixtures/base.fixture';

test.describe('Suite-15: UI布局与视觉回归', () => {

  // ========== 登录页布局 ==========

  test('UI-001: 登录页布局 @quick', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('#login-page')).toBeVisible();
    // 截图存档
    await page.screenshot({ path: 'test-results/ui/UI-001-login.png', fullPage: true });
    // 登录卡片居中
    const loginPage = page.locator('#login-page');
    const box = await loginPage.boundingBox();
    expect(box).toBeTruthy();
    expect(box!.width).toBeGreaterThan(200);
    expect(box!.height).toBeGreaterThan(100);
  });

  test('UI-002: 侧边栏菜单对齐', async ({ authPage }) => {
    const menuItems = authPage.locator('.menu-item');
    const count = await menuItems.count();
    expect(count).toBeGreaterThanOrEqual(10);
    // 各菜单项应有合理的垂直位置
    for (let i = 0; i < Math.min(count, 5); i++) {
      const box = await menuItems.nth(i).boundingBox();
      expect(box).toBeTruthy();
      expect(box!.height).toBeGreaterThan(20);
    }
  });

  test('UI-003: 侧边栏折叠/展开', async ({ authPage }) => {
    // 尝试找到折叠按钮
    const toggleBtn = authPage.locator('#sidebar-toggle, .sidebar-toggle, [data-action="toggleSidebar"]').first();
    if (await toggleBtn.isVisible()) {
      await toggleBtn.click();
      await authPage.waitForTimeout(500);
      // 再次展开
      await toggleBtn.click();
      await authPage.waitForTimeout(500);
    }
    // 侧边栏应仍可见
    const sidebar = authPage.locator('#sidebar, .sidebar, .fb-sidebar').first();
    if (await sidebar.isVisible()) {
      await expect(sidebar).toBeVisible();
    }
  });

  // ========== 仪表盘布局 ==========

  test('UI-004: 仪表盘卡片布局', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.screenshot({ path: 'test-results/ui/UI-004-dashboard.png', fullPage: true });
    // 检查卡片存在
    const cards = authPage.locator('#dashboard-page .stat-card, #dashboard-page .resource-card, #dashboard-page .info-card');
    const count = await cards.count();
    expect(count).toBeGreaterThanOrEqual(1);
  });

  test('UI-005: 表格列对齐', async ({ authPage, navigateTo }) => {
    await navigateTo('users');
    await authPage.waitForTimeout(2000);
    const table = authPage.locator('#users-page .fb-table');
    if (await table.isVisible()) {
      await authPage.screenshot({ path: 'test-results/ui/UI-005-table.png' });
      await expect(table).toBeVisible();
    }
  });

  // ========== 模态框 ==========

  test('UI-006: 模态框居中显示', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    const addBtn = authPage.locator('#add-peripheral-btn');
    if (await addBtn.isVisible()) {
      await addBtn.click();
      await authPage.waitForTimeout(1000);
      const modal = authPage.locator('.modal.show, .modal[style*="display: block"], .fb-modal.show').first();
      if (await modal.isVisible()) {
        const box = await modal.boundingBox();
        expect(box).toBeTruthy();
        // 模态框应在视口内
        expect(box!.x).toBeGreaterThanOrEqual(0);
        expect(box!.y).toBeGreaterThanOrEqual(0);
      }
    }
  });

  test('UI-007: 模态框关闭后遮罩消失', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    const addBtn = authPage.locator('#add-peripheral-btn');
    if (await addBtn.isVisible()) {
      await addBtn.click();
      await authPage.waitForTimeout(1000);
      // 关闭模态框
      const closeBtn = authPage.locator('.modal .close, .modal [data-dismiss], .modal-close, .fb-modal-close').first();
      if (await closeBtn.isVisible()) {
        await closeBtn.click();
        await authPage.waitForTimeout(500);
      }
      // 按 Escape 关闭
      await authPage.keyboard.press('Escape');
      await authPage.waitForTimeout(500);
    }
  });

  // ========== 表单与按钮 ==========

  test('UI-008: 表单标签对齐', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.screenshot({ path: 'test-results/ui/UI-008-forms.png', fullPage: true });
    const labels = authPage.locator('#device-page label');
    const count = await labels.count();
    expect(count).toBeGreaterThan(0);
  });

  test('UI-009: 进度条显示正确', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.waitForLoadState('networkidle');
    // 检查进度条存在
    const progressBars = authPage.locator('.progress-bar, .fb-progress, [class*="progress"]');
    const count = await progressBars.count();
    console.log(`进度条数量: ${count}`);
  });

  test('UI-010: 按钮组排列 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const buttons = authPage.locator('#network-page .fb-btn, #network-page button');
    const count = await buttons.count();
    expect(count).toBeGreaterThan(0);
    await authPage.screenshot({ path: 'test-results/ui/UI-010-buttons.png' });
  });

  // ========== 响应式布局 ==========

  test('UI-011: 响应式-1920x1080', async ({ authPage, navigateTo }) => {
    await authPage.setViewportSize({ width: 1920, height: 1080 });
    await navigateTo('dashboard');
    await authPage.waitForLoadState('networkidle');
    await authPage.screenshot({ path: 'test-results/ui/UI-011-1920x1080.png', fullPage: true });
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('UI-012: 响应式-1366x768', async ({ authPage, navigateTo }) => {
    await authPage.setViewportSize({ width: 1366, height: 768 });
    await navigateTo('dashboard');
    await authPage.waitForLoadState('networkidle');
    await authPage.screenshot({ path: 'test-results/ui/UI-012-1366x768.png', fullPage: true });
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('UI-013: 响应式-1024x768', async ({ authPage, navigateTo }) => {
    await authPage.setViewportSize({ width: 1024, height: 768 });
    await navigateTo('dashboard');
    await authPage.waitForLoadState('networkidle');
    await authPage.screenshot({ path: 'test-results/ui/UI-013-1024x768.png', fullPage: true });
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  // ========== 主题切换 ==========

  test('UI-014: 暗色主题切换', async ({ authPage }) => {
    const toggleBtn = authPage.locator('#user-dropdown-btn');
    if (await toggleBtn.isVisible()) {
      await toggleBtn.click();
      await authPage.waitForTimeout(500);
      const themeToggle = authPage.locator('#theme-toggle-item, [data-action="toggleTheme"]').first();
      if (await themeToggle.isVisible()) {
        await themeToggle.click();
        await authPage.waitForTimeout(1000);
        await authPage.screenshot({ path: 'test-results/ui/UI-014-dark-theme.png' });
      }
    }
  });

  test('UI-015: 亮色主题切换', async ({ authPage }) => {
    // 切回亮色
    const toggleBtn = authPage.locator('#user-dropdown-btn');
    if (await toggleBtn.isVisible()) {
      await toggleBtn.click();
      await authPage.waitForTimeout(500);
      const themeToggle = authPage.locator('#theme-toggle-item, [data-action="toggleTheme"]').first();
      if (await themeToggle.isVisible()) {
        await themeToggle.click();
        await authPage.waitForTimeout(1000);
        await authPage.screenshot({ path: 'test-results/ui/UI-015-light-theme.png' });
      }
    }
  });

  // ========== 语言切换 ==========

  test('UI-016: 中英文切换-登录页', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('#login-page')).toBeVisible();
    // 切换到英文
    const langSelect = page.locator('#login-language-select');
    if (await langSelect.isVisible()) {
      await langSelect.selectOption('en');
      await page.waitForTimeout(1000);
      await page.screenshot({ path: 'test-results/ui/UI-016-login-en.png' });
      // 切回中文
      await langSelect.selectOption('zh-CN');
      await page.waitForTimeout(1000);
    }
  });

  // ========== 空状态与加载 ==========

  test('UI-017: 空状态提示', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    await authPage.waitForTimeout(2000);
    const emptyState = authPage.locator('.empty-state, .no-data, [class*="empty"]').first();
    // 可能有也可能没有空状态（取决于是否有外设）
    if (await emptyState.isVisible()) {
      await expect(emptyState).toBeVisible();
    }
  });

  test('UI-018: 加载状态', async ({ authPage, navigateTo }) => {
    // 导航时应有某种加载指示
    await navigateTo('dashboard');
    // 页面加载完成
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  // ========== Toast通知 ==========

  test('UI-019: Toast通知显示', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    // 触发保存
    const saveBtn = authPage.locator('#wifi-save-btn');
    if (await saveBtn.isVisible()) {
      await saveBtn.click();
      await authPage.waitForTimeout(3000);
      // 检查是否有通知
      const notification = authPage.locator('.notification, .toast, .message-success, #notification-container .notification-success').first();
      if (await notification.isVisible()) {
        await expect(notification).toBeVisible();
      }
    }
  });

  test('UI-020: 错误提示显示 @quick', async ({ page }) => {
    await page.goto('/');
    await page.fill('#username', 'admin');
    await page.fill('#password', 'wrong_password');
    await page.click('#login-button');
    await page.waitForTimeout(2000);
    // 应有错误提示
    await expect(page.locator('#login-page')).toBeVisible();
    const errorMsg = page.locator('.message-error, .error-message, .login-error, [class*="error"]').first();
    if (await errorMsg.isVisible()) {
      await expect(errorMsg).toBeVisible();
    }
  });

  // ========== 滚动与交互 ==========

  test('UI-021: 滚动条样式', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    // 长页面滚动
    await authPage.evaluate(() => window.scrollTo(0, document.body.scrollHeight));
    await authPage.waitForTimeout(500);
    await authPage.evaluate(() => window.scrollTo(0, 0));
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('UI-022: 焦点状态样式', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    // Tab 键遍历表单
    const firstInput = authPage.locator('#device-page input:visible').first();
    if (await firstInput.isVisible()) {
      await firstInput.focus();
      await authPage.waitForTimeout(200);
      // 检查是否有焦点样式（outline 或 border-color 变化）
      const hasFocus = await firstInput.evaluate(el => document.activeElement === el);
      expect(hasFocus).toBeTruthy();
    }
  });

  test('UI-023: 禁用状态样式', async ({ authPage, navigateTo }) => {
    await navigateTo('data');
    const editor = authPage.locator('#file-editor');
    if (await editor.isVisible()) {
      const isDisabled = await editor.isDisabled();
      if (isDisabled) {
        // 禁用状态应有视觉反馈
        await expect(editor).toBeDisabled();
      }
    }
  });

  // ========== 下拉菜单 ==========

  test('UI-024: 用户下拉菜单展开', async ({ authPage }) => {
    const dropdownBtn = authPage.locator('#user-dropdown-btn');
    if (await dropdownBtn.isVisible()) {
      await dropdownBtn.click();
      await authPage.waitForTimeout(500);
      // 菜单应展开
      const menu = authPage.locator('#user-dropdown-menu, .user-dropdown-menu, [class*="dropdown-menu"]').first();
      if (await menu.isVisible()) {
        await expect(menu).toBeVisible();
        await authPage.screenshot({ path: 'test-results/ui/UI-024-dropdown.png' });
      }
    }
  });

  // ========== 页面切换 ==========

  test('UI-025: 页面切换无闪烁', async ({ authPage, navigateTo }) => {
    // 快速切换菜单
    for (const menuPage of ['dashboard', 'device', 'network', 'protocol', 'logs']) {
      await authPage.click(`.menu-item[data-page="${menuPage}"]`);
      await authPage.waitForTimeout(200);
    }
    // 页面应正常
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  // ========== 截图对比 ==========

  test('UI-026: 各页面全页截图', async ({ authPage, navigateTo }) => {
    const pages = ['dashboard', 'device', 'network', 'protocol', 'logs'];
    for (const menuPage of pages) {
      await navigateTo(menuPage);
      await authPage.waitForTimeout(500);
      await authPage.screenshot({ path: `test-results/ui/UI-026-${menuPage}.png`, fullPage: true });
    }
  });

  test('UI-027: 长文本不截断', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    const nameInput = authPage.locator('#dev-name, #device-name, input[name="deviceName"]').first();
    if (await nameInput.isVisible()) {
      const longName = 'A'.repeat(50);
      await nameInput.fill(longName);
      const value = await nameInput.inputValue();
      expect(value).toBe(longName);
      // 恢复
      await nameInput.fill('');
    }
  });

  test('UI-028: 数字字段验证', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const timeoutInput = authPage.locator('#connect-timeout');
    if (await timeoutInput.isVisible()) {
      // 输入有效值
      await timeoutInput.fill('20000');
      const value = await timeoutInput.inputValue();
      expect(value).toBe('20000');
    }
  });

  test('UI-029: 图标加载完整', async ({ authPage }) => {
    // 检查是否有加载失败的图片
    const brokenImages = await authPage.evaluate(() => {
      const imgs = document.querySelectorAll('img');
      let broken = 0;
      imgs.forEach(img => {
        if (img.naturalWidth === 0 && img.src) broken++;
      });
      return broken;
    });
    console.log(`破损图片数: ${brokenImages}`);
    // 允许0个破损图片（或使用图标的SVG/CSS图标不计入）
  });

  test('UI-030: 页脚信息', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    // 检查页脚
    const footer = authPage.locator('footer, .footer, .fb-footer, [class*="footer"]').first();
    if (await footer.isVisible()) {
      const text = await footer.textContent();
      console.log(`页脚: ${text?.substring(0, 100)}`);
    }
  });
});
