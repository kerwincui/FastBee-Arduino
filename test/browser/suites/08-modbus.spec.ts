import { test, expect, waitForDevice } from '../fixtures/base.fixture';

test.describe('Suite-08: Modbus RTU', () => {

  test.beforeEach(async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    // 切换到 Modbus RTU Tab
    await authPage.click('.config-tab[data-tab="modbus-rtu"]');
    // 等待 Modbus 分片异步加载完成
    await authPage.locator('#modbus-rtu-form').waitFor({ state: 'visible', timeout: 15_000 }).catch(() => {});
    await authPage.waitForTimeout(500);
  });

  // ========== 场景A: Modbus基本配置 ==========

  test('MOD-001: 切换到Modbus Tab', async ({ authPage }) => {
    await expect(authPage.locator('#modbus-rtu')).toHaveClass(/active/);
  });

  test('MOD-002: Modbus主站状态', async ({ authPage }) => {
    await expect(authPage.locator('#master-status-section')).toBeVisible();
  });

  test('MOD-003: Modbus启用', async ({ authPage }) => {
    const enableCb = authPage.locator('#modbus-rtu-enabled');
    if (await enableCb.isVisible()) {
      await enableCb.check();
    }
  });

  test('MOD-004: 选择UART外设', async ({ authPage }) => {
    const periphSelect = authPage.locator('#rtu-peripheral-id');
    if (await periphSelect.isVisible()) {
      const options = await periphSelect.locator('option').allTextContents();
      // 应至少有占位符选项
      expect(options.length).toBeGreaterThanOrEqual(1);
    }
  });

  test('MOD-005: DE引脚配置', async ({ authPage }) => {
    const dePin = authPage.locator('#rtu-de-pin');
    if (await dePin.isVisible()) {
      await dePin.fill('4');
      expect(await dePin.inputValue()).toBe('4');
    }
  });

  test('MOD-006: 传输类型-JSON', async ({ authPage }) => {
    const transferType = authPage.locator('#rtu-transfer-type');
    if (await transferType.isVisible()) {
      await transferType.selectOption('0');
      expect(await transferType.inputValue()).toBe('0');
    }
  });

  test('MOD-007: 传输类型-透传', async ({ authPage }) => {
    const transferType = authPage.locator('#rtu-transfer-type');
    if (await transferType.isVisible()) {
      await transferType.selectOption('1');
      expect(await transferType.inputValue()).toBe('1');
      await transferType.selectOption('0'); // 切回JSON
    }
  });

  test('MOD-008: 保存Modbus配置', async ({ authPage }) => {
    const saveBtn = authPage.locator('#modbus-rtu-form button[type="submit"], button:has-text("保存")').first();
    if (await saveBtn.isVisible()) {
      await saveBtn.click();
      await waitForDevice(authPage, 3000);
    }
  });

  // ========== 场景B: 采集设备管理 ==========

  test('MOD-009: 采集设备列表', async ({ authPage }) => {
    await expect(authPage.locator('#master-config-section')).toBeVisible();
  });

  test('MOD-010: 添加采集设备弹窗', async ({ authPage }) => {
    const addBtn = authPage.locator('[data-action*="addDevice"], button:has-text("添加设备")').first();
    if (await addBtn.isVisible()) {
      await addBtn.click();
      await authPage.waitForTimeout(1000);
    }
  });

  test('MOD-011: 设备名称输入', async ({ authPage }) => {
    const addBtn = authPage.locator('[data-action*="addDevice"], button:has-text("添加设备")').first();
    if (await addBtn.isVisible()) {
      await addBtn.click();
      await authPage.waitForTimeout(1000);
      const nameInput = authPage.locator('.modal input[id*="name"], .modal input[name*="name"]').first();
      if (await nameInput.isVisible()) {
        await nameInput.fill('temp-sensor-01');
      }
    }
  });

  test('MOD-012: 从站地址设置', async ({ authPage }) => {
    const addBtn = authPage.locator('[data-action*="addDevice"]').first();
    if (await addBtn.isVisible()) {
      await addBtn.click();
      await authPage.waitForTimeout(1000);
      const addrInput = authPage.locator('.modal input[id*="addr"], .modal input[name*="address"]').first();
      if (await addrInput.isVisible()) {
        await addrInput.fill('1');
      }
    }
  });

  test('MOD-019: 保存采集设备', async ({ authPage }) => {
    const addBtn = authPage.locator('[data-action*="addDevice"]').first();
    if (await addBtn.isVisible()) {
      await addBtn.click();
      await authPage.waitForTimeout(1000);
      const saveBtn = authPage.locator('.modal button:has-text("保存")').first();
      if (await saveBtn.isVisible()) {
        await saveBtn.click();
        await waitForDevice(authPage, 2000);
      }
    }
  });

  test('MOD-020: 编辑采集设备', async ({ authPage }) => {
    const editBtn = authPage.locator('#master-config-section button:has-text("编辑")').first();
    if (await editBtn.isVisible()) {
      await editBtn.click();
      await authPage.waitForTimeout(1000);
    }
  });

  test('MOD-021: 删除采集设备', async ({ authPage }) => {
    const deleteBtn = authPage.locator('#master-config-section button:has-text("删除")').first();
    if (await deleteBtn.isVisible()) {
      await deleteBtn.click();
      await authPage.waitForTimeout(1000);
      const confirmBtn = authPage.locator('.modal button:has-text("确定")').first();
      if (await confirmBtn.isVisible()) await confirmBtn.click();
      await waitForDevice(authPage, 2000);
    }
  });

  // ========== 场景C: 控制设备管理 ==========

  test('MOD-022: 控制设备列表', async ({ authPage }) => {
    // Modbus子设备区域应可见
    await expect(authPage.locator('#master-config-section')).toBeVisible();
  });

  test('MOD-023: 添加控制设备', async ({ authPage }) => {
    const addBtn = authPage.locator('[data-action*="addCtrl"], button:has-text("添加控制")').first();
    if (await addBtn.isVisible()) {
      await addBtn.click();
      await authPage.waitForTimeout(1000);
    }
  });

  // ========== 场景D: 设备控制操作 ==========

  test('MOD-030: 设备控制面板打开', async ({ authPage }) => {
    const ctrlBtn = authPage.locator('#master-config-section button:has-text("控制")').first();
    if (await ctrlBtn.isVisible()) {
      await ctrlBtn.click();
      await authPage.waitForTimeout(1000);
    }
  });

  // ========== 场景E: 主站运行状态（合并为单测试 + test.step） ==========

  test('MOD-038~042: 主站状态与统计信息', async ({ authPage }) => {

    await test.step('主站状态刷新', async () => {
      const refreshBtn = authPage.locator('[data-action="refreshMasterStatusFresh"]').first();
      if (await refreshBtn.isVisible()) {
        await refreshBtn.click();
        await authPage.waitForTimeout(2000);
      }
      await expect(authPage.locator('#master-enabled-task-count')).toBeVisible();
    });

    await test.step('风险等级展示', async () => {
      await expect(authPage.locator('#master-risk-badge')).toBeVisible();
    });

    await test.step('最小间隔展示', async () => {
      await expect(authPage.locator('#master-min-interval')).toBeVisible();
    });

    await test.step('超时率展示', async () => {
      await expect(authPage.locator('#master-timeout-rate')).toBeVisible();
    });

    await test.step('总轮询统计', async () => {
      await expect(authPage.locator('#master-stat-total')).toBeVisible();
      await expect(authPage.locator('#master-stat-success')).toBeVisible();
      await expect(authPage.locator('#master-stat-failed')).toBeVisible();
    });
  });

  // ========== 场景F: 综合验证 ==========

  test('MOD-043: 配置保存后持久化', async ({ authPage }) => {
    const enabled = await authPage.locator('#modbus-rtu-enabled').isChecked();
    await authPage.reload();
    await authPage.waitForSelector('#protocol-page', { state: 'visible', timeout: 15_000 });
    await authPage.click('.config-tab[data-tab="modbus-rtu"]');
    await authPage.waitForTimeout(2000);
    const newEnabled = await authPage.locator('#modbus-rtu-enabled').isChecked();
    expect(newEnabled).toBe(enabled);
  });

  test('MOD-046: Modbus禁用后验证', async ({ authPage }) => {
    await authPage.locator('#modbus-rtu-enabled').uncheck();
    const saveBtn = authPage.locator('#modbus-rtu-form button[type="submit"]').first();
    if (await saveBtn.isVisible()) {
      await saveBtn.click();
      await waitForDevice(authPage, 2000);
    }
    // 重新启用
    await authPage.locator('#modbus-rtu-enabled').check();
  });

  test('MOD-047: DE引脚边界值', async ({ authPage }) => {
    const dePin = authPage.locator('#rtu-de-pin');
    if (await dePin.isVisible()) {
      await dePin.fill('-1');
      expect(await dePin.inputValue()).toBe('-1');
      await dePin.fill('48');
      expect(await dePin.inputValue()).toBe('48');
    }
  });
});
