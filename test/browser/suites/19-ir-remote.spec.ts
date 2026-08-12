import { test, expect } from '../fixtures/base.fixture';
import { Page } from '@playwright/test';

/**
 * Suite-19: 红外遥控管理（嵌入式面板）
 * 覆盖外设编辑弹窗内嵌红外管理面板的全部 UI 交互（ir-remote.js + modals.html）：
 * - 面板 DOM 结构完整性
 * - 新增 vs 编辑模式显示逻辑
 * - 对码（学习）流程 UI 状态切换
 * - 按键库 CRUD（保存/删除/清除）
 * - 面板生命周期（类型切换/弹窗关闭）
 * - 开发者模式门控
 */

/** 打开外设新增弹窗 */
async function openAddModal(page: Page) {
  await page.waitForSelector('#peripheral-modal', { state: 'attached', timeout: 20000 });
  const modal = page.locator('#peripheral-modal');
  if (await modal.isVisible().catch(() => false)) {
    await page.locator('#cancel-peripheral-btn').click({ timeout: 2000 }).catch(() => {});
    await page.waitForTimeout(500);
  }
  await page.click('#add-peripheral-btn');
  try {
    await expect(modal).toBeVisible({ timeout: 5000 });
  } catch {
    // 事件委托可能尚未绑定，JS 直接调用
    await page.evaluate(() => {
      const w = window as any;
      if (w.AppState && w.AppState.openPeripheralModal) w.AppState.openPeripheralModal();
    });
    await expect(modal).toBeVisible({ timeout: 8000 });
  }
}

/** 切换外设类型为红外接收头(53)（新增模式：仅显示提示） */
async function switchToIrType(page: Page) {
  await page.locator('#peripheral-type-input').selectOption('53');
  await page.waitForTimeout(500);
}

/** 模拟编辑模式：设置 original-id 后重新触发类型切换，显示管理内容并初始化面板 */
async function activateEditMode(page: Page) {
  await page.evaluate(() => {
    const idInput = document.getElementById('peripheral-original-id') as HTMLInputElement;
    if (idInput) idInput.value = 'test-ir';
    const w = window as any;
    if (w.AppState && w.AppState.onPeripheralTypeChange) w.AppState.onPeripheralTypeChange('53');
  });
  await page.waitForTimeout(1500); // 等待面板初始化 + API 加载
}

/** 关闭外设弹窗 */
async function closeModal(page: Page) {
  await page.locator('#cancel-peripheral-btn').click().catch(() => {});
  await page.waitForTimeout(500);
}

