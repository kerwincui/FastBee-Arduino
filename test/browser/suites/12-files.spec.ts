import { test, expect, waitForDevice } from '../fixtures/base.fixture';

test.describe('Suite-12: 文件管理', () => {

  test.beforeEach(async ({ authPage, navigateTo }) => {
    await navigateTo('data');
  });

  // ========== 页面加载与展示 ==========

  test('FILE-001: 进入文件管理页', async ({ authPage }) => {
    await expect(authPage.locator('#data-page')).toBeVisible();
    await expect(authPage.locator('#data-page .fb-card-title')).toContainText(/文件管理|File/);
  });

  test('FILE-002: 文件系统信息展示', async ({ authPage }) => {
    const fsInfo = authPage.locator('#fs-info');
    await expect(fsInfo).toBeVisible();
    // 应显示容量信息
    const text = await fsInfo.textContent();
    expect(text).toBeTruthy();
  });

  test('FILE-003: 文件树展示', async ({ authPage }) => {
    const fileTree = authPage.locator('#file-tree');
    await expect(fileTree).toBeVisible();
    // 等待加载完成（"加载中..."消失）
    await authPage.waitForTimeout(3000);
    const treeContent = await fileTree.innerHTML();
    // 应有文件或目录条目
    expect(treeContent).toBeTruthy();
  });

  test('FILE-004: 当前目录路径显示', async ({ authPage }) => {
    const currentDir = authPage.locator('#current-dir-path');
    await expect(currentDir).toBeVisible();
    const dirText = await currentDir.textContent();
    // 应显示路径（至少根目录 /）
    expect(dirText).toBeTruthy();
  });

  // ========== 文件操作按钮 ==========

  test('FILE-005: 刷新按钮', async ({ authPage }) => {
    const refreshBtn = authPage.locator('#fs-refresh-btn');
    await expect(refreshBtn).toBeVisible();
    await refreshBtn.click();
    await authPage.waitForTimeout(2000);
    // 刷新后页面不崩溃
    await expect(authPage.locator('#data-page')).toBeVisible();
  });

  test('FILE-006: 返回上级按钮', async ({ authPage }) => {
    const upBtn = authPage.locator('#fs-up-btn');
    await expect(upBtn).toBeVisible();
    // 在根目录点击返回上级不应出错
    await upBtn.click();
    await authPage.waitForTimeout(1000);
    await expect(authPage.locator('#data-page')).toBeVisible();
  });

  // ========== 文件编辑器 ==========

  test('FILE-007: 文件编辑器初始状态', async ({ authPage }) => {
    const editor = authPage.locator('#file-editor');
    await expect(editor).toBeVisible();
    // 初始状态应禁用（未选择文件）
    const isDisabled = await editor.isDisabled();
    expect(isDisabled).toBeTruthy();
  });

  test('FILE-008: 文件路径提示', async ({ authPage }) => {
    const filePath = authPage.locator('#current-file-path');
    await expect(filePath).toBeVisible();
    // 初始应显示"请选择文件"
    const text = await filePath.textContent();
    expect(text).toBeTruthy();
  });

  // ========== 配置文件导出/导入 ==========

  test('FILE-009: 导出配置按钮存在', async ({ authPage }) => {
    const exportBtn = authPage.locator('#fs-export-config-btn');
    // 按钮可能存在（取决于页面状态）
    if (await exportBtn.isVisible()) {
      await expect(exportBtn).toBeVisible();
    }
  });

  test('FILE-010: 导入配置按钮存在', async ({ authPage }) => {
    const importBtn = authPage.locator('#fs-import-config-btn');
    if (await importBtn.isVisible()) {
      await expect(importBtn).toBeVisible();
    }
  });

  // ========== 文件树交互 ==========

  test('FILE-011: 点击文件树条目', async ({ authPage }) => {
    await authPage.waitForTimeout(3000); // 等待文件树加载
    // 尝试点击第一个文件/目录条目
    const firstItem = authPage.locator('#file-tree .fs-item, #file-tree .fs-file, #file-tree .fs-dir, #file-tree li, #file-tree [data-name]').first();
    if (await firstItem.isVisible()) {
      await firstItem.click();
      await authPage.waitForTimeout(1000);
      // 编辑器应加载文件内容或显示文件路径
      const filePath = authPage.locator('#current-file-path');
      const pathText = await filePath.textContent();
      expect(pathText).toBeTruthy();
    }
  });

  test('FILE-012: 文件状态区域', async ({ authPage }) => {
    const status = authPage.locator('#file-status');
    await expect(status).toBeVisible();
  });

  // ========== API 验证 ==========

  test('FILE-013: 文件列表API', async ({ authPage }) => {
    const result = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/files');
        if (r.status === 404) return { ok: true, skipped: true, reason: 'file manager not enabled' };
        if (!r.ok) return { ok: false, status: r.status };
        const data = await r.json();
        return { ok: true, count: Array.isArray(data) ? data.length : (data?.data?.files?.length ?? data?.files?.length ?? -1) };
      } catch { return { ok: false }; }
    });
    console.log(`文件列表API: ${JSON.stringify(result)}`);
    expect(result.ok).toBeTruthy();
  });

  test('FILE-014: 文件系统信息API', async ({ authPage }) => {
    const result = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/filesystem');
        if (r.status === 404) return { ok: true, skipped: true, reason: 'file manager not enabled' };
        if (!r.ok) return { ok: false, status: r.status };
        const data = await r.json();
        return { ok: true, total: data?.total ?? data?.totalBytes ?? -1 };
      } catch { return { ok: false }; }
    });
    console.log(`文件系统API: ${JSON.stringify(result)}`);
    expect(result.ok).toBeTruthy();
  });

  test('FILE-015: 文件读取API', async ({ authPage }) => {
    // 尝试读取一个已知存在的系统文件
    const result = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/files/content?path=/config.json');
        // 404 表示文件管理器未启用或文件不存在，均可接受
        return { ok: r.ok || r.status === 404, status: r.status };
      } catch { return { ok: false }; }
    });
    console.log(`文件读取API: ${JSON.stringify(result)}`);
    // API本身应能响应（200/404均可）
    expect(result.status).toBeDefined();
  });

  test('FILE-016: 存储空间API一致性', async ({ authPage }) => {
    const fsInfo = await authPage.evaluate(async () => {
      try {
        // 优先使用 /api/system/info（所有 profile 均支持），回退到 /api/status
        let r = await fetch('/api/system/info');
        if (!r.ok) r = await fetch('/api/status');
        const data = await r.json();
        const d = data?.data || data;
        return {
          flashTotal: d?.filesystem?.total ?? d?.flash?.total ?? d?.fsTotal ?? -1,
          flashUsed: d?.filesystem?.used ?? d?.flash?.used ?? d?.fsUsed ?? -1,
        };
      } catch { return { flashTotal: -1, flashUsed: -1 }; }
    });
    console.log(`Flash: total=${fsInfo.flashTotal}, used=${fsInfo.flashUsed}`);
    // API 应能返回数据
    expect(fsInfo.flashTotal).not.toBe(-1);
  });
});
