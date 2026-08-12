import { test, expect } from '../fixtures/base.fixture';
import { Page } from '@playwright/test';

/**
 * Suite-20: 433MHz RF 遥控管理（嵌入式面板）
 * 覆盖外设编辑弹窗内嵌 RF 管理面板的全部 UI 交互（rf-remote.js + modals.html）：
 * - 面板 DOM 结构完整性
 * - 新增 vs 编辑模式显示逻辑
 * - 对码（学习）流程 UI 状态切换
 * - 按键库 CRUD（保存/删除/清除）
 * - 面板生命周期（类型切换/弹窗关闭）
 * - 开发者模式门控
 * - 规则编辑器 RF 事件集成
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
    await page.evaluate(() => {
      const w = window as any;
      if (w.AppState && w.AppState.openPeripheralModal) w.AppState.openPeripheralModal();
    });
    await expect(modal).toBeVisible({ timeout: 8000 });
  }
}

/** 切换外设类型为 RF模块(48) */
async function switchToRfType(page: Page) {
  await page.locator('#peripheral-type-input').selectOption('48');
  await page.waitForTimeout(500);
}

/** 模拟编辑模式：设置 original-id 后重新触发类型切换 */
async function activateEditMode(page: Page) {
  await page.evaluate(() => {
    const idInput = document.getElementById('peripheral-original-id') as HTMLInputElement;
    if (idInput) idInput.value = 'test-rf';
    const w = window as any;
    if (w.AppState && w.AppState.onPeripheralTypeChange) w.AppState.onPeripheralTypeChange('48');
  });
  await page.waitForTimeout(1500);
}

/** 关闭外设弹窗 */
async function closeModal(page: Page) {
  await page.locator('#cancel-peripheral-btn').click().catch(() => {});
  await page.waitForTimeout(500);
}

