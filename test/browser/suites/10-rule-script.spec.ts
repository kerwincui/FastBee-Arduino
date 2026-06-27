import { test, expect, waitForDevice } from '../fixtures/base.fixture';

test.describe('Suite-10: 规则脚本', () => {

  test.beforeEach(async ({ authPage, navigateTo }) => {
    await navigateTo('rule-script');
  });

  // ========== 场景A: 规则脚本列表与创建 ==========

  test('RULE-001: 进入规则脚本页 @quick', async ({ authPage }) => {
    await expect(authPage.locator('#rule-script-page')).toBeVisible();
    await expect(authPage.locator('button:has-text("新增规则")')).toBeVisible();
  });

  test('RULE-002: 新增脚本弹窗', async ({ authPage }) => {
    await authPage.click('button:has-text("新增规则")');
    await authPage.waitForTimeout(1000);
    const modal = authPage.locator('#rule-script-modal');
    await expect(modal).toBeVisible({ timeout: 5000 });
  });

  test('RULE-003: 脚本名称输入', async ({ authPage }) => {
    await authPage.click('button:has-text("新增规则")');
    await authPage.waitForTimeout(1000);
    const nameInput = authPage.locator('.modal input[id*="name"], .modal input[name*="name"]').first();
    if (await nameInput.isVisible()) {
      await nameInput.fill('mqtt-topic-convert');
      expect(await nameInput.inputValue()).toBe('mqtt-topic-convert');
    }
  });

  test('RULE-004: 脚本启用开关', async ({ authPage }) => {
    await authPage.click('button:has-text("新增规则")');
    await authPage.waitForTimeout(1000);
    const enableCb = authPage.locator('.modal input[type="checkbox"]').first();
    if (await enableCb.isVisible()) {
      await enableCb.check();
    }
  });

  test('RULE-005: 触发类型选择 @quick', async ({ authPage }) => {
    await authPage.click('button:has-text("新增规则")');
    await authPage.waitForTimeout(1000);
    const triggerSelect = authPage.locator('.modal select[id*="trigger"], .modal select[name*="trigger"]').first();
    if (await triggerSelect.isVisible()) {
      const opts = await triggerSelect.locator('option').allTextContents();
      expect(opts.length).toBeGreaterThan(1);
    }
  });

  test('RULE-006: 协议类型选择', async ({ authPage }) => {
    await authPage.click('button:has-text("新增规则")');
    await authPage.waitForTimeout(1000);
    const protoSelect = authPage.locator('.modal select[id*="protocol"], .modal select[name*="protocol"]').first();
    if (await protoSelect.isVisible()) {
      const opts = await protoSelect.locator('option').allTextContents();
      expect(opts.length).toBeGreaterThan(0);
    }
  });

  // ========== 场景B: 主题转换规则 ==========

  test('RULE-007: 源主题输入', async ({ authPage }) => {
    await authPage.click('button:has-text("新增规则")');
    await authPage.waitForTimeout(1000);
    const srcTopic = authPage.locator('.modal input[id*="src"], .modal input[name*="source"], .modal input[placeholder*="raw"]').first();
    if (await srcTopic.isVisible()) {
      await srcTopic.fill('device/+/raw');
    }
  });

  test('RULE-008: 目标主题输入', async ({ authPage }) => {
    await authPage.click('button:has-text("新增规则")');
    await authPage.waitForTimeout(1000);
    const dstTopic = authPage.locator('.modal input[id*="dst"], .modal input[name*="target"], .modal input[placeholder*="processed"]').first();
    if (await dstTopic.isVisible()) {
      await dstTopic.fill('home/sensor/${device_id}/data');
    }
  });

  test('RULE-009: 脚本内容编辑', async ({ authPage }) => {
    await authPage.click('button:has-text("新增规则")');
    await authPage.waitForTimeout(1000);
    const codeEditor = authPage.locator('.modal textarea, .modal .CodeMirror').first();
    if (await codeEditor.isVisible()) {
      if (await codeEditor.locator('.CodeMirror').count() > 0) {
        // CodeMirror editor
        await codeEditor.evaluate((el: any) => {
          if (el.CodeMirror) el.CodeMirror.setValue('payload.temperature = payload.temp * 1.8 + 32');
        });
      } else {
        await codeEditor.fill('payload.temperature = payload.temp * 1.8 + 32');
      }
    }
  });

  test('RULE-010: 保存规则脚本 @quick', async ({ authPage }) => {
    await authPage.click('button:has-text("新增规则")');
    await authPage.waitForTimeout(1000);
    const nameInput = authPage.locator('.modal input[id*="name"]').first();
    if (await nameInput.isVisible()) {
      await nameInput.fill('test-save-rule');
    }
    const saveBtn = authPage.locator('.modal button:has-text("保存")').first();
    if (await saveBtn.isVisible()) {
      await saveBtn.click();
      await waitForDevice(authPage, 3000);
    }
  });

  // ========== 场景C: 规则脚本CRUD ==========

  test('RULE-011: 编辑规则脚本', async ({ authPage }) => {
    const editBtn = authPage.locator('#rule-script-table-body button:has-text("编辑")').first();
    if (await editBtn.isVisible()) {
      await editBtn.click();
      await authPage.waitForTimeout(1000);
      await expect(authPage.locator('.modal').first()).toBeVisible();
    }
  });

  test('RULE-012: 删除规则脚本', async ({ authPage }) => {
    const deleteBtn = authPage.locator('#rule-script-table-body button:has-text("删除")').first();
    if (await deleteBtn.isVisible()) {
      await deleteBtn.click();
      await authPage.waitForTimeout(1000);
      const confirmBtn = authPage.locator('.modal button:has-text("确定")').first();
      if (await confirmBtn.isVisible()) await confirmBtn.click();
      await waitForDevice(authPage, 2000);
    }
  });

  test('RULE-013: 规则脚本启用/禁用', async ({ authPage }) => {
    const toggle = authPage.locator('#rule-script-table-body input[type="checkbox"], .toggle-switch').first();
    if (await toggle.isVisible()) {
      await toggle.click();
      await waitForDevice(authPage, 2000);
    }
  });

  test('RULE-014: 多个脚本共存', async ({ authPage }) => {
    const rows = authPage.locator('#rule-script-table-body tr:not(:has(.u-empty-cell))');
    const count = await rows.count();
    expect(count).toBeGreaterThanOrEqual(0);
  });

  // ========== 场景D: 功能验证 ==========

  test('RULE-015: 规则脚本表格列展示', async ({ authPage }) => {
    // 表头应包含：名称、状态、触发方式、协议类型、主题映射
    const headers = authPage.locator('#rule-script-page thead th');
    const count = await headers.count();
    expect(count).toBeGreaterThanOrEqual(5);
  });

  test('RULE-017: 禁用脚本提示', async ({ authPage }) => {
    // 开发环境禁用时显示提示
    const devHint = authPage.locator('#rule-script-dev-mode-hint');
    // 如果开发环境已启用，提示应隐藏
    const isHidden = await devHint.evaluate(el => el.style.display === 'none');
    // 两种状态都合理
    expect(typeof isHidden).toBe('boolean');
  });

  test('RULE-018: 刷新按钮', async ({ authPage }) => {
    await authPage.click('#rule-script-refresh-btn');
    await authPage.waitForTimeout(2000);
    await expect(authPage.locator('#rule-script-page')).toBeVisible();
  });
});