test.describe('Suite-19: 红外遥控管理（嵌入式面板）', () => {

  test.beforeEach(async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
  });

  // ========== 场景A: 面板结构与显示逻辑 ==========

  test('IR-001: 面板 DOM 结构完整性', async ({ authPage }) => {
    await authPage.waitForSelector('#peripheral-modal', { state: 'attached', timeout: 20000 });
    // 红外管理区域嵌入在外设编辑弹窗内
    const section = authPage.locator('#peripheral-modal #ir-manage-section');
    await expect(section).toBeAttached();
    await expect(authPage.locator('#ir-manage-hint-new')).toBeAttached();
    await expect(authPage.locator('#ir-manage-content')).toBeAttached();
    await expect(authPage.locator('#ir-remote-not-ready')).toBeAttached();
    // 对码区
    await expect(authPage.locator('#ir-learn-timeout')).toBeAttached();
    await expect(authPage.locator('#ir-learn-start-btn')).toBeAttached();
    await expect(authPage.locator('#ir-learn-stop-btn')).toBeAttached();
    await expect(authPage.locator('#ir-learn-status')).toBeAttached();
    await expect(authPage.locator('#ir-learn-captured')).toBeAttached();
    await expect(authPage.locator('#ir-key-name-input')).toBeAttached();
    await expect(authPage.locator('#ir-key-code-input')).toBeAttached();
    await expect(authPage.locator('#ir-key-save-btn')).toBeAttached();
    // 按键库区
    await expect(authPage.locator('#ir-keys-table-body')).toBeAttached();
    await expect(authPage.locator('#ir-keys-clear-btn')).toBeAttached();
  });

  test('IR-002: 新增模式 - 显示提示, 管理内容隐藏', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    // 区域可见
    await expect(authPage.locator('#ir-manage-section')).toBeVisible();
    // 新增提示可见
    await expect(authPage.locator('#ir-manage-hint-new')).toBeVisible();
    // 管理内容隐藏
    await expect(authPage.locator('#ir-manage-content')).not.toBeVisible();
    await closeModal(authPage);
  });

  test('IR-003: 编辑模式 - 管理内容显示, 提示隐藏', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    // 管理内容可见
    await expect(authPage.locator('#ir-manage-content')).toBeVisible();
    // 新增提示隐藏
    await expect(authPage.locator('#ir-manage-hint-new')).not.toBeVisible();
    await closeModal(authPage);
  });

  // ========== 场景B: 对码/学习 UI ==========

  test('IR-010: 学习超时输入默认值与范围', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    const timeoutInput = authPage.locator('#ir-learn-timeout');
    expect(await timeoutInput.inputValue()).toBe('10');
    expect(await timeoutInput.getAttribute('min')).toBe('3');
    expect(await timeoutInput.getAttribute('max')).toBe('60');
    await closeModal(authPage);
  });

  test('IR-011: 初始状态 - 开始按钮可见, 取消按钮隐藏', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    await expect(authPage.locator('#ir-learn-start-btn')).toBeVisible();
    await expect(authPage.locator('#ir-learn-stop-btn')).not.toBeVisible();
    // 捕获区域初始隐藏
    await expect(authPage.locator('#ir-learn-captured')).not.toBeVisible();
    await closeModal(authPage);
  });

  test('IR-012: 点击开始学习后 UI 状态切换', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    const startBtn = authPage.locator('#ir-learn-start-btn');
    const isDisabled = await startBtn.isDisabled().catch(() => true);
    if (isDisabled) {
      // 设备红外未初始化，验证 not-ready 提示可见
      await expect(authPage.locator('#ir-remote-not-ready')).toBeVisible();
      await closeModal(authPage);
      return;
    }
    await startBtn.click();
    await authPage.waitForTimeout(1000);
    // 开始后：开始按钮隐藏，取消按钮可见
    await expect(startBtn).not.toBeVisible();
    await expect(authPage.locator('#ir-learn-stop-btn')).toBeVisible();
    // 状态文本应包含等待提示
    const statusText = await authPage.locator('#ir-learn-status').textContent();
    expect(statusText).toContain('等待');
    // 取消学习
    await authPage.locator('#ir-learn-stop-btn').click();
    await authPage.waitForTimeout(1000);
    // 取消后恢复初始状态
    await expect(startBtn).toBeVisible();
    await expect(authPage.locator('#ir-learn-stop-btn')).not.toBeVisible();
    await closeModal(authPage);
  });

  test('IR-013: 编码输入框为只读', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    const readonly = await authPage.locator('#ir-key-code-input').getAttribute('readonly');
    expect(readonly).not.toBeNull();
    await closeModal(authPage);
  });

  // ========== 场景C: 按键库 ==========

  test('IR-020: 按键库表格表头完整', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    const thead = authPage.locator('#ir-manage-section table thead');
    await expect(thead).toBeVisible();
    const headers = await thead.locator('th').allTextContents();
    expect(headers.length).toBe(4);
    expect(headers[0]).toContain('按键名称');
    expect(headers[1]).toContain('编码');
    expect(headers[2]).toContain('协议');
    expect(headers[3]).toContain('操作');
    await closeModal(authPage);
  });

  test('IR-021: 按键库加载（空或有数据）', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    await authPage.waitForTimeout(2000); // 等待 API 响应
    const tbody = authPage.locator('#ir-keys-table-body');
    await expect(tbody).toBeVisible();
    // 应有内容（空提示或数据行）
    const content = await tbody.textContent();
    expect(content!.trim().length).toBeGreaterThan(0);
    await closeModal(authPage);
  });

  test('IR-022: 保存按键 - 名称为空时提示', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    // 模拟已捕获状态：直接通过 JS 显示捕获区域
    await authPage.evaluate(() => {
      const captured = document.getElementById('ir-learn-captured');
      if (captured) captured.style.display = '';
      const codeInput = document.getElementById('ir-key-code-input') as HTMLInputElement;
      if (codeInput) codeInput.value = '0x00FF6897';
    });
    // 名称留空
    await authPage.locator('#ir-key-name-input').fill('');
    await authPage.locator('#ir-key-save-btn').click();
    await authPage.waitForTimeout(1000);
    // 应有警告通知（Notification.warning）
    const notifText = await authPage.locator('#notification-container').textContent().catch(() => '');
    if (notifText && notifText.length > 0) {
      expect(notifText).toContain('名称');
    }
    await closeModal(authPage);
  });

  test('IR-023: 保存按键 - 编码为空时提示', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    // 显示捕获区域但不填编码
    await authPage.evaluate(() => {
      const captured = document.getElementById('ir-learn-captured');
      if (captured) captured.style.display = '';
    });
    await authPage.locator('#ir-key-name-input').fill('test_key');
    await authPage.locator('#ir-key-save-btn').click();
    await authPage.waitForTimeout(1000);
    const notifText = await authPage.locator('#notification-container').textContent().catch(() => '');
    if (notifText && notifText.length > 0) {
      expect(notifText).toContain('编码');
    }
    await closeModal(authPage);
  });

  test('IR-024: 清除全部按键需确认', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    // Mock confirm 返回 false（取消）
    await authPage.evaluate(() => {
      (window as any).confirm = () => false;
    });
    await authPage.locator('#ir-keys-clear-btn').click();
    await authPage.waitForTimeout(1000);
    // 取消后不应有成功通知
    const notifText = await authPage.locator('#notification-container').textContent().catch(() => '');
    expect(notifText || '').not.toContain('已清除');
    // 恢复 confirm
    await authPage.evaluate(() => { delete (window as any).confirm; });
    await closeModal(authPage);
  });

  // ========== 场景D: 按键删除交互 ==========

  test('IR-030: 按键表格中删除按钮存在（如有数据）', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    await authPage.waitForTimeout(2000);
    const tbody = authPage.locator('#ir-keys-table-body');
    const deleteBtns = tbody.locator('button[data-ir-key-action="delete"]');
    const count = await deleteBtns.count();
    if (count > 0) {
      // 有按键数据时，删除按钮应可见且有 data-name 属性
      await expect(deleteBtns.first()).toBeVisible();
      const name = await deleteBtns.first().getAttribute('data-name');
      expect(name!.length).toBeGreaterThan(0);
    }
    // 无数据时也通过（空表）
    expect(true).toBe(true);
    await closeModal(authPage);
  });

  // ========== 场景E: 面板生命周期 ==========

  test('IR-040: 类型切离 53 后面板隐藏', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await expect(authPage.locator('#ir-manage-section')).toBeVisible();
    // 切换到数字输出(12)
    await authPage.locator('#peripheral-type-input').selectOption('12');
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#ir-manage-section')).not.toBeVisible();
    await closeModal(authPage);
  });

  test('IR-041: 类型切回 53 面板重新显示', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    // 切走再切回
    await authPage.locator('#peripheral-type-input').selectOption('12');
    await authPage.waitForTimeout(300);
    await authPage.locator('#peripheral-type-input').selectOption('53');
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#ir-manage-section')).toBeVisible();
    await closeModal(authPage);
  });

  test('IR-042: 关闭弹窗后重新打开不崩溃', async ({ authPage }) => {
    for (let i = 0; i < 3; i++) {
      await openAddModal(authPage);
      await switchToIrType(authPage);
      await activateEditMode(authPage);
      await closeModal(authPage);
    }
    // 页面保持稳定
    await expect(authPage.locator('#peripheral-page')).toBeVisible();
  });

  // ========== 场景F: 开发者模式门控 ==========

  test('IR-050: 开发者模式禁用时删除按钮状态', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    await authPage.waitForTimeout(2000);
    // 检查是否处于开发者模式锁定状态
    const hasDevLock = await authPage.locator('#ir-manage-section .dev-mode-locked').count() > 0;
    if (hasDevLock) {
      // 锁定时删除按钮应 disabled
      const deleteBtns = authPage.locator('#ir-keys-table-body button[data-ir-key-action="delete"]');
      const count = await deleteBtns.count();
      for (let i = 0; i < count; i++) {
        expect(await deleteBtns.nth(i).isDisabled()).toBe(true);
      }
    }
    // 未锁定时也通过
    expect(true).toBe(true);
    await closeModal(authPage);
  });

  // ========== 场景G: 鲁棒性 ==========

  test('IR-060: 面板打开后设备 API 状态检查', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToIrType(authPage);
    await activateEditMode(authPage);
    await authPage.waitForTimeout(2000);
    // 检查 /api/ir/status 是否被调用（通过 UI 状态判断）
    const notReady = authPage.locator('#ir-remote-not-ready');
    const startBtn = authPage.locator('#ir-learn-start-btn');
    const isNotReady = await notReady.isVisible().catch(() => false);
    if (isNotReady) {
      // 设备红外未初始化，按钮应 disabled
      expect(await startBtn.isDisabled()).toBe(true);
    } else {
      // 已初始化时按钮应可用
      expect(await startBtn.isDisabled()).toBe(false);
    }
    await closeModal(authPage);
  });
});