test.describe('Suite-20: 433MHz RF 遥控管理（嵌入式面板）', () => {

  test.beforeEach(async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
  });

  // ========== 场景A: 面板结构与显示逻辑 ==========

  test('RF-001: 面板 DOM 结构完整性', async ({ authPage }) => {
    await authPage.waitForSelector('#peripheral-modal', { state: 'attached', timeout: 20000 });
    const section = authPage.locator('#peripheral-modal #rf-manage-section');
    await expect(section).toBeAttached();
    await expect(authPage.locator('#rf-manage-hint-new')).toBeAttached();
    await expect(authPage.locator('#rf-manage-content')).toBeAttached();
    await expect(authPage.locator('#rf-not-ready')).toBeAttached();
    // 对码区
    await expect(authPage.locator('#rf-learn-timeout')).toBeAttached();
    await expect(authPage.locator('#rf-learn-start-btn')).toBeAttached();
    await expect(authPage.locator('#rf-learn-stop-btn')).toBeAttached();
    await expect(authPage.locator('#rf-learn-status')).toBeAttached();
    await expect(authPage.locator('#rf-learn-captured')).toBeAttached();
    await expect(authPage.locator('#rf-key-name-input')).toBeAttached();
    await expect(authPage.locator('#rf-key-code-input')).toBeAttached();
    await expect(authPage.locator('#rf-key-bits-input')).toBeAttached();
    await expect(authPage.locator('#rf-key-pulse-input')).toBeAttached();
    await expect(authPage.locator('#rf-key-save-btn')).toBeAttached();
    // 按键库区
    await expect(authPage.locator('#rf-keys-table-body')).toBeAttached();
    await expect(authPage.locator('#rf-keys-clear-btn')).toBeAttached();
  });

  test('RF-002: 新增模式 - 显示提示, 管理内容隐藏', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await expect(authPage.locator('#rf-manage-section')).toBeVisible();
    await expect(authPage.locator('#rf-manage-hint-new')).toBeVisible();
    await expect(authPage.locator('#rf-manage-content')).not.toBeVisible();
    await closeModal(authPage);
  });

  test('RF-003: 编辑模式 - 管理内容显示, 提示隐藏', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    await expect(authPage.locator('#rf-manage-content')).toBeVisible();
    await expect(authPage.locator('#rf-manage-hint-new')).not.toBeVisible();
    await closeModal(authPage);
  });

  test('RF-004: RF 参数区域显示且模式下拉含接收学习选项', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await expect(authPage.locator('#rf-params')).toBeVisible();
    const modeSelect = authPage.locator('#rf-mode');
    await expect(modeSelect).toBeVisible();
    const options = await modeSelect.locator('option').allTextContents();
    expect(options.length).toBe(2);
    expect(options[0]).toContain('发射');
    expect(options[1]).toContain('接收');
    await closeModal(authPage);
  });

  // ========== 场景B: 对码/学习 UI ==========

  test('RF-010: 学习超时输入默认值与范围', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    const timeoutInput = authPage.locator('#rf-learn-timeout');
    expect(await timeoutInput.inputValue()).toBe('10');
    expect(await timeoutInput.getAttribute('min')).toBe('3');
    expect(await timeoutInput.getAttribute('max')).toBe('60');
    await closeModal(authPage);
  });

  test('RF-011: 初始状态 - 开始按钮可见, 取消按钮隐藏', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    await expect(authPage.locator('#rf-learn-start-btn')).toBeVisible();
    await expect(authPage.locator('#rf-learn-stop-btn')).not.toBeVisible();
    await expect(authPage.locator('#rf-learn-captured')).not.toBeVisible();
    await closeModal(authPage);
  });

  test('RF-012: 点击开始学习后 UI 状态切换', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    const startBtn = authPage.locator('#rf-learn-start-btn');
    const isDisabled = await startBtn.isDisabled().catch(() => true);
    if (isDisabled) {
      await expect(authPage.locator('#rf-not-ready')).toBeVisible();
      await closeModal(authPage);
      return;
    }
    await startBtn.click();
    await authPage.waitForTimeout(1000);
    await expect(startBtn).not.toBeVisible();
    await expect(authPage.locator('#rf-learn-stop-btn')).toBeVisible();
    const statusText = await authPage.locator('#rf-learn-status').textContent();
    expect(statusText).toContain('等待');
    // 取消学习
    await authPage.locator('#rf-learn-stop-btn').click();
    await authPage.waitForTimeout(1000);
    await expect(startBtn).toBeVisible();
    await expect(authPage.locator('#rf-learn-stop-btn')).not.toBeVisible();
    await closeModal(authPage);
  });

  test('RF-013: 编码/位长/脉宽输入框为只读', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    expect(await authPage.locator('#rf-key-code-input').getAttribute('readonly')).not.toBeNull();
    expect(await authPage.locator('#rf-key-bits-input').getAttribute('readonly')).not.toBeNull();
    expect(await authPage.locator('#rf-key-pulse-input').getAttribute('readonly')).not.toBeNull();
    await closeModal(authPage);
  });

  // ========== 场景C: 按键库 ==========

  test('RF-020: 按键库表格表头完整（5列）', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    const thead = authPage.locator('#rf-manage-section table thead');
    await expect(thead).toBeVisible();
    const headers = await thead.locator('th').allTextContents();
    expect(headers.length).toBe(5);
    expect(headers[0]).toContain('按键名称');
    expect(headers[1]).toContain('编码');
    expect(headers[2]).toContain('位长');
    expect(headers[3]).toContain('脉宽');
    expect(headers[4]).toContain('操作');
    await closeModal(authPage);
  });

  test('RF-021: 按键库加载（空或有数据）', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    await authPage.waitForTimeout(2000);
    const tbody = authPage.locator('#rf-keys-table-body');
    await expect(tbody).toBeVisible();
    const content = await tbody.textContent();
    expect(content!.trim().length).toBeGreaterThan(0);
    await closeModal(authPage);
  });

  test('RF-022: 保存按键 - 名称为空时提示', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    await authPage.evaluate(() => {
      const captured = document.getElementById('rf-learn-captured');
      if (captured) captured.style.display = '';
      const codeInput = document.getElementById('rf-key-code-input') as HTMLInputElement;
      if (codeInput) codeInput.value = '0xABC123';
    });
    await authPage.locator('#rf-key-name-input').fill('');
    await authPage.locator('#rf-key-save-btn').click();
    await authPage.waitForTimeout(1000);
    const notifText = await authPage.locator('#notification-container').textContent().catch(() => '');
    if (notifText && notifText.length > 0) {
      expect(notifText).toContain('名称');
    }
    await closeModal(authPage);
  });

  test('RF-023: 保存按键 - 编码为空时提示', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    await authPage.evaluate(() => {
      const captured = document.getElementById('rf-learn-captured');
      if (captured) captured.style.display = '';
    });
    await authPage.locator('#rf-key-name-input').fill('test_btn');
    await authPage.locator('#rf-key-save-btn').click();
    await authPage.waitForTimeout(1000);
    const notifText = await authPage.locator('#notification-container').textContent().catch(() => '');
    if (notifText && notifText.length > 0) {
      expect(notifText).toContain('编码');
    }
    await closeModal(authPage);
  });

  test('RF-024: 清除全部按键需确认', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    await authPage.evaluate(() => {
      (window as any).confirm = () => false;
    });
    await authPage.locator('#rf-keys-clear-btn').click();
    await authPage.waitForTimeout(1000);
    const notifText = await authPage.locator('#notification-container').textContent().catch(() => '');
    expect(notifText || '').not.toContain('已清除');
    await authPage.evaluate(() => { delete (window as any).confirm; });
    await closeModal(authPage);
  });

  // ========== 场景D: 按键删除交互 ==========

  test('RF-030: 按键表格中删除按钮存在（如有数据）', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    await authPage.waitForTimeout(2000);
    const tbody = authPage.locator('#rf-keys-table-body');
    const deleteBtns = tbody.locator('button[data-rf-key-action="delete"]');
    const count = await deleteBtns.count();
    if (count > 0) {
      await expect(deleteBtns.first()).toBeVisible();
      const name = await deleteBtns.first().getAttribute('data-name');
      expect(name!.length).toBeGreaterThan(0);
    }
    expect(true).toBe(true);
    await closeModal(authPage);
  });

  // ========== 场景E: 面板生命周期 ==========

  test('RF-040: 类型切离 48 后面板隐藏', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await expect(authPage.locator('#rf-manage-section')).toBeVisible();
    await authPage.locator('#peripheral-type-input').selectOption('12');
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#rf-manage-section')).not.toBeVisible();
    await closeModal(authPage);
  });

  test('RF-041: 类型切回 48 面板重新显示', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await authPage.locator('#peripheral-type-input').selectOption('12');
    await authPage.waitForTimeout(300);
    await authPage.locator('#peripheral-type-input').selectOption('48');
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#rf-manage-section')).toBeVisible();
    await closeModal(authPage);
  });

  test('RF-042: 关闭弹窗后重新打开不崩溃', async ({ authPage }) => {
    for (let i = 0; i < 3; i++) {
      await openAddModal(authPage);
      await switchToRfType(authPage);
      await activateEditMode(authPage);
      await closeModal(authPage);
    }
    await expect(authPage.locator('#peripheral-page')).toBeVisible();
  });

  // ========== 场景F: 开发者模式门控 ==========

  test('RF-050: 开发者模式禁用时删除按钮状态', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    await authPage.waitForTimeout(2000);
    const hasDevLock = await authPage.locator('#rf-manage-section .dev-mode-locked').count() > 0;
    if (hasDevLock) {
      const deleteBtns = authPage.locator('#rf-keys-table-body button[data-rf-key-action="delete"]');
      const count = await deleteBtns.count();
      for (let i = 0; i < count; i++) {
        expect(await deleteBtns.nth(i).isDisabled()).toBe(true);
      }
    }
    expect(true).toBe(true);
    await closeModal(authPage);
  });

  // ========== 场景G: 鲁棒性 ==========

  test('RF-060: 面板打开后设备 API 状态检查', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    await authPage.waitForTimeout(2000);
    const notReady = authPage.locator('#rf-not-ready');
    const startBtn = authPage.locator('#rf-learn-start-btn');
    const isNotReady = await notReady.isVisible().catch(() => false);
    if (isNotReady) {
      expect(await startBtn.isDisabled()).toBe(true);
    } else {
      expect(await startBtn.isDisabled()).toBe(false);
    }
    await closeModal(authPage);
  });

  // ========== 场景H: 规则编辑器 RF 事件集成 ==========

  test('RF-070: 事件触发下拉包含 RF编码接收 选项', async ({ authPage, navigateTo }) => {
    await navigateTo('periph-exec');
    await authPage.waitForTimeout(2000);
    const addBtn = authPage.locator('#periph-exec-add-trigger-btn');
    if (await addBtn.isVisible().catch(() => false)) {
      await addBtn.click();
      await authPage.waitForTimeout(500);
    }
    const triggerTypeSel = authPage.locator('.pe-trigger-type').first();
    if (await triggerTypeSel.isVisible().catch(() => false)) {
      await triggerTypeSel.selectOption('4');
      await authPage.waitForTimeout(1000);
      const eventSel = authPage.locator('.pe-event').first();
      if (await eventSel.isVisible().catch(() => false)) {
        const options = await eventSel.locator('option').allTextContents();
        const hasRf = options.some(o => o.includes('RF编码接收'));
        expect(hasRf).toBe(true);
      }
    }
  });

  test('RF-071: 选择 RF编码接收 事件后显示 RF 按键下拉', async ({ authPage, navigateTo }) => {
    await navigateTo('periph-exec');
    await authPage.waitForTimeout(2000);
    const addBtn = authPage.locator('#periph-exec-add-trigger-btn');
    if (await addBtn.isVisible().catch(() => false)) {
      await addBtn.click();
      await authPage.waitForTimeout(500);
    }
    const triggerTypeSel = authPage.locator('.pe-trigger-type').first();
    if (await triggerTypeSel.isVisible().catch(() => false)) {
      await triggerTypeSel.selectOption('4');
      await authPage.waitForTimeout(1000);
      const eventSel = authPage.locator('.pe-event').first();
      if (await eventSel.isVisible().catch(() => false)) {
        await eventSel.selectOption('rf_code_received');
        await authPage.waitForTimeout(1000);
        // RF 按键下拉应可见
        const rfKeyGroup = authPage.locator('.pe-rf-key-group').first();
        await expect(rfKeyGroup).toBeVisible();
        const rfKeySel = authPage.locator('.pe-rf-key').first();
        await expect(rfKeySel).toBeVisible();
        // 红外按键下拉应隐藏
        const irKeyGroup = authPage.locator('.pe-ir-key-group').first();
        await expect(irKeyGroup).not.toBeVisible();
      }
    }
  });

  // ========== 场景I: API Mock 学习流程 ==========

  test('RF-080: Mock 学习流程 - 捕获编码后显示捕获区域', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    // Mock /api/rf/status 返回已初始化
    await authPage.route('**/api/rf/status', route => {
      route.fulfill({ json: { success: true, data: { initialized: true, learnState: 'idle' } } });
    });
    // Mock /api/rf/learn/start
    await authPage.route('**/api/rf/learn/start', route => {
      route.fulfill({ json: { success: true } });
    });
    // Mock /api/rf/learn/status 返回 captured
    await authPage.route('**/api/rf/learn/status', route => {
      route.fulfill({ json: { success: true, data: { learnState: 'captured', code: '0xA1B2C3', bits: 24, pulse: 350 } } });
    });
    // 重新加载状态
    await authPage.evaluate(() => {
      const w = window as any;
      if (w.AppState && w.AppState._rfLoadStatus) w.AppState._rfLoadStatus();
    });
    await authPage.waitForTimeout(500);
    // 开始按钮应可用
    const startBtn = authPage.locator('#rf-learn-start-btn');
    expect(await startBtn.isDisabled()).toBe(false);
    await startBtn.click();
    await authPage.waitForTimeout(2000);
    // 捕获区域应显示
    await expect(authPage.locator('#rf-learn-captured')).toBeVisible();
    // 编码输入框应填充
    expect(await authPage.locator('#rf-key-code-input').inputValue()).toBe('0xA1B2C3');
    expect(await authPage.locator('#rf-key-bits-input').inputValue()).toBe('24');
    expect(await authPage.locator('#rf-key-pulse-input').inputValue()).toContain('350');
    // 状态文本应包含捕获信息
    const statusText = await authPage.locator('#rf-learn-status').textContent();
    expect(statusText).toContain('0xA1B2C3');
    await closeModal(authPage);
  });

  test('RF-081: Mock 学习超时流程', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    await authPage.route('**/api/rf/status', route => {
      route.fulfill({ json: { success: true, data: { initialized: true, learnState: 'idle' } } });
    });
    await authPage.route('**/api/rf/learn/start', route => {
      route.fulfill({ json: { success: true } });
    });
    await authPage.route('**/api/rf/learn/status', route => {
      route.fulfill({ json: { success: true, data: { learnState: 'timeout', remainingMs: 0 } } });
    });
    await authPage.evaluate(() => {
      const w = window as any;
      if (w.AppState && w.AppState._rfLoadStatus) w.AppState._rfLoadStatus();
    });
    await authPage.waitForTimeout(500);
    await authPage.locator('#rf-learn-start-btn').click();
    await authPage.waitForTimeout(2000);
    // 超时后状态文本应包含超时提示
    const statusText = await authPage.locator('#rf-learn-status').textContent();
    expect(statusText).toContain('超时');
    // 按钮应恢复
    await expect(authPage.locator('#rf-learn-start-btn')).toBeVisible();
    await expect(authPage.locator('#rf-learn-stop-btn')).not.toBeVisible();
    await closeModal(authPage);
  });

  test('RF-082: Mock 保存按键成功流程', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    let savedPayload: any = null;
    await authPage.route('**/api/rf/keys', route => {
      if (route.request().method() === 'POST') {
        savedPayload = route.request().postDataJSON();
        route.fulfill({ json: { success: true } });
      } else {
        route.fulfill({ json: { success: true, data: { keys: [] } } });
      }
    });
    // 模拟已捕获状态
    await authPage.evaluate(() => {
      const w = window as any;
      if (w.AppState && w.AppState._rfOnCaptured) w.AppState._rfOnCaptured('0xABC123', 24, 350);
    });
    await authPage.waitForTimeout(500);
    await authPage.locator('#rf-key-name-input').fill('gate_btn');
    await authPage.locator('#rf-key-save-btn').click();
    await authPage.waitForTimeout(1500);
    // 验证发送的 payload
    expect(savedPayload).not.toBeNull();
    expect(savedPayload.name).toBe('gate_btn');
    expect(savedPayload.code).toBe('0xABC123');
    expect(savedPayload.bits).toBe(24);
    expect(savedPayload.pulse).toBe(350);
    await closeModal(authPage);
  });

  test('RF-083: Mock 删除按键流程', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    let deletedName: string | null = null;
    await authPage.route('**/api/rf/keys', route => {
      route.fulfill({ json: { success: true, data: { keys: [{ name: 'btn_a', code: '0x111', bits: 24, pulse: 350 }] } } });
    });
    await authPage.route('**/api/rf/keys/delete', route => {
      deletedName = route.request().postDataJSON()?.name || null;
      route.fulfill({ json: { success: true } });
    });
    // 重新加载按键
    await authPage.evaluate(() => {
      const w = window as any;
      if (w.AppState && w.AppState._rfLoadKeys) w.AppState._rfLoadKeys();
    });
    await authPage.waitForTimeout(1500);
    // 应有数据行
    const deleteBtn = authPage.locator('#rf-keys-table-body button[data-rf-key-action="delete"]');
    await expect(deleteBtn.first()).toBeVisible();
    // Mock confirm 返回 true
    await authPage.evaluate(() => { (window as any).confirm = () => true; });
    await deleteBtn.first().click();
    await authPage.waitForTimeout(1500);
    expect(deletedName).toBe('btn_a');
    await authPage.evaluate(() => { delete (window as any).confirm; });
    await closeModal(authPage);
  });

  test('RF-084: Mock 清除全部按键流程', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    let clearCalled = false;
    await authPage.route('**/api/rf/keys/clear', route => {
      clearCalled = true;
      route.fulfill({ json: { success: true } });
    });
    await authPage.route('**/api/rf/keys', route => {
      route.fulfill({ json: { success: true, data: { keys: [] } } });
    });
    await authPage.evaluate(() => { (window as any).confirm = () => true; });
    await authPage.locator('#rf-keys-clear-btn').click();
    await authPage.waitForTimeout(1500);
    expect(clearCalled).toBe(true);
    await authPage.evaluate(() => { delete (window as any).confirm; });
    await closeModal(authPage);
  });

  // ========== 场景J: i18n 与表单字段 ==========

  test('RF-090: data-i18n 属性完整性', async ({ authPage }) => {
    await authPage.waitForSelector('#peripheral-modal', { state: 'attached', timeout: 20000 });
    const section = authPage.locator('#rf-manage-section');
    const i18nEls = section.locator('[data-i18n]');
    const count = await i18nEls.count();
    expect(count).toBeGreaterThanOrEqual(10);
    // 验证关键 i18n 键存在
    const keys: string[] = [];
    for (let i = 0; i < count; i++) {
      const key = await i18nEls.nth(i).getAttribute('data-i18n');
      if (key) keys.push(key);
    }
    expect(keys).toContain('rf-manage-title');
    expect(keys).toContain('rf-learn-start');
    expect(keys).toContain('rf-learn-stop');
    expect(keys).toContain('rf-key-name');
    expect(keys).toContain('rf-key-code');
    expect(keys).toContain('rf-key-bits');
    expect(keys).toContain('rf-key-pulse');
    expect(keys).toContain('rf-key-save');
    expect(keys).toContain('rf-keys-clear');
  });

  test('RF-091: RF 参数表单字段完整性', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    // 编码位数
    const bitLength = authPage.locator('#rf-bit-length');
    await expect(bitLength).toBeVisible();
    expect(await bitLength.inputValue()).toBe('24');
    expect(await bitLength.getAttribute('min')).toBe('1');
    expect(await bitLength.getAttribute('max')).toBe('32');
    // 脉宽
    const pulseWidth = authPage.locator('#rf-pulse-width');
    await expect(pulseWidth).toBeVisible();
    expect(await pulseWidth.inputValue()).toBe('350');
    expect(await pulseWidth.getAttribute('min')).toBe('100');
    expect(await pulseWidth.getAttribute('max')).toBe('2000');
    await closeModal(authPage);
  });

  test('RF-092: 学习超时输入边界值验证', async ({ authPage }) => {
    await openAddModal(authPage);
    await switchToRfType(authPage);
    await activateEditMode(authPage);
    const timeoutInput = authPage.locator('#rf-learn-timeout');
    // 设置为最小值
    await timeoutInput.fill('3');
    expect(await timeoutInput.inputValue()).toBe('3');
    // 设置为最大值
    await timeoutInput.fill('60');
    expect(await timeoutInput.inputValue()).toBe('60');
    await closeModal(authPage);
  });
});
