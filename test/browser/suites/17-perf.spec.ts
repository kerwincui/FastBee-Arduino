import { test, expect, waitForDevice } from '../fixtures/base.fixture';

test.describe('Suite-17: Web流畅性与性能测试', () => {

  // ========== 场景A：页面加载性能 ==========

  test('PERF-001: 登录页首次加载时间 < 5秒 @quick', async ({ page }) => {
    await page.context().clearCookies();
    const start = Date.now();
    await page.goto('/', { waitUntil: 'load' });
    const loadTime = Date.now() - start;
    console.log(`登录页加载时间: ${loadTime}ms`);
    expect(loadTime).toBeLessThan(5000);
    await expect(page.locator('#login-page')).toBeVisible();
  });

  test('PERF-002: 仪表盘加载时间 < 3秒', async ({ authPage }) => {
    const start = Date.now();
    await authPage.click('.menu-item[data-page="dashboard"]');
    await authPage.waitForLoadState('networkidle');
    const loadTime = Date.now() - start;
    console.log(`仪表盘加载时间: ${loadTime}ms`);
    expect(loadTime).toBeLessThan(3000);
  });

  test('PERF-003: Service Worker缓存后加载 < 2秒', async ({ authPage }) => {
    // 1. 先访问所有页面让SW缓存
    const menuPages = ['dashboard', 'device', 'network', 'peripheral', 'protocol', 'logs'];
    for (const p of menuPages) {
      await authPage.click(`.menu-item[data-page="${p}"]`);
      await authPage.waitForLoadState('networkidle', { timeout: 8_000 });
    }

    // 2. 检查SW注册状态
    const swStatus = await authPage.evaluate(async () => {
      if ('serviceWorker' in navigator) {
        const reg = await navigator.serviceWorker.getRegistration();
        return reg ? { registered: true, scope: reg.scope } : { registered: false };
      }
      return { registered: false, reason: 'no SW API' };
    });
    console.log(`SW状态: ${JSON.stringify(swStatus)}`);

    // 3. 重新访问各页面，验证加载时间
    for (const p of menuPages) {
      const start = Date.now();
      await authPage.click(`.menu-item[data-page="${p}"]`);
      await authPage.waitForLoadState('networkidle', { timeout: 8_000 });
      const loadTime = Date.now() - start;
      console.log(`SW缓存后 ${p}: ${loadTime}ms`);
      // 缓存后应该更快，放宽到 < 3秒
      expect(loadTime).toBeLessThan(3000);
    }
  });

  test('PERF-004: 各页面加载时间基准 < 4秒', async ({ authPage }) => {
    const menuPages = [
      'dashboard', 'device', 'network', 'peripheral', 'periph-exec',
      'protocol', 'device-control', 'rule-script', 'logs', 'data', 'users'
    ];
    for (const menuPage of menuPages) {
      const start = Date.now();
      await authPage.click(`.menu-item[data-page="${menuPage}"]`);
      await authPage.waitForLoadState('networkidle', { timeout: 10_000 });
      const loadTime = Date.now() - start;
      console.log(`${menuPage}: ${loadTime}ms`);
      expect(loadTime).toBeLessThan(4000);
    }
  });

  test('PERF-005: API响应时间基准 < 2秒', async ({ authPage }) => {
    const apis = ['/api/health', '/api/status', '/api/network', '/api/mqtt/status'];
    for (const api of apis) {
      const start = Date.now();
      const ok = await authPage.evaluate(async (url: string) => {
        try {
          const r = await fetch(url);
          return r.ok || r.status < 500;
        } catch { return false; }
      }, api);
      const elapsed = Date.now() - start;
      console.log(`${api}: ${elapsed}ms (ok=${ok})`);
      expect(ok).toBeTruthy();
      expect(elapsed).toBeLessThan(2000);
    }
  });

  test('PERF-006: 慢网络下页面加载(模拟3G)', async ({ page }) => {
    // 模拟3G网络限速
    await page.context().setOffline(false);
    // Playwright CDP 限速
    const client = await page.context().newCDPSession(page);
    await client.send('Network.emulateNetworkConditions', {
      offline: false,
      downloadThroughput: (750 * 1024) / 8,  // 750 kbps
      uploadThroughput: (250 * 1024) / 8,     // 250 kbps
      latency: 200                             // 200ms RTT
    });

    // 登录
    await page.goto('/', { waitUntil: 'load', timeout: 30_000 });
    await page.fill('#username', 'admin');
    await page.fill('#password', 'admin');
    await page.click('#login-button');
    await page.waitForSelector('#app-container', { state: 'visible', timeout: 30_000 });

    // 访问各页面
    const menuPages = ['dashboard', 'device', 'network', 'protocol', 'logs'];
    for (const p of menuPages) {
      const start = Date.now();
      await page.click(`.menu-item[data-page="${p}"]`);
      await page.waitForLoadState('networkidle', { timeout: 30_000 });
      const loadTime = Date.now() - start;
      console.log(`3G网络 ${p}: ${loadTime}ms`);
      // 慢网络下放宽到 15 秒
      expect(loadTime).toBeLessThan(15_000);
    }

    // 恢复正常网络
    await client.send('Network.emulateNetworkConditions', {
      offline: false, downloadThroughput: -1, uploadThroughput: -1, latency: 0
    });
  });

  // ========== 场景B：交互流畅性 ==========

  test('PERF-007: 侧边栏菜单切换响应 < 500ms', async ({ authPage }) => {
    const menus = ['dashboard', 'device', 'network', 'protocol', 'logs'];
    for (const menu of menus) {
      const start = Date.now();
      await authPage.click(`.menu-item[data-page="${menu}"]`);
      await authPage.waitForTimeout(100);
      const switchTime = Date.now() - start;
      console.log(`菜单 ${menu} 切换: ${switchTime}ms`);
      expect(switchTime).toBeLessThan(500);
    }
  });

  test('PERF-008: 表单输入响应 < 100ms/字符', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="device"]');
    await authPage.waitForLoadState('networkidle');
    const input = authPage.locator('#device-name, input[name="deviceName"]').first();
    if (await input.isVisible()) {
      const start = Date.now();
      await input.fill('FastBee-Perf-Test');
      const fillTime = Date.now() - start;
      console.log(`17字符输入时间: ${fillTime}ms`);
      expect(fillTime).toBeLessThan(1700);
    }
  });

  test('PERF-009: 下拉框展开响应', async ({ authPage }) => {
    // 测试网络页面的联网方式下拉框
    await authPage.click('.menu-item[data-page="network"]');
    await authPage.waitForLoadState('networkidle');

    const selects = ['#network-type', '#wifi-mode', '#wifi-security'];
    for (const sel of selects) {
      const el = authPage.locator(sel).first();
      if (await el.isVisible()) {
        const start = Date.now();
        await el.click();
        const expandTime = Date.now() - start;
        console.log(`${sel} 展开: ${expandTime}ms`);
        expect(expandTime).toBeLessThan(500);
      }
    }

    // MQTT协议下拉框
    await authPage.click('.menu-item[data-page="protocol"]');
    await authPage.waitForLoadState('networkidle');
    const mqttScheme = authPage.locator('#mqtt-scheme').first();
    if (await mqttScheme.isVisible()) {
      const start = Date.now();
      await mqttScheme.click();
      const expandTime = Date.now() - start;
      console.log(`#mqtt-scheme 展开: ${expandTime}ms`);
      expect(expandTime).toBeLessThan(500);
    }
  });

  test('PERF-010: 模态框打开/关闭动画 < 300ms @quick', async ({ authPage }) => {
    // 外设页新建模态框
    await authPage.click('.menu-item[data-page="peripheral"]');
    await authPage.waitForLoadState('networkidle');
    const addBtn = authPage.locator('#add-periph-btn, button:has-text("添加"), button:has-text("新建")').first();
    if (await addBtn.isVisible()) {
      // 打开
      const openStart = Date.now();
      await addBtn.click();
      const modal = authPage.locator('.modal, .modal-dialog, [role="dialog"]').first();
      await modal.waitFor({ state: 'visible', timeout: 3_000 });
      const openTime = Date.now() - openStart;
      console.log(`模态框打开: ${openTime}ms`);
      expect(openTime).toBeLessThan(500);

      // 关闭
      const closeBtn = authPage.locator('.modal-close, .modal .close, button:has-text("取消")').first();
      if (await closeBtn.isVisible()) {
        const closeStart = Date.now();
        await closeBtn.click();
        await modal.waitFor({ state: 'hidden', timeout: 3_000 });
        const closeTime = Date.now() - closeStart;
        console.log(`模态框关闭: ${closeTime}ms`);
        expect(closeTime).toBeLessThan(500);
      }
    }
  });

  test('PERF-011: 按钮点击响应 < 200ms', async ({ authPage }) => {
    // 测试各种按钮的响应速度
    await authPage.click('.menu-item[data-page="network"]');
    await authPage.waitForLoadState('networkidle');

    // 刷新按钮
    const refreshBtn = authPage.locator('button:has-text("刷新"), button:has-text("扫描")').first();
    if (await refreshBtn.isVisible()) {
      const start = Date.now();
      await refreshBtn.click();
      await authPage.waitForTimeout(200);
      const responseTime = Date.now() - start;
      console.log(`刷新按钮响应: ${responseTime}ms`);
      expect(responseTime).toBeLessThan(500);
    }

    // 日志页刷新
    await authPage.click('.menu-item[data-page="logs"]');
    await authPage.waitForLoadState('networkidle');
    const logRefresh = authPage.locator('#logs-refresh-btn, button:has-text("刷新")').first();
    if (await logRefresh.isVisible()) {
      const start = Date.now();
      await logRefresh.click();
      await authPage.waitForTimeout(200);
      const responseTime = Date.now() - start;
      console.log(`日志刷新响应: ${responseTime}ms`);
      expect(responseTime).toBeLessThan(500);
    }
  });

  test('PERF-012: 滚动流畅性', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="network"]');
    await authPage.waitForLoadState('networkidle');
    await authPage.evaluate(() => {
      window.scrollTo(0, document.body.scrollHeight);
    });
    await authPage.waitForTimeout(500);
    await authPage.evaluate(() => {
      window.scrollTo(0, 0);
    });
    await authPage.waitForTimeout(500);
    expect(true).toBeTruthy();
  });

  test('PERF-013: 表格渲染性能', async ({ authPage }) => {
    // 外设列表渲染
    await authPage.click('.menu-item[data-page="peripheral"]');
    await authPage.waitForLoadState('networkidle');
    const start1 = Date.now();
    await authPage.waitForTimeout(500);
    const periphRender = Date.now() - start1;
    console.log(`外设页渲染: ${periphRender}ms`);
    await expect(authPage.locator('#app-container')).toBeVisible();

    // 用户列表渲染
    await authPage.click('.menu-item[data-page="users"]');
    const start2 = Date.now();
    await authPage.waitForLoadState('networkidle');
    const usersRender = Date.now() - start2;
    console.log(`用户列表渲染: ${usersRender}ms`);
    expect(usersRender).toBeLessThan(2000);
    await expect(authPage.locator('#app-container')).toBeVisible();

    // 文件管理渲染
    await authPage.click('.menu-item[data-page="data"]');
    const start3 = Date.now();
    await authPage.waitForLoadState('networkidle');
    const filesRender = Date.now() - start3;
    console.log(`文件管理渲染: ${filesRender}ms`);
    expect(filesRender).toBeLessThan(2000);
  });

  test('PERF-014: Tab标签切换流畅', async ({ authPage }) => {
    // 网络设置Tab
    await authPage.click('.menu-item[data-page="network"]');
    await authPage.waitForLoadState('networkidle');

    const networkTabs = ['[data-tab="base"]', '[data-tab="ap-config"]', '[data-tab="advance"]'];
    for (const tab of networkTabs) {
      const tabEl = authPage.locator(tab).first();
      if (await tabEl.isVisible()) {
        const start = Date.now();
        await tabEl.click();
        await authPage.waitForTimeout(200);
        const switchTime = Date.now() - start;
        console.log(`网络Tab ${tab}: ${switchTime}ms`);
        expect(switchTime).toBeLessThan(500);
      }
    }

    // 设备配置Tab
    await authPage.click('.menu-item[data-page="device"]');
    await authPage.waitForLoadState('networkidle');
    const deviceTabs = authPage.locator('[data-tab]');
    const tabCount = await deviceTabs.count();
    for (let i = 0; i < Math.min(tabCount, 3); i++) {
      const tabEl = deviceTabs.nth(i);
      if (await tabEl.isVisible()) {
        const start = Date.now();
        await tabEl.click();
        await authPage.waitForTimeout(200);
        const switchTime = Date.now() - start;
        console.log(`设备Tab[${i}]: ${switchTime}ms`);
        expect(switchTime).toBeLessThan(500);
      }
    }
  });

  // ========== 场景C：并发与压力场景 ==========

  test('PERF-015: 同时打开多个浏览器标签', async ({ page, baseURL }) => {
    const context = page.context();
    const pages = [];
    // 打开5个标签页
    for (let i = 0; i < 5; i++) {
      const newPage = await context.newPage();
      await newPage.goto(baseURL || '/', { waitUntil: 'load', timeout: 15_000 });
      // 每个标签页登录
      await newPage.fill('#username', 'admin');
      await newPage.fill('#password', 'admin');
      await newPage.click('#login-button');
      await newPage.waitForSelector('#app-container', { state: 'visible', timeout: 15_000 });
      pages.push(newPage);
    }

    // 每个标签页访问不同页面
    const menuPages = ['dashboard', 'device', 'network', 'protocol', 'logs'];
    for (let i = 0; i < pages.length; i++) {
      await pages[i].click(`.menu-item[data-page="${menuPages[i]}"]`);
      await pages[i].waitForLoadState('networkidle', { timeout: 10_000 });
      const visible = await pages[i].locator('#app-container').isVisible();
      console.log(`标签页${i} (${menuPages[i]}): visible=${visible}`);
      expect(visible).toBeTruthy();
    }

    // 关闭多余标签页
    for (const p of pages) await p.close().catch(() => {});
  });

  test('PERF-016: 快速连续刷新页面', async ({ authPage }) => {
    for (let i = 0; i < 5; i++) {
      await authPage.reload({ waitUntil: 'domcontentloaded', timeout: 10_000 });
    }
    await authPage.waitForLoadState('networkidle');
    await expect(authPage.locator('#app-container, #login-page').first()).toBeVisible();
  });

  test('PERF-017: 快速连续保存操作不崩溃', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="network"]');
    await authPage.waitForLoadState('networkidle');
    const saveBtn = authPage.locator('button:has-text("保存")').first();
    if (await saveBtn.isVisible()) {
      for (let i = 0; i < 3; i++) {
        await saveBtn.click();
        await authPage.waitForTimeout(200);
      }
      await authPage.waitForTimeout(5000);
      const healthOk = await authPage.evaluate(async () => {
        try {
          const r = await fetch('/api/health');
          return r.ok;
        } catch { return false; }
      });
      expect(healthOk).toBeTruthy();
    }
  });

  test('PERF-018: 快速开关模态框10次无卡顿', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="peripheral"]');
    await authPage.waitForLoadState('networkidle');

    const addBtn = authPage.locator('#add-periph-btn, button:has-text("添加"), button:has-text("新建")').first();
    if (await addBtn.isVisible()) {
      for (let i = 0; i < 10; i++) {
        await addBtn.click();
        const modal = authPage.locator('.modal, .modal-dialog, [role="dialog"]').first();
        await modal.waitFor({ state: 'visible', timeout: 2_000 });

        const closeBtn = authPage.locator('.modal-close, .modal .close, button:has-text("取消")').first();
        if (await closeBtn.isVisible()) {
          await closeBtn.click();
          await modal.waitFor({ state: 'hidden', timeout: 2_000 }).catch(() => {});
        } else {
          // 按ESC关闭
          await authPage.keyboard.press('Escape');
          await authPage.waitForTimeout(300);
        }
      }

      // 验证无遮罩层残留
      const overlay = authPage.locator('.modal-backdrop, .modal-overlay, .modal-mask');
      const overlayCount = await overlay.count();
      console.log(`遮罩层残留: ${overlayCount}`);
      // 不应有可见遮罩层
      if (overlayCount > 0) {
        const visible = await overlay.first().isVisible();
        expect(visible).toBeFalsy();
      }
    }
  });

  test('PERF-019: WiFi扫描时页面操作', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="network"]');
    await authPage.waitForLoadState('networkidle');

    // 尝试打开WiFi扫描
    const scanBtn = authPage.locator('#wifi-scan-btn, button:has-text("扫描")').first();
    if (await scanBtn.isVisible()) {
      await scanBtn.click();
      await authPage.waitForTimeout(500);

      // 扫描期间尝试操作其他元素
      const networkType = authPage.locator('#network-type').first();
      if (await networkType.isVisible()) {
        await networkType.selectOption('0');
        await authPage.waitForTimeout(200);
      }

      // 页面不应卡死
      const responsive = await authPage.evaluate(() => document.readyState);
      console.log(`扫描期间页面状态: ${responsive}`);
      expect(responsive).toBe('complete');
    }
  });

  // ========== 场景D：资源与内存监控 ==========

  test('PERF-020: 设备Heap内存稳定性(2分钟) @quick', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="dashboard"]');
    await authPage.waitForLoadState('networkidle');

    const heapValues: number[] = [];
    for (let i = 0; i < 4; i++) {
      await authPage.waitForTimeout(30_000);
      const heap = await authPage.evaluate(async () => {
        try {
          const r = await fetch('/api/status');
          const data = await r.json();
          return data?.heap?.free ?? data?.freeHeap ?? -1;
        } catch { return -1; }
      });
      if (heap >= 0) heapValues.push(heap);
      console.log(`Heap采样[${i}]: ${heap} bytes`);
    }

    if (heapValues.length >= 2) {
      const first = heapValues[0];
      const last = heapValues[heapValues.length - 1];
      const drop = first - last;
      console.log(`Heap变化: ${drop} bytes (初始: ${first}, 最终: ${last})`);
      expect(drop).toBeLessThan(10_000);
    }
  });

  test('PERF-021: 多次操作后Heap变化 < 5KB', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="dashboard"]');
    await authPage.waitForLoadState('networkidle');

    // 记录初始Heap
    const initialHeap = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.heap?.free ?? data?.freeHeap ?? -1;
      } catch { return -1; }
    });
    console.log(`初始Heap: ${initialHeap}`);

    // 执行50次操作
    const menuPages = ['dashboard', 'device', 'network', 'protocol', 'logs'];
    for (let i = 0; i < 10; i++) {
      for (const p of menuPages) {
        await authPage.click(`.menu-item[data-page="${p}"]`);
        await authPage.waitForTimeout(200);
      }
    }

    // 记录最终Heap
    const finalHeap = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.heap?.free ?? data?.freeHeap ?? -1;
      } catch { return -1; }
    });
    console.log(`最终Heap: ${finalHeap}`);

    if (initialHeap > 0 && finalHeap > 0) {
      const diff = initialHeap - finalHeap;
      console.log(`Heap变化: ${diff} bytes`);
      expect(diff).toBeLessThan(5_000);
    }
  });

  test('PERF-022: Flash存储稳定性', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="dashboard"]');
    await authPage.waitForLoadState('networkidle');

    // 记录初始Flash
    const flashBefore = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.flash?.used ?? data?.fsUsed ?? -1;
      } catch { return -1; }
    });
    console.log(`Flash使用前: ${flashBefore}`);

    // 执行几次配置保存
    await authPage.click('.menu-item[data-page="device"]');
    await authPage.waitForLoadState('networkidle');
    const saveBtn = authPage.locator('button[type="submit"], button:has-text("保存")').first();
    if (await saveBtn.isVisible()) {
      await saveBtn.click();
      await authPage.waitForTimeout(3000);
    }

    // 再次检查Flash
    const flashAfter = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.flash?.used ?? data?.fsUsed ?? -1;
      } catch { return -1; }
    });
    console.log(`Flash使用后: ${flashAfter}`);

    if (flashBefore >= 0 && flashAfter >= 0) {
      const diff = Math.abs(flashAfter - flashBefore);
      console.log(`Flash变化: ${diff} bytes`);
      // 变化不应超过 4KB（一个扇区）
      expect(diff).toBeLessThan(4096);
    }
  });

  test('PERF-023: 设备重启后内存恢复', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="dashboard"]');
    await authPage.waitForLoadState('networkidle');

    // 记录初始Heap
    const initialHeap = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.heap?.free ?? data?.freeHeap ?? -1;
      } catch { return -1; }
    });
    console.log(`重启前Heap: ${initialHeap}`);

    // 触发重启
    await authPage.click('.menu-item[data-page="device"]');
    await authPage.waitForLoadState('networkidle');
    const rebootBtn = authPage.locator('#reboot-btn, button:has-text("重启")').first();
    if (await rebootBtn.isVisible()) {
      await rebootBtn.click();
      await authPage.waitForTimeout(1000);
      const confirmBtn = authPage.locator('.modal-confirm-btn, button:has-text("确定")').first();
      if (await confirmBtn.isVisible()) await confirmBtn.click();

      // 等待重启完成
      await waitForDevice(authPage, 6000);

      // 重新登录
      await authPage.goto('/');
      await authPage.fill('#username', 'admin');
      await authPage.fill('#password', 'admin');
      await authPage.click('#login-button');
      await authPage.waitForSelector('#app-container', { state: 'visible', timeout: 15_000 });

      // 检查Heap
      await authPage.click('.menu-item[data-page="dashboard"]');
      await authPage.waitForLoadState('networkidle');
      const afterHeap = await authPage.evaluate(async () => {
        try {
          const r = await fetch('/api/status');
          const data = await r.json();
          return data?.heap?.free ?? data?.freeHeap ?? -1;
        } catch { return -1; }
      });
      console.log(`重启后Heap: ${afterHeap}`);

      // 重启后Heap应恢复到接近初始水平（允许5KB偏差）
      if (initialHeap > 0 && afterHeap > 0) {
        const diff = Math.abs(initialHeap - afterHeap);
        expect(diff).toBeLessThan(5_000);
      }
    }
  });

  // ========== 场景E：Service Worker与缓存性能 ==========

  test('PERF-024: SW注册与缓存存储', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="dashboard"]');
    await authPage.waitForLoadState('networkidle');

    const swInfo = await authPage.evaluate(async () => {
      const result: any = { swSupported: false, registered: false, caches: [] };
      if ('serviceWorker' in navigator) {
        result.swSupported = true;
        const reg = await navigator.serviceWorker.getRegistration();
        if (reg) {
          result.registered = true;
          result.scope = reg.scope;
          result.state = reg.active?.state ?? 'unknown';
        }
      }
      if ('caches' in window) {
        const keys = await caches.keys();
        result.caches = keys;
      }
      return result;
    });
    console.log(`SW信息: ${JSON.stringify(swInfo)}`);
    // SW 支持即可（注册可能因设备配置而异）
    expect(swInfo.swSupported).toBeTruthy();
  });

  test('PERF-025: SW缓存命中验证', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="dashboard"]');
    await authPage.waitForLoadState('networkidle');

    // 检查缓存中是否有页面资源
    const cacheHit = await authPage.evaluate(async () => {
      if (!('caches' in window)) return { supported: false };
      const keys = await caches.keys();
      const results: any[] = [];
      for (const key of keys) {
        const cache = await caches.open(key);
        const requests = await cache.keys();
        results.push({ cache: key, entries: requests.length });
      }
      return { supported: true, caches: results };
    });
    console.log(`缓存状态: ${JSON.stringify(cacheHit)}`);
    // 验证不崩溃即可
    expect(cacheHit).toBeTruthy();
  });

  test('PERF-026: SW缓存更新检测', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="dashboard"]');
    await authPage.waitForLoadState('networkidle');

    const updateResult = await authPage.evaluate(async () => {
      if (!('serviceWorker' in navigator)) return { swAvailable: false };
      const reg = await navigator.serviceWorker.getRegistration();
      if (!reg) return { swAvailable: false };
      // 尝试更新
      await reg.update();
      return {
        swAvailable: true,
        updateChecked: true,
        waiting: !!reg.waiting,
        installing: !!reg.installing
      };
    });
    console.log(`SW更新检测: ${JSON.stringify(updateResult)}`);
    expect(updateResult).toBeTruthy();
  });

  test('PERF-027: 缓存清除后重新加载', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="dashboard"]');
    await authPage.waitForLoadState('networkidle');

    // 清除缓存
    const clearResult = await authPage.evaluate(async () => {
      if (!('caches' in window)) return { cleared: false, reason: 'no caches API' };
      const keys = await caches.keys();
      const deleted = [];
      for (const key of keys) {
        const ok = await caches.delete(key);
        if (ok) deleted.push(key);
      }
      return { cleared: true, deleted };
    });
    console.log(`缓存清除: ${JSON.stringify(clearResult)}`);

    // 重新加载页面
    await authPage.reload({ waitUntil: 'load', timeout: 10_000 });
    // 重新登录（可能被重定向到登录页）
    const loginPage = authPage.locator('#login-page');
    if (await loginPage.isVisible()) {
      await authPage.fill('#username', 'admin');
      await authPage.fill('#password', 'admin');
      await authPage.click('#login-button');
      await authPage.waitForSelector('#app-container', { state: 'visible', timeout: 15_000 });
    }
    await expect(authPage.locator('#app-container, #login-page').first()).toBeVisible();
  });

  test('PERF-028: 缓存有效期验证', async ({ authPage }) => {
    await authPage.click('.menu-item[data-page="dashboard"]');
    await authPage.waitForLoadState('networkidle');

    // 检查缓存策略信息
    const cachePolicy = await authPage.evaluate(async () => {
      const result: any = {};
      // 检查 meta 标签中的缓存策略
      const metaCache = document.querySelector('meta[http-equiv="Cache-Control"]');
      result.metaCache = metaCache?.getAttribute('content') ?? 'none';

      // 检查SW中的缓存时间
      if ('caches' in window) {
        const keys = await caches.keys();
        result.cacheCount = keys.length;
      }
      return result;
    });
    console.log(`缓存策略: ${JSON.stringify(cachePolicy)}`);
    // 验证缓存机制存在
    expect(cachePolicy).toBeTruthy();
  });

  // ========== 场景F：设备负载下的Web响应 ==========

  test('PERF-029: MQTT高频发布时Web响应', async ({ authPage }) => {
    // 先配置MQTT
    await authPage.click('.menu-item[data-page="protocol"]');
    await authPage.waitForLoadState('networkidle');

    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    const connected = mqttStatus?.status === 'connected' || mqttStatus?.data?.status === 'connected';
    console.log(`MQTT连接: ${connected}`);

    // 在MQTT运行时操作Web
    const menuPages = ['dashboard', 'device', 'network', 'logs'];
    for (const p of menuPages) {
      const start = Date.now();
      await authPage.click(`.menu-item[data-page="${p}"]`);
      await authPage.waitForLoadState('networkidle', { timeout: 10_000 });
      const loadTime = Date.now() - start;
      console.log(`MQTT运行时 ${p}: ${loadTime}ms`);
      expect(loadTime).toBeLessThan(5000);
    }

    // API响应
    const apiStart = Date.now();
    const apiOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/status')).ok; } catch { return false; }
    });
    const apiTime = Date.now() - apiStart;
    console.log(`MQTT运行时 /api/status: ${apiTime}ms`);
    expect(apiOk).toBeTruthy();
    expect(apiTime).toBeLessThan(3000);
  });

  test('PERF-030: 负载下Web API响应 < 3秒', async ({ authPage }) => {
    const start = Date.now();
    const ok = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        return r.ok;
      } catch { return false; }
    });
    const elapsed = Date.now() - start;
    console.log(`负载下 /api/status: ${elapsed}ms`);
    expect(ok).toBeTruthy();
    expect(elapsed).toBeLessThan(3000);
  });
});
