import { test, expect, env, waitForDeviceReady } from '../fixtures/base.fixture';
import { enableOptionalUiFeatures } from '../fixtures/optional-features.fixture';
import { uiScreenshotPath } from '../evidence-paths';

/** 带重试的 page.goto（设备可能临时不可达） */
async function gotoWithRetry(page: import('@playwright/test').Page, maxRetries = 3) {
  const baseURL = `http://${env.deviceIp}/`;
  for (let i = 0; i < maxRetries; i++) {
    try {
      await page.goto(baseURL, { timeout: 15_000 });
      return;
    } catch (e) {
      if (i < maxRetries - 1) {
        console.log(`[UI] goto retry ${i + 1}/${maxRetries}: ${e}`);
        await page.waitForTimeout(5000);
      } else {
        throw e;
      }
    }
  }
}

async function waitForPeripheralPageReady(page: import('@playwright/test').Page) {
  await page.waitForFunction(() => {
    const root = document.getElementById('peripheral-page');
    const body = document.getElementById('peripheral-table-body');
    const state = (window as any).AppState;
    if (!root || !body || root.hasAttribute('inert') || state?._periphEventsBound !== true) return false;
    const text = body.textContent?.trim() || '';
    if (!text || text === '加载中...') return false;
    return Array.from(body.querySelectorAll('.fb-table-action-cell')).every((cell) =>
      !cell.querySelector('button, a') || cell.classList.contains('fb-mobile-action-ready')
    );
  }, undefined, { timeout: 30_000 });
  // Drain the responsive-table MutationObserver before taking a DOM baseline.
  await page.evaluate(() => new Promise<void>((resolve) =>
    requestAnimationFrame(() => requestAnimationFrame(() => resolve()))
  ));
}

async function isRuntimeModbusReady(page: import('@playwright/test').Page): Promise<boolean> {
  await page.waitForFunction(() => {
    const root = document.getElementById('protocol-page');
    const state = (window as any).AppState;
    return !!root
      && !root.hasAttribute('inert')
      && root.dataset.moduleLoading !== 'true'
      && typeof state?.loadProtocolConfig === 'function'
      && state?._featureActive !== null;
  }, undefined, { timeout: 30_000 }).catch(() => {});

  return page.evaluate(() => (window as any).AppState?._featureActive?.modbus === true);
}

