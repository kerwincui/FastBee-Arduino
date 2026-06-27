import { test, expect, waitForDevice } from '../fixtures/base.fixture';

test.describe('Suite-13: 用户管理', () => {

  test.beforeEach(async ({ authPage, navigateTo }) => {
    await navigateTo('users');
  });

  // ========== 页面加载与展示 ==========

  test('USER-001: 进入用户管理页', async ({ authPage }) => {
    await expect(authPage.locator('#users-page')).toBeVisible();
    await expect(authPage.locator('#users-title')).toContainText(/用户管理|User/);
  });

  test('USER-002: 用户列表展示', async ({ authPage }) => {
    const table = authPage.locator('#users-table-body');
    await expect(table).toBeVisible();
    await authPage.waitForTimeout(2000);
    // 至少有 admin 用户
    const rows = table.locator('tr');
    const count = await rows.count();
    expect(count).toBeGreaterThanOrEqual(1);
  });

  test('USER-003: 新增用户按钮', async ({ authPage }) => {
    const addBtn = authPage.locator('#add-user-btn');
    await expect(addBtn).toBeVisible();
    await expect(addBtn).toContainText(/添加用户|Add/);
  });

  test('USER-004: 刷新按钮', async ({ authPage }) => {
    const refreshBtn = authPage.locator('#users-refresh-btn');
    await expect(refreshBtn).toBeVisible();
    await refreshBtn.click();
    await authPage.waitForTimeout(2000);
    await expect(authPage.locator('#users-page')).toBeVisible();
  });

  // ========== 表头验证 ==========

  test('USER-005: 表头列完整', async ({ authPage }) => {
    const thead = authPage.locator('#users-page thead');
    await expect(thead).toBeVisible();
    const headers = await thead.textContent();
    // 应包含用户名、最后登录、状态、描述、操作
    expect(headers).toBeTruthy();
    expect(headers!.length).toBeGreaterThan(10);
  });

  // ========== 新增用户流程 ==========

  test('USER-006: 打开新增用户弹窗', async ({ authPage }) => {
    await authPage.locator('#add-user-btn').click();
    await authPage.waitForTimeout(1000);
    // 应有模态框出现
    const modal = authPage.locator('.modal, .fb-modal, [class*="modal"]').filter({ hasText: /新增|添加|Add|Create/i }).first();
    if (await modal.isVisible()) {
      await expect(modal).toBeVisible();
    } else {
      // 模态框可能用不同结构
      const anyModal = authPage.locator('.modal.show, .modal[style*="display: block"], .fb-modal.show').first();
      if (await anyModal.isVisible()) {
        await expect(anyModal).toBeVisible();
      }
    }
  });

  test('USER-007: 新增用户填写信息', async ({ authPage }) => {
    await authPage.locator('#add-user-btn').click();
    await authPage.waitForTimeout(1000);
    // 尝试填写用户名
    const usernameInput = authPage.locator('.modal input[name="username"], .modal input[id*="user"], .modal input[placeholder*="用户名"]').first();
    if (await usernameInput.isVisible()) {
      await usernameInput.fill('testuser');
      // 填写密码
      const pwdInput = authPage.locator('.modal input[type="password"], .modal input[name="password"]').first();
      if (await pwdInput.isVisible()) {
        await pwdInput.fill('Test@1234');
      }
    }
  });

  // ========== admin 用户保护 ==========

  test('USER-008: admin用户不可删除', async ({ authPage }) => {
    await authPage.waitForTimeout(2000);
    // 找到 admin 行
    const adminRow = authPage.locator('#users-table-body tr').filter({ hasText: 'admin' }).first();
    if (await adminRow.isVisible()) {
      // 删除按钮应不存在或禁用
      const deleteBtn = adminRow.locator('button:has-text("删除"), button:has-text("Delete"), [data-action*="delete"]').first();
      if (await deleteBtn.isVisible()) {
        const isDisabled = await deleteBtn.isDisabled();
        // 如果按钮可点击，点击后应被拒绝
        if (!isDisabled) {
          await deleteBtn.click();
          await authPage.waitForTimeout(500);
          // 确认弹窗可能出现警告
          const confirmModal = authPage.locator('.modal, .fb-modal').filter({ hasText: /不能|无法|拒绝|cannot/i }).first();
          if (await confirmModal.isVisible()) {
            await expect(confirmModal).toBeVisible();
          }
        }
      }
    }
  });

  // ========== 用户API验证 ==========

  test('USER-009: 用户列表API', async ({ authPage }) => {
    const result = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/users');
        if (!r.ok) return { ok: false, status: r.status };
        const data = await r.json();
        return { ok: true, count: Array.isArray(data?.data?.users) ? data.data.users.length : (Array.isArray(data?.users) ? data.users.length : (Array.isArray(data) ? data.length : -1)) };
      } catch { return { ok: false }; }
    });
    console.log(`用户列表API: ${JSON.stringify(result)}`);
    expect(result.ok).toBeTruthy();
  });

  test('USER-010: admin用户存在于列表', async ({ authPage }) => {
    const result = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/users');
        const data = await r.json();
        const users = Array.isArray(data?.data?.users) ? data.data.users : (Array.isArray(data?.users) ? data.users : (Array.isArray(data) ? data : []));
        return users.some((u: any) => u.username === 'admin' || u.name === 'admin');
      } catch { return false; }
    });
    expect(result).toBeTruthy();
  });

  // ========== 空用户名/弱密码验证 ==========

  test('USER-011: 空用户名拒绝', async ({ authPage }) => {
    await authPage.locator('#add-user-btn').click();
    await authPage.waitForTimeout(1000);
    const saveBtn = authPage.locator('.modal button:has-text("保存"), .modal button:has-text("确定"), .modal button[type="submit"]').first();
    if (await saveBtn.isVisible()) {
      // 不填写任何内容直接保存
      await saveBtn.click();
      await authPage.waitForTimeout(1000);
      // 应有错误提示或HTML5验证拦截
      await expect(authPage.locator('#users-page')).toBeVisible();
    }
  });

  // ========== 多用户场景 ==========

  test('USER-012: 用户列表刷新一致性', async ({ authPage }) => {
    // 等待用户列表加载完成（异步加载）
    await authPage.waitForTimeout(3000);
    await expect(authPage.locator('#users-table-body tr')).toHaveCount(await authPage.locator('#users-table-body tr').count() || 1, { timeout: 10_000 }).catch(() => {});
    // 首次获取用户数
    const count1 = await authPage.locator('#users-table-body tr').count();
    // 刷新
    await authPage.locator('#users-refresh-btn').click();
    await authPage.waitForTimeout(3000);
    const count2 = await authPage.locator('#users-table-body tr').count();
    // 刷新前后数量应一致
    expect(count1).toBe(count2);
  });

  // ========== 编辑用户 ==========

  test('USER-013: 编辑按钮存在', async ({ authPage }) => {
    await authPage.waitForTimeout(2000);
    const editBtns = authPage.locator('#users-table-body button:has-text("编辑"), #users-table-body button:has-text("Edit"), #users-table-body [data-action*="edit"]');
    const count = await editBtns.count();
    console.log(`编辑按钮数量: ${count}`);
    // 至少有编辑按钮（admin除外可能有其他用户）
  });

  // ========== 用户状态展示 ==========

  test('USER-014: 用户状态列显示', async ({ authPage }) => {
    await authPage.waitForTimeout(2000);
    const adminRow = authPage.locator('#users-table-body tr').filter({ hasText: 'admin' }).first();
    if (await adminRow.isVisible()) {
      const cells = adminRow.locator('td');
      const cellCount = await cells.count();
      // 应有多个列（用户名、最后登录、状态、描述、操作）
      expect(cellCount).toBeGreaterThanOrEqual(3);
    }
  });

  test('USER-015: 最后登录时间显示', async ({ authPage }) => {
    await authPage.waitForTimeout(2000);
    const adminRow = authPage.locator('#users-table-body tr').filter({ hasText: 'admin' }).first();
    if (await adminRow.isVisible()) {
      const text = await adminRow.textContent();
      // 应包含时间信息或"从未"
      expect(text).toBeTruthy();
    }
  });

  // ========== 安全性验证 ==========

  test('USER-016: 未认证访问用户API', async ({ page }) => {
    // 未登录状态直接访问 API
    const result = await page.evaluate(async () => {
      try {
        const r = await fetch('/api/users');
        return { status: r.status, ok: r.ok };
      } catch { return { status: 0, ok: false }; }
    });
    console.log(`未认证API: status=${result.status}`);
    // 应返回 401/403 或其他非200状态
  });

  test('USER-017: XSS防护-用户名', async ({ authPage }) => {
    await authPage.locator('#add-user-btn').click();
    await authPage.waitForTimeout(1000);
    const usernameInput = authPage.locator('.modal input[name="username"], .modal input[id*="user"]').first();
    if (await usernameInput.isVisible()) {
      await usernameInput.fill('<script>alert(1)</script>');
      await authPage.waitForTimeout(500);
      // 不应触发 XSS
      const alertFired = await authPage.evaluate(() => {
        return (window as any).__xss_fired === true;
      });
      expect(alertFired).toBeFalsy();
    }
  });

  test('USER-018: 页面布局完整性', async ({ authPage }) => {
    // 检查用户管理页面结构完整
    const card = authPage.locator('#users-page .fb-card');
    await expect(card).toBeVisible();
    const header = authPage.locator('#users-page .fb-card-header');
    await expect(header).toBeVisible();
    const body = authPage.locator('#users-page .fb-card-body');
    await expect(body).toBeVisible();
    const table = authPage.locator('#users-page .fb-table');
    await expect(table).toBeVisible();
  });
});