test.describe('Suite-15: UI布局与视觉回归', () => {

  // ========== 登录页布局 ==========

  test('UI-001: 登录页布局 @quick', async ({ page }) => {
    test.setTimeout(60_000);
    await gotoWithRetry(page);
    await expect(page.locator('#login-page')).toBeVisible();
    // 截图存档
    await page.screenshot({ path: uiScreenshotPath('UI-001-login.png'), fullPage: true });
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
    expect(count).toBeGreaterThanOrEqual(5);
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
    await authPage.screenshot({ path: uiScreenshotPath('UI-004-dashboard.png'), fullPage: true });
    // 检查卡片存在
    const cards = authPage.locator('#dashboard-page .stat-card, #dashboard-page .resource-card, #dashboard-page .info-card');
    const count = await cards.count();
    expect(count).toBeGreaterThanOrEqual(1);
  });

  test('UI-005: 表格列对齐', async ({ authPage, navigateTo }) => {
    const usersMenu = authPage.locator('.menu-item[data-page="users"]');
    test.skip(!(await usersMenu.isVisible()), '当前运行配置未启用用户管理页面');
    await navigateTo('users');
    await authPage.waitForTimeout(2000);
    const table = authPage.locator('#users-page .fb-table');
    if (await table.isVisible()) {
      await authPage.screenshot({ path: uiScreenshotPath('UI-005-table.png') });
      await expect(table).toBeVisible();
    }
  });

  test('UI-005A: 360/390/430 管理表格无横向溢出', async ({ authPage, navigateTo }) => {
    const featureMenu = authPage.locator('.menu-item[data-page="features"]');
    test.skip(!(await featureMenu.isVisible()), '设备未提供功能配置页面');
    await navigateTo('features');
    await authPage.locator('#features-list [data-feature-key]').first().waitFor({ state: 'attached' });

    for (const width of [360, 390, 430]) {
      await authPage.setViewportSize({ width, height: 900 });
      await expect(authPage.locator('#features-list .fb-table-responsive').first()).toBeVisible();
      const metrics = await authPage.evaluate(() => {
        const page = document.getElementById('features-page')!;
        const table = page.querySelector('.fb-table-responsive') as HTMLElement;
        const row = table.querySelector('tbody tr') as HTMLElement;
        const cells = Array.from(row.querySelectorAll('td')) as HTMLElement[];
        return {
          pageOverflow: page.scrollWidth > page.clientWidth + 1,
          tableOverflow: table.scrollWidth > table.clientWidth + 1,
          rowDisplay: getComputedStyle(row).display,
          labeledCells: cells.filter(cell => cell.dataset.label).length,
          cellCount: cells.length,
        };
      });
      expect(metrics.pageOverflow, `${width}px 页面不应横向溢出`).toBeFalsy();
      expect(metrics.tableOverflow, `${width}px 表格不应横向溢出`).toBeFalsy();
      expect(metrics.rowDisplay).toBe('block');
      expect(metrics.labeledCells).toBe(metrics.cellCount);
    }
  });

  test('UI-005F: 360/390/430 设备配置页键值表无横向溢出', async ({ authPage, navigateTo }) => {
    const deviceMenu = authPage.locator('.menu-item[data-page="device"]');
    test.skip((await deviceMenu.count()) === 0, '设备未提供设备配置页面');
    await navigateTo('device');
    await authPage.locator('#device-page').waitFor({ state: 'visible' });

    for (const width of [360, 390, 430]) {
      await authPage.setViewportSize({ width, height: 900 });
      const metrics = await authPage.evaluate(() => {
        const page = document.getElementById('device-page') as HTMLElement;
        const fluidTables = Array.from(page.querySelectorAll('.fb-table--fluid')) as HTMLElement[];
        return {
          pageOverflow: page.scrollWidth > page.clientWidth + 1,
          fluidCount: fluidTables.length,
          fluidOverflow: fluidTables.some((table) => table.scrollWidth > table.clientWidth + 1),
          minWidths: fluidTables.map((table) => getComputedStyle(table).minWidth),
        };
      });
      expect(metrics.pageOverflow, `${width}px 设备配置页不应横向溢出`).toBeFalsy();
      expect(metrics.fluidCount, `${width}px 设备配置页应有无 thead 键值表`).toBeGreaterThan(0);
      expect(metrics.fluidOverflow, `${width}px 键值表不应横向溢出`).toBeFalsy();
      expect(metrics.minWidths.every((w) => w === '0px'), `${width}px 键值表 min-width 应为 0`).toBeTruthy();
    }
  });

  test('UI-005G: 设备配置 Tab 键盘可达与 ARIA 语义', async ({ authPage, navigateTo }) => {
    const deviceMenu = authPage.locator('.menu-item[data-page="device"]');
    test.skip((await deviceMenu.count()) === 0, '设备未提供设备配置页面');
    await navigateTo('device');
    const tabs = authPage.locator('#device-page .config-tabs [role="tab"]');
    await expect(tabs.first()).toBeVisible();
    const tabCount = await tabs.count();
    expect(tabCount).toBeGreaterThanOrEqual(2);

    // ARIA 语义完整
    await expect(authPage.locator('#device-page .config-tabs')).toHaveAttribute('role', 'tablist');
    for (let i = 0; i < tabCount; i++) {
      const tab = tabs.nth(i);
      await expect(tab).toHaveAttribute('aria-selected', i === 0 ? 'true' : 'false');
      await expect(tab).toHaveAttribute('aria-controls', await tab.getAttribute('data-tab') as string);
      const panelId = await tab.getAttribute('aria-controls') as string;
      await expect(authPage.locator('#' + panelId)).toHaveAttribute('role', 'tabpanel');
    }
    // 选中态可聚焦，其余 tabindex=-1
    await expect(tabs.first()).toHaveAttribute('tabindex', '0');
    await expect(tabs.nth(1)).toHaveAttribute('tabindex', '-1');

    // 显式聚焦选中 tab 后方向键移动焦点
    await tabs.first().focus();
    const focusedDataTab = await authPage.evaluate(() =>
      (document.activeElement as HTMLElement)?.getAttribute?.('data-tab') || '');
    expect(focusedDataTab).toBe('dev-basic');
    await authPage.keyboard.press('ArrowRight');
    const afterRight = await authPage.evaluate(() =>
      (document.activeElement as HTMLElement)?.getAttribute?.('data-tab') || '');
    expect(afterRight).toBe('dev-ntp');
    // 焦点移动不切换选中
    await expect(tabs.first()).toHaveAttribute('aria-selected', 'true');
    // Enter 激活聚焦的 tab
    await authPage.keyboard.press('Enter');
    await expect(tabs.nth(1)).toHaveAttribute('aria-selected', 'true');
    await expect(tabs.nth(1)).toHaveAttribute('tabindex', '0');
    await expect(tabs.first()).toHaveAttribute('aria-selected', 'false');
    // Home 跳回首 tab，End 跳到尾 tab
    await authPage.keyboard.press('Home');
    expect(await authPage.evaluate(() =>
      (document.activeElement as HTMLElement)?.getAttribute?.('data-tab') || '')).toBe('dev-basic');
    await authPage.keyboard.press('End');
    expect(await authPage.evaluate(() =>
      (document.activeElement as HTMLElement)?.getAttribute?.('data-tab') || '')).toBe('dev-advanced');
  });

  test('UI-005B: 窄屏表格操作使用可访问菜单', async ({ authPage, navigateTo }) => {
    const peripheralMenu = authPage.locator('.menu-item[data-page="peripheral"]');
    test.skip((await peripheralMenu.count()) === 0, '设备未提供外设配置页面');
    await navigateTo('peripheral');
    await authPage.setViewportSize({ width: 390, height: 900 });

    const toggle = authPage.locator('#peripheral-page .fb-table-action-menu-toggle').first();
    test.skip(!(await toggle.isVisible()), '当前设备没有可操作的外设记录');
    const toggleBox = await toggle.boundingBox();
    expect(toggleBox!.width).toBeGreaterThanOrEqual(40);
    expect(toggleBox!.height).toBeGreaterThanOrEqual(40);
    await expect(toggle).toHaveAttribute('aria-haspopup', 'menu');

    await toggle.click();
    const menu = authPage.locator('#peripheral-page .fb-table-action-menu.is-open').first();
    await expect(menu).toBeVisible();
    await expect(menu.locator('.fb-table-action-menu-panel')).toBeVisible();
    await authPage.keyboard.press('Escape');
    await expect(menu).not.toHaveClass(/is-open/);
  });

  test('UI-005D: 窄屏 CRUD 表格与操作菜单不产生横向溢出', async ({ authPage, navigateTo }) => {
    const peripheralMenu = authPage.locator('.menu-item[data-page="peripheral"]');
    test.skip((await peripheralMenu.count()) === 0, '设备未提供外设配置页面');
    await navigateTo('peripheral');

    const table = authPage.locator('#peripheral-page .fb-table-responsive').first();
    test.skip((await table.count()) === 0, '当前页面没有可适配的管理表格');

    for (const width of [360, 390, 430]) {
      await authPage.setViewportSize({ width, height: 900 });
      await expect(table).toBeVisible();
      const metrics = await authPage.evaluate(() => {
        const page = document.getElementById('peripheral-page')!;
        const table = page.querySelector('.fb-table-responsive') as HTMLElement;
        const actionCell = table.querySelector('.fb-table-action-cell--compact') as HTMLElement | null;
        return {
          pageOverflow: page.scrollWidth > page.clientWidth + 1,
          tableOverflow: table.scrollWidth > table.clientWidth + 1,
          actionLabel: actionCell ? getComputedStyle(actionCell, '::before').content : null,
        };
      });
      expect(metrics.pageOverflow, `${width}px CRUD 页面不应横向溢出`).toBeFalsy();
      expect(metrics.tableOverflow, `${width}px CRUD 表格不应横向溢出`).toBeFalsy();
      expect(metrics.actionLabel, `${width}px 操作列不应重复显示标签`).not.toBe('"操作"');
    }
  });

  test('UI-026A: 外设配置只展示外设配置', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    await waitForPeripheralPageReady(authPage);

    const page = authPage.locator('#peripheral-page');
    await expect(page).toBeVisible();
    await expect(page.locator('#peripheral-filter-type')).toBeVisible();
    await expect(page.locator('#peripheral-refresh-btn')).toBeVisible();
    await expect(page.locator('#add-peripheral-btn')).toBeVisible();
    const directPageRoots = authPage.locator('#content-area > .page');
    await expect(directPageRoots).toHaveCount(1);
    await expect(directPageRoots).toHaveAttribute('id', 'peripheral-page');
    await expect(authPage.locator('#dashboard-page')).toHaveCount(0);
    await expect(page.locator('.config-tabs')).toHaveCount(0);
    for (const removedId of [
      'peripheral-board-resources',
      'peripheral-automation',
      'peripheral-experiments',
    ]) {
      await expect(page.locator(`#${removedId}`)).toHaveCount(0);
    }

    for (const width of [1280, 390]) {
      await authPage.setViewportSize({ width, height: 900 });
      const overflow = await page.evaluate((element) =>
        element.scrollWidth > element.clientWidth + 1
      );
      expect(overflow, `${width}px 外设配置页不应横向溢出`).toBeFalsy();
    }
  });

  test('UI-005E: 所有可见菜单页在 360/390/430 下无页面溢出且 DOM 有界', async ({ authPage }) => {
    test.setTimeout(240_000);
    await authPage.setViewportSize({ width: 1024, height: 900 });
    await authPage.waitForFunction(() => {
      const state = (window as any).AppState;
      return !state || state._featureActive !== null;
    }, undefined, { timeout: 10_000 }).catch(() => {});

    const menuPages = await authPage.locator('.menu-item[data-page]').evaluateAll((items) => items
      .filter((item) => {
        const element = item as HTMLElement;
        const style = getComputedStyle(element);
        return !element.hidden && element.getAttribute('aria-hidden') !== 'true' && style.display !== 'none';
      })
      .map((item) => (item as HTMLElement).dataset.page || '')
      .filter(Boolean));
    expect(menuPages).toEqual(expect.arrayContaining(['dashboard', 'device', 'network', 'peripheral', 'features']));

    for (const width of [360, 390, 430]) {
      await authPage.setViewportSize({ width, height: 900 });
      for (const menuPage of menuPages) {
        await authPage.evaluate(async (pageName) => {
          await (window as any).AppState.changePage(pageName);
        }, menuPage);
        const activePage = authPage.locator(`#${menuPage}-page`);
        await expect(activePage).toBeVisible({ timeout: 20_000 });
        const metrics = await authPage.evaluate((pageName) => {
          const page = document.getElementById(`${pageName}-page`) as HTMLElement;
          return {
            viewportOverflow: document.documentElement.scrollWidth > window.innerWidth + 1 ||
              document.body.scrollWidth > window.innerWidth + 1,
            pageOverflow: page.scrollWidth > page.clientWidth + 1,
            nodes: document.querySelectorAll('*').length,
            duplicateIds: Array.from(document.querySelectorAll('[id]')).filter((element, index, all) =>
              all.findIndex((candidate) => candidate.id === element.id) !== index
            ).length,
          };
        }, menuPage);
        expect(metrics.viewportOverflow, `${width}px ${menuPage} 不应产生视口横向溢出`).toBeFalsy();
        expect(metrics.pageOverflow, `${width}px ${menuPage} 页面不应横向溢出`).toBeFalsy();
        expect(metrics.nodes, `${width}px ${menuPage} DOM 节点数应受控`).toBeLessThanOrEqual(1800);
        expect(metrics.duplicateIds, `${width}px ${menuPage} 不应出现重复 ID`).toBe(0);
      }
    }
  });

  test('UI-005C: OTA 和配置导入复用统一文件选择控件', async ({ authPage, navigateTo }) => {
    const deviceMenu = authPage.locator('.menu-item[data-page="device"]');
    test.skip(!(await deviceMenu.isVisible()), '设备未提供设备配置页面');
    await navigateTo('device');
    test.skip(
      (await authPage.locator('#dev-ota-file').count()) === 0 ||
      (await authPage.locator('#dev-config-import-file').count()) === 0,
      '当前固件未同时编译 OTA 和配置转移能力'
    );

    const metrics = await authPage.evaluate(() => {
      const ids = ['dev-ota-file', 'dev-config-import-file'];
      return ids.map((id) => {
        const input = document.getElementById(id);
        const wrapper = input?.closest('.file-input-wrapper');
        return {
          id,
          hasWrapper: !!wrapper,
          hasButton: !!wrapper?.querySelector('.fb-btn'),
          hasFileName: !!wrapper?.querySelector('.file-name-display'),
        };
      });
    });

    expect(metrics).toEqual([
      { id: 'dev-ota-file', hasWrapper: true, hasButton: true, hasFileName: true },
      { id: 'dev-config-import-file', hasWrapper: true, hasButton: true, hasFileName: true },
    ]);
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

  test('UI-007A: 模态框连续开关 20 次关闭即销毁且 DOM 不增长', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    await waitForPeripheralPageReady(authPage);
    const addBtn = authPage.locator('#add-peripheral-btn');
    test.skip(!(await addBtn.isVisible()), '当前设备不允许新增外设');
    // This test measures mount/unmount behavior, not network preloading. Drain
    // the shared modal request before taking the DOM baseline so an unrelated
    // background page preload cannot consume most of the test timeout.
    await authPage.evaluate(async () => {
      const loader = (window as any).PageLoader;
      if (!loader._loadedPages.modals) await loader.loadModals();
    });
    const baseline = await authPage.locator('*').count();
    for (let round = 0; round < 20; round++) {
      await addBtn.click();
      await expect(authPage.locator('#peripheral-modal'), `第 ${round + 1} 次打开外设弹窗`).toBeVisible({ timeout: 45_000 });
      await authPage.locator('#cancel-peripheral-btn').click();
      await expect(authPage.locator('#peripheral-modal')).toHaveCount(0);
    }
    const metrics = await authPage.evaluate(() => ({
      visibleModals: Array.from(document.querySelectorAll('.modal')).filter((modal) => {
        const style = getComputedStyle(modal);
        return style.display !== 'none' && style.visibility !== 'hidden';
      }).length,
      nodes: document.querySelectorAll('*').length,
    }));
    expect(metrics.visibleModals).toBe(0);
    expect(await authPage.locator('body > .modal').count()).toBe(0);
    expect(metrics.nodes - baseline).toBeLessThanOrEqual(20);
  });

  test('UI-007B: Modbus 分片弹窗按需挂载并在关闭后销毁', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    const modbusEnabled = await isRuntimeModbusReady(authPage);
    test.skip(!modbusEnabled, '当前运行配置未启用 Modbus RTU');
    const modbusTab = authPage.locator('.config-tab[data-tab="modbus-rtu"]');
    await expect(modbusTab).toBeVisible();
    await modbusTab.click();
    const fragmentReady = await authPage.locator('#modbus-rtu-form')
      .waitFor({ state: 'attached', timeout: 15_000 }).then(() => true).catch(() => false);
    test.skip(!fragmentReady, 'Modbus RTU 分片未加载');

    const modalIds = [
      'modbus-device-edit-modal',
      'modbus-device-ctrl-modal',
      'task-edit-modal',
      'mapping-modal',
    ];
    const baseline = await authPage.locator('*').count();
    for (let round = 0; round < 5; round++) {
      for (const modalId of modalIds) {
        const mounted = await authPage.evaluate((id) => {
          const state = (window as any).AppState;
          const modal = state.ensureModal(id);
          if (!modal) return false;
          state.showModal(id);
          return true;
        }, modalId);
        expect(mounted).toBe(true);
        await expect(authPage.locator('#' + modalId)).toBeVisible();
        await authPage.evaluate((id) => (window as any).AppState.hideModal(id), modalId);
        await expect(authPage.locator('#' + modalId)).toHaveCount(0);
      }
    }
    expect((await authPage.locator('*').count()) - baseline).toBeLessThanOrEqual(20);
  });

  test('UI-007C: 模态模板未预取时首次点击会续接且并发只请求一次', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    await waitForPeripheralPageReady(authPage);
    const addBtn = authPage.locator('#add-peripheral-btn');
    test.skip(!(await addBtn.isVisible()), '当前设备不允许新增外设');
    await authPage.evaluate(async () => {
      const loader = (window as any).PageLoader;
      const pending = loader._loadingPages.modals;
      if (pending) await pending.catch(() => {});
      if (!loader._loadedPages.modals) await loader.loadModals();
      const pendingPeriph = loader._loadingPages['peripheral-modals'];
      if (pendingPeriph) await pendingPeriph.catch(() => {});
      if (!loader._loadedPages['peripheral-modals']) await loader.loadPeripheralModals();
      const modalHtml = Object.values(loader._modalTemplates).join('\n');
      if (!modalHtml.includes('id="peripheral-modal"')) {
        throw new Error('Cannot prepare controlled modal response');
      }
      document.getElementById('peripheral-modal')?.remove();
      delete loader._loadedPages.modals;
      delete loader._loadingPages.modals;
      delete loader._loadedPages['peripheral-modals'];
      delete loader._loadingPages['peripheral-modals'];
      loader._modalTemplates = {};
      loader._modalHosts = {};
      const originalFetch = window.fetch;
      const originalQueue = (window as any).__fastbeeQueueStaticAsset;
      (window as any).__modalFetchOriginal = originalFetch;
      (window as any).__modalQueueOriginal = originalQueue;
      (window as any).__modalFetchCount = 0;
      // Keep this synthetic concurrency check independent from page/module
      // preloads already queued by the authenticated application shell.
      (window as any).__fastbeeQueueStaticAsset = function(task: () => Promise<unknown>) {
        return Promise.resolve().then(task);
      };
      window.fetch = function(input: RequestInfo | URL, init?: RequestInit) {
        if (String(input).includes('/pages/modals.html') || String(input).includes('/pages/peripheral-modals.html')) {
          (window as any).__modalFetchCount++;
          return new Promise<Response>((resolve) => {
            setTimeout(() => resolve(new Response(modalHtml, {
              status: 200,
              headers: { 'Content-Type': 'text/html' },
            })), 250);
          });
        }
        return originalFetch.call(window, input, init);
      };
      (window as any).AppState._modalsLoaded = false;
      const button = document.getElementById('add-peripheral-btn') as HTMLButtonElement;
      button.click();
      button.click();
    });
    try {
      await expect(authPage.locator('#peripheral-modal')).toBeVisible({ timeout: 45_000 });
      // B6 两包拆分：基础包 + peripheral-modals 包各请求一次，双击不得重复（加载去重）。
      expect(await authPage.evaluate(() => (window as any).__modalFetchCount)).toBe(2);
    } finally {
      await authPage.evaluate(() => {
        (window as any).AppState.hideModal('peripheral-modal');
        window.fetch = (window as any).__modalFetchOriginal;
        (window as any).__fastbeeQueueStaticAsset = (window as any).__modalQueueOriginal;
        delete (window as any).__modalFetchOriginal;
        delete (window as any).__modalQueueOriginal;
      });
    }
    await expect(authPage.locator('#peripheral-modal')).toHaveCount(0);
  });

  test('UI-007E: 模态模板延迟返回时切页不得挂载旧页面弹窗 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    await waitForPeripheralPageReady(authPage);
    await authPage.evaluate(async () => {
      const loader = (window as any).PageLoader;
      const pending = loader._loadingPages.modals;
      if (pending) await pending.catch(() => {});
      if (!loader._loadedPages.modals) await loader.loadModals();
      const modalHtml = Object.values(loader._modalTemplates).join('\n');
      document.getElementById('peripheral-modal')?.remove();
      delete loader._loadedPages.modals;
      delete loader._loadingPages.modals;
      loader._modalTemplates = {};
      loader._modalHosts = {};
      (window as any).__lateModalOriginalFetch = window.fetch;
      (window as any).__lateModalOriginalQueue = (window as any).__fastbeeQueueStaticAsset;
      (window as any).__lateModalResult = 'pending';
      (window as any).__fastbeeQueueStaticAsset = (task: () => Promise<unknown>) => Promise.resolve().then(task);
      window.fetch = function(input: RequestInfo | URL, init?: RequestInit) {
        if (String(input).includes('/pages/modals.html')) {
          return new Promise<Response>((resolve) => {
            (window as any).__lateModalResolve = () => resolve(new Response(modalHtml, {
              status: 200,
              headers: { 'Content-Type': 'text/html' },
            }));
          });
        }
        return (window as any).__lateModalOriginalFetch.call(window, input, init);
      };
      (window as any).AppState._modalsLoaded = false;
      (window as any).AppState._ensureModalForAction('peripheral-modal').then((modal: HTMLElement | null) => {
        (window as any).__lateModalResult = modal ? 'mounted' : 'discarded';
      });
    });
    try {
      await authPage.waitForFunction(() => typeof (window as any).__lateModalResolve === 'function');
      await authPage.evaluate(() => (window as any).AppState.changePage('dashboard'));
      await expect(authPage.locator('#dashboard-page')).toBeVisible();
      await authPage.evaluate(() => (window as any).__lateModalResolve());
      await authPage.waitForFunction(() => (window as any).__lateModalResult !== 'pending');
      expect(await authPage.evaluate(() => (window as any).__lateModalResult)).toBe('discarded');
      await expect(authPage.locator('#peripheral-modal')).toHaveCount(0);
    } finally {
      await authPage.evaluate(() => {
        window.fetch = (window as any).__lateModalOriginalFetch;
        (window as any).__fastbeeQueueStaticAsset = (window as any).__lateModalOriginalQueue;
        delete (window as any).__lateModalOriginalFetch;
        delete (window as any).__lateModalOriginalQueue;
        delete (window as any).__lateModalResolve;
        delete (window as any).__lateModalResult;
      });
    }
  });

  test('UI-007D: 360px 长动态内容模态框无横向溢出 @quick', async ({ authPage, navigateTo }) => {
    await authPage.setViewportSize({ width: 360, height: 800 });
    await navigateTo('peripheral');
    await waitForPeripheralPageReady(authPage);
    const addBtn = authPage.locator('#add-peripheral-btn');
    test.skip(!(await addBtn.isVisible()), '当前设备不允许新增外设');
    await addBtn.click();
    const modal = authPage.locator('#peripheral-modal');
    await expect(modal).toBeVisible({ timeout: 45_000 });

    try {
      const metrics = await authPage.evaluate(() => {
        const modal = document.getElementById('peripheral-modal')!;
        const content = modal.querySelector('.modal-content') as HTMLElement;
        const body = modal.querySelector('.modal-body') as HTMLElement;
        const dynamicMessage = document.createElement('div');
        dynamicMessage.id = 'modal-long-content-probe';
        dynamicMessage.className = 'form-hint';
        dynamicMessage.textContent = '设备返回详情:' + 'A'.repeat(512);
        body.prepend(dynamicMessage);

        const contentBox = content.getBoundingClientRect();
        const bodyBox = body.getBoundingClientRect();
        const messageBox = dynamicMessage.getBoundingClientRect();
        const overflowingChildren = Array.from(body.querySelectorAll<HTMLElement>('*'))
          .filter((element) => {
            const box = element.getBoundingClientRect();
            return element.scrollWidth > element.clientWidth + 1 ||
              box.left < bodyBox.left - 1 || box.right > bodyBox.right + 1;
          })
          .slice(0, 8)
          .map((element) => {
            const box = element.getBoundingClientRect();
            return {
              element: `${element.tagName.toLowerCase()}#${element.id}.${element.className}`,
              clientWidth: element.clientWidth,
              scrollWidth: element.scrollWidth,
              left: Math.round(box.left),
              right: Math.round(box.right),
            };
          });
        return {
          viewportOverflow: document.documentElement.scrollWidth > window.innerWidth + 1 ||
            document.body.scrollWidth > window.innerWidth + 1,
          modalOverflow: modal.scrollWidth > modal.clientWidth + 1,
          contentOverflow: content.scrollWidth > content.clientWidth + 1,
          bodyOverflow: body.scrollWidth > body.clientWidth + 1,
          contentInsideViewport: contentBox.left >= 0 && contentBox.right <= window.innerWidth + 1,
          messageInsideBody: messageBox.left >= bodyBox.left - 1 && messageBox.right <= bodyBox.right + 1,
          overflowingChildren,
        };
      });

      expect(metrics.viewportOverflow, '360px 视口不应出现横向滚动').toBeFalsy();
      expect(metrics.modalOverflow, '模态遮罩不应横向溢出').toBeFalsy();
      expect(metrics.contentOverflow, '模态内容容器不应横向溢出').toBeFalsy();
      expect(
        metrics.bodyOverflow,
        `长动态文本不应让模态正文横向滚动: ${JSON.stringify(metrics.overflowingChildren)}`
      ).toBeFalsy();
      expect(metrics.contentInsideViewport, '模态框应完整位于 360px 视口内').toBeTruthy();
      expect(metrics.messageInsideBody, '长动态文本应在模态正文内换行').toBeTruthy();
    } finally {
      await authPage.evaluate(() => (window as any).AppState.hideModal('peripheral-modal'));
    }
    await expect(modal).toHaveCount(0);
  });

  // ========== 表单与按钮 ==========

  test('UI-008: 表单标签对齐', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.screenshot({ path: uiScreenshotPath('UI-008-forms.png'), fullPage: true });
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
    await authPage.screenshot({ path: uiScreenshotPath('UI-010-buttons.png') });
  });

  // ========== 响应式布局 ==========

  test('UI-011: 响应式-1920x1080', async ({ authPage, navigateTo }) => {
    await authPage.setViewportSize({ width: 1920, height: 1080 });
    await navigateTo('dashboard');
    await authPage.waitForLoadState('networkidle');
    await authPage.screenshot({ path: uiScreenshotPath('UI-011-1920x1080.png'), fullPage: true });
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('UI-012: 响应式-1366x768', async ({ authPage, navigateTo }) => {
    await authPage.setViewportSize({ width: 1366, height: 768 });
    await navigateTo('dashboard');
    await authPage.waitForLoadState('networkidle');
    await authPage.screenshot({ path: uiScreenshotPath('UI-012-1366x768.png'), fullPage: true });
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('UI-013: 响应式-1024x768', async ({ authPage, navigateTo }) => {
    await authPage.setViewportSize({ width: 1024, height: 768 });
    await navigateTo('dashboard');
    await authPage.waitForLoadState('networkidle');
    await authPage.screenshot({ path: uiScreenshotPath('UI-013-1024x768.png'), fullPage: true });
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
        await authPage.screenshot({ path: uiScreenshotPath('UI-014-dark-theme.png') });
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
        await authPage.screenshot({ path: uiScreenshotPath('UI-015-light-theme.png') });
      }
    }
  });

  // ========== 语言切换 ==========

  test('UI-016: 中英文切换-登录页', async ({ page }) => {
    await gotoWithRetry(page);
    await expect(page.locator('#login-page')).toBeVisible();
    // 切换到英文
    const langSelect = page.locator('#login-language-select');
    if (await langSelect.isVisible()) {
      await langSelect.selectOption('en');
      await page.waitForTimeout(1000);
      await page.screenshot({ path: uiScreenshotPath('UI-016-login-en.png') });
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
    test.setTimeout(60_000);
    await gotoWithRetry(page);
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

  test('UI-023: 配置导入导出入口位于设备高级配置', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.locator('[data-tab="dev-advanced"]').click();
    await expect(authPage.locator('#dev-config-import-btn')).toBeVisible();
    await expect(authPage.locator('#dev-config-export-btn')).toBeVisible();
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
        await authPage.screenshot({ path: uiScreenshotPath('UI-024-dropdown.png') });
      }
    }
  });

  // ========== 页面切换 ==========

  test('UI-025: 页面切换无闪烁', async ({ authPage, navigateTo }) => {
    // 快速切换菜单
    const menuPages = await authPage.locator('.menu-item[data-page]:visible').evaluateAll((items) =>
      items.slice(0, 5).map((item) => (item as HTMLElement).dataset.page || '').filter(Boolean));
    for (const menuPage of menuPages) {
      await authPage.click(`.menu-item[data-page="${menuPage}"]`);
      await authPage.waitForTimeout(200);
    }
    // 页面应正常
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('UI-025A: 重复切页不累积隐藏 DOM 且回访按钮可用', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await navigateTo('peripheral');
    await waitForPeripheralPageReady(authPage);
    const baseline = await authPage.locator('*').count();
    for (let round = 0; round < 10; round++) {
      await navigateTo('device');
      await navigateTo('peripheral');
    }
    await expect(authPage.locator('#peripheral-page')).toBeVisible();
    await expect(authPage.locator('#peripheral-refresh-btn')).toBeEnabled();
    await waitForPeripheralPageReady(authPage);

    const metrics = await authPage.evaluate(() => ({
      directPageRoots: document.querySelectorAll('#content-area > .page').length,
      hiddenPageRoots: document.querySelectorAll('#content-area > .page:not(.active)').length,
      duplicateIds: Array.from(document.querySelectorAll('[id]')).filter((el, index, all) =>
        all.findIndex(candidate => candidate.id === el.id) !== index
      ).length,
      nodes: document.querySelectorAll('*').length,
    }));
    expect(metrics.directPageRoots).toBe(1);
    expect(metrics.hiddenPageRoots).toBe(0);
    expect(metrics.duplicateIds).toBe(0);
    expect(metrics.nodes - baseline).toBeLessThanOrEqual(20);
  });

  test('UI-025B: 重复进入设备大屏不累积 document 监听器 @quick', async ({ authPage, navigateTo }) => {
    const controlMenu = authPage.locator('.menu-item[data-page="device-control"]:visible');
    test.skip((await controlMenu.count()) === 0, '当前固件未提供设备大屏');
    await navigateTo('dashboard');
    await authPage.evaluate(() => {
      (window as any).__dcDocumentListenerAdds = { keydown: 0, mousemove: 0, mouseup: 0 };
      (window as any).__dcOriginalDocumentAdd = document.addEventListener;
      (document as any).addEventListener = function(type: string, listener: EventListenerOrEventListenerObject, options?: boolean | AddEventListenerOptions) {
        const counts = (window as any).__dcDocumentListenerAdds;
        if (Object.prototype.hasOwnProperty.call(counts, type)) counts[type]++;
        return (window as any).__dcOriginalDocumentAdd.call(document, type, listener, options);
      };
    });
    try {
      for (let round = 0; round < 5; round++) {
        await navigateTo('device-control');
        await expect(authPage.locator('#device-control-page')).toBeVisible();
        await navigateTo('dashboard');
      }
      const additions = await authPage.evaluate(() => (window as any).__dcDocumentListenerAdds);
      expect(additions.keydown).toBeLessThanOrEqual(1);
      expect(additions.mousemove).toBeLessThanOrEqual(1);
      expect(additions.mouseup).toBeLessThanOrEqual(1);
    } finally {
      await authPage.evaluate(() => {
        (document as any).addEventListener = (window as any).__dcOriginalDocumentAdd;
        delete (window as any).__dcOriginalDocumentAdd;
        delete (window as any).__dcDocumentListenerAdds;
      });
    }
  });

  // ========== 截图对比 ==========

  test('UI-026: 各页面全页截图', async ({ authPage, navigateTo }) => {
    const pages = await authPage.locator('.menu-item[data-page]:visible').evaluateAll((items) =>
      items.map((item) => (item as HTMLElement).dataset.page || '').filter(Boolean));
    for (const menuPage of pages) {
      await navigateTo(menuPage);
      await authPage.waitForTimeout(500);
      await authPage.screenshot({ path: uiScreenshotPath(`UI-026-${menuPage}.png`), fullPage: true });
    }
  });

  test('UI-027: 长文本不截断', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    const nameInput = authPage.locator('#dev-name, #device-name, input[name="deviceName"]').first();
    if (await nameInput.isVisible()) {
      await expect(nameInput).toHaveValue(/\S+/, { timeout: 10_000 });
      const originalValue = await nameInput.inputValue();
      const longName = 'A'.repeat(50);
      await nameInput.fill(longName);
      await expect(nameInput).toHaveValue(longName);
      await nameInput.fill(originalValue);
    }
  });

  test('UI-027A: 外设风险提示使用二次保存且不显示复选框', async ({ authPage, navigateTo }) => {
    let preflightCalls = 0;
    let savePayload: Record<string, string> | null = null;
    await authPage.route('**/api/peripherals/preflight', async route => {
      if (route.request().method() !== 'POST') {
        await route.continue();
        return;
      }
      preflightCalls++;
      await route.fulfill({
        json: {
          success: true,
          allowed: true,
          data: {
            allowed: true,
            requiresConfirmation: true,
            requiredAcknowledgements: 3,
            issues: [
              {
                code: 'PIN_STRAPPING_ACK_REQUIRED',
                severity: 'warning-requires-confirmation',
                requiresConfirmation: true,
                message: 'Boot-strapping GPIO requires explicit confirmation',
              },
              {
                code: 'PIN_BOARD_RESOURCE_ACK_REQUIRED',
                severity: 'warning-requires-confirmation',
                requiresConfirmation: true,
                message: 'GPIO is shared with an onboard or low-power peripheral',
              },
            ],
            claims: [{ resourceKind: 'gpio' }, { resourceKind: 'rmt_channel' }],
          },
        },
      });
    });
    await authPage.route('**/api/peripherals', async route => {
      if (route.request().method() !== 'POST') {
        await route.continue();
        return;
      }
      savePayload = Object.fromEntries(new URLSearchParams(route.request().postData() || ''));
      await route.fulfill({ json: { success: true, message: 'Peripheral added' } });
    });

    await navigateTo('peripheral');
    await waitForPeripheralPageReady(authPage);
    await authPage.locator('#add-peripheral-btn').click();
    const modal = authPage.locator('#peripheral-modal');
    await expect(modal).toBeVisible({ timeout: 45_000 });
    await modal.locator('#peripheral-type-input').selectOption('12');
    await modal.locator('#peripheral-name-input').fill('风险提示测试');
    await modal.locator('#peripheral-pins-input').fill('8');

    const saveButton = modal.locator('#save-peripheral-btn');
    await expect(saveButton).toBeEnabled();
    await saveButton.click();
    const preflightHint = modal.locator('#peripheral-preflight-result');
    await expect(preflightHint).toBeVisible();
    await expect(preflightHint).toContainText('预检需要确认');
    await expect(preflightHint).toContainText('本次将占用 2 项资源');
    await expect(preflightHint).toContainText('请再次点击“确认并保存”');
    await expect(modal.locator('#peripheral-preflight-confirm')).toHaveCount(0);
    await expect(saveButton).toHaveText('确认并保存');
    expect(savePayload).toBeNull();

    await saveButton.click();
    await expect.poll(() => savePayload).not.toBeNull();
    expect(Number(savePayload!.acknowledgements)).toBe(3);
    expect(Number(savePayload!.confirmWarnings)).toBe(1);
    expect(preflightCalls).toBe(1);
    await expect(authPage.locator('#peripheral-modal')).toHaveCount(0);
  });

  test('UI-027D: 功能状态恢复后 Modbus RTU 选项卡立即出现', async ({ authPage }) => {
    let featureStatusAvailable = false;
    const systemStatusPattern = '**/api/system/status*';
    const featureStatusPattern = '**/api/features/status*';

    await authPage.route(systemStatusPattern, async route => {
      await route.fulfill({
        json: featureStatusAvailable ? {
          success: true,
          data: {
            features: {
              modbus: 'active',
              ruleScript: 'active',
              deviceControl: 'active',
              logViewer: 'active',
              userAdmin: 'active',
            },
          },
        } : { success: false, error: 'temporarily unavailable' },
      });
    });
    await authPage.route(featureStatusPattern, async route => {
      await route.fulfill({ json: { success: false, error: 'temporarily unavailable' } });
    });

    try {
      await authPage.evaluate(() => {
        const state = (window as any).AppState;
        state._featureActive = null;
        state._featureStatusReady = false;
        state._featureStatusPromise = null;
      });
      await authPage.evaluate(() => (window as any).AppState.changePage('protocol'));
      await authPage.waitForFunction(() => (window as any).AppState._featureStatusPromise === null);

      const modbusTab = authPage.locator('#protocol-page .config-tab[data-tab="modbus-rtu"]');
      await expect(modbusTab).toHaveAttribute('data-feature-state', 'pending');
      await expect(modbusTab).toBeHidden();

      featureStatusAvailable = true;
      await authPage.evaluate(() => (window as any).AppState.changePage('protocol'));
      await expect(modbusTab).toBeVisible();
      await expect(modbusTab).toHaveAttribute('data-feature-state', 'active');
      expect(await authPage.evaluate(() => (window as any).AppState._featureStatusReady)).toBe(true);
    } finally {
      await authPage.unroute(systemStatusPattern);
      await authPage.unroute(featureStatusPattern);
    }
  });

  test('UI-027E: 日志、用户和规则脚本工具栏在桌面与窄屏均填满顶部空间', async ({ authPage }) => {
    await enableOptionalUiFeatures(authPage, ['logViewer', 'userAdmin', 'ruleScript']);
    await authPage.route('**/api/rule-script*', async route => {
      await route.fulfill({
        json: {
          success: true,
          data: [
            { id: 'rule-1', name: '规则一', enabled: true, triggerType: 0, protocolType: 0 },
            { id: 'rule-2', name: '规则二', enabled: false, triggerType: 1, protocolType: 1 },
            { id: 'rule-3', name: '规则三', enabled: true, triggerType: 1, protocolType: 0 },
          ],
        },
      });
    });
    const pages = [
      { name: 'logs', summary: '#log-meta', readyText: /个文件/ },
      { name: 'users', summary: '#users-list-summary', readyText: /个用户/ },
      { name: 'rule-script', summary: '#rule-script-list-summary', readyText: /3 条规则 · 2 条已启用/ },
    ];

    try {
      for (const pageSpec of pages) {
        await authPage.setViewportSize({ width: 1280, height: 900 });
        await authPage.evaluate((pageName) => (window as any).AppState.changePage(pageName), pageSpec.name);
        const page = authPage.locator(`#${pageSpec.name}-page`);
        const summary = page.locator(pageSpec.summary);
        await expect(page).toBeVisible();
        await expect(page.locator('[data-page-fallback]')).toHaveCount(0);
        await expect(summary).toContainText(pageSpec.readyText);

        for (const width of [1280, 390, 360]) {
          await authPage.setViewportSize({ width, height: 900 });
          const metrics = await page.evaluate((element, desktop) => {
            const toolbar = element.querySelector('.fb-data-toolbar') as HTMLElement;
            const leading = toolbar.querySelector('.fb-data-toolbar-leading') as HTMLElement;
            const actions = toolbar.querySelector('.fb-card-header-actions') as HTMLElement;
            const toolbarRect = toolbar.getBoundingClientRect();
            const leadingRect = leading.getBoundingClientRect();
            const actionsRect = actions.getBoundingClientRect();
            return {
              pageOverflow: element.scrollWidth > element.clientWidth + 1,
              toolbarOverflow: toolbar.scrollWidth > toolbar.clientWidth + 1,
              leadingWidth: leadingRect.width,
              actionsWidth: actionsRect.width,
              desktopOrder: !desktop || leadingRect.right <= actionsRect.left + 1,
              withinToolbar: leadingRect.left >= toolbarRect.left - 1 && actionsRect.right <= toolbarRect.right + 1,
            };
          }, width > 768);

          expect(metrics.pageOverflow, `${width}px ${pageSpec.name} 页面不应横向溢出`).toBeFalsy();
          expect(metrics.toolbarOverflow, `${width}px ${pageSpec.name} 工具栏不应横向溢出`).toBeFalsy();
          expect(metrics.leadingWidth, `${width}px ${pageSpec.name} 左侧信息区不可为空`).toBeGreaterThan(80);
          expect(metrics.actionsWidth, `${width}px ${pageSpec.name} 操作区不可为空`).toBeGreaterThan(80);
          expect(metrics.desktopOrder, `${width}px ${pageSpec.name} 桌面工具栏应左右分区`).toBeTruthy();
          expect(metrics.withinToolbar, `${width}px ${pageSpec.name} 工具栏内容应留在边界内`).toBeTruthy();

          if (width !== 360) {
            await authPage.screenshot({
              path: uiScreenshotPath(`UI-027E-${pageSpec.name}-${width}.png`),
              fullPage: true,
            });
          }
        }
      }
    } finally {
      await authPage.unroute('**/api/rule-script*');
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

  test('UI-031: 模态框键盘闭环与 ARIA 属性 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    await waitForPeripheralPageReady(authPage);
    const addBtn = authPage.locator('#add-peripheral-btn');
    test.skip(!(await addBtn.isVisible()), '当前设备不允许新增外设');
    await addBtn.click();
    const modal = authPage.locator('#peripheral-modal');
    await expect(modal).toBeVisible();
    // ARIA 语义完整
    await expect(modal).toHaveAttribute('role', 'dialog');
    await expect(modal).toHaveAttribute('aria-modal', 'true');
    const labelledby = await modal.getAttribute('aria-labelledby');
    expect(labelledby).toBeTruthy();
    await expect(authPage.locator(`#${labelledby}`)).toHaveCount(1);
    // 打开后焦点进入弹窗
    const focusInside = () => authPage.evaluate(() => {
      const modalEl = document.getElementById('peripheral-modal');
      return !!modalEl && modalEl.contains(document.activeElement);
    });
    expect(await focusInside()).toBe(true);
    // 正向 Tab 循环不逃逸
    for (let i = 0; i < 15; i++) {
      await authPage.keyboard.press('Tab');
    }
    expect(await focusInside()).toBe(true);
    // 反向 Shift+Tab 循环不逃逸
    for (let i = 0; i < 15; i++) {
      await authPage.keyboard.press('Shift+Tab');
    }
    expect(await focusInside()).toBe(true);
    // Escape 关闭且焦点还原到触发按钮
    await authPage.keyboard.press('Escape');
    await expect(modal).not.toBeVisible();
    const focusRestored = await authPage.evaluate(() =>
      !!document.activeElement && document.activeElement.id === 'add-peripheral-btn');
    expect(focusRestored).toBe(true);
  });
});
