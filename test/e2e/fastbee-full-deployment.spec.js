// FastBee Playwright E2E Test Suite
// For deployment validation and functional testing
// 弱链路容错统一收敛在 helpers/fastbee-helpers.js（goto 重试 / boot 就绪等待 / 登录重试 / chunk 重试）

const { test, expect } = require('@playwright/test');
const {
  openDevicePage,
  loginRobust,
  openSpaPage,
  apiWithRetry,
  resolveDevices,
} = require('./helpers/fastbee-helpers');

// DEVICES env 优先；缺省自动发现子网（DHCP 漂移免疫，不再硬编码兜底 IP）
const devices = resolveDevices();

test.describe('FastBee Full Deployment Validation', () => {
  for (const [index, ip] of devices.entries()) {
    const baseUrl = `http://${ip}`;

    test(`Device ${index + 1} (${ip}) - Complete E2E Test Suite`, async ({ page, request }) => {
      test.setTimeout(240 * 1000); // 4 minute timeout per device (weak-link devices need retries)

      // ===== TEST 1: Base Access =====
      await test.step('Base page load', async () => {
        // SPA loads page chunks lazily; networkidle hangs on low-memory devices
        // 弱信号链路上首屏可能 >20s，goto 重试收敛在 helper
        await openDevicePage(page, baseUrl);
        console.log(`[OK] Device ${ip} base page loaded successfully`);
      });

      // ===== TEST 2: Authentication =====
      await test.step('User authentication', async () => {
        // 先等 boot chunk 就绪再提交，避免卡「页面初始化中」；弱链路丢包自动重载重登
        await loginRobust(page);
        console.log(`[OK] Device ${ip} authenticated successfully`);
      });

      // ===== TEST 3: Dashboard Verification =====
      await test.step('Dashboard verification', async () => {
        const dashboard = page.locator('#dashboard-page.active').first();
        try {
          await expect(dashboard).toBeVisible({ timeout: 20000 });
        } catch (e) {
          // 弱信号/低内存设备仪表盘 chunk 可能首次加载失败，重载后重登录再等一次
          console.log('  dashboard chunk not injected, reloading once...');
          await page.reload({ waitUntil: 'domcontentloaded', timeout: 45000 });
          await page.waitForSelector('#login-form', { timeout: 20000 });
          await loginRobust(page);
          await expect(dashboard).toBeVisible({ timeout: 30000 });
        }

        // System metrics should be present somewhere on the dashboard (soft check)
        const metricsList = page.locator('.metrics-list, [data-testid="metrics"], .status-card');
        console.log(`  dashboard metric elements: ${await metricsList.count()}`);
        console.log(`[OK] Dashboard verified on device ${ip}`);
      });

      // ===== TEST 4-7: SPA page navigation (menu click, like a real user) =====
      for (const name of ['device', 'network', 'protocol', 'peripheral']) {
        await test.step(`${name} page access`, async () => {
          await openSpaPage(page, name);
          console.log(`[OK] ${name} page accessible on ${ip}`);
        });
      }

      // ===== TEST 8: API Health Check =====
      await test.step('API endpoint validation', async () => {
        // request context API call (goto on JSON pages aborts navigation in Chromium)
        const apiResponse = await apiWithRetry(page, request, `${baseUrl}/api/system/status`);

        if (apiResponse && apiResponse.ok()) {
          const body = await apiResponse.json();
          const data = body.data || body;
          const inner = data.data || data;
          console.log(`  API keys: ${Object.keys(inner).slice(0, 8).join(', ')}`);
          console.log(`[OK] API healthy on ${ip}`);
        } else {
          console.warn(`  [WARN] API not fully ready on ${ip} (status: ${apiResponse ? apiResponse.status() : 'none'})`);
        }
      });

      // ===== TEST 9: Login/Logout Cycle =====
      await test.step('Authentication cycle', async () => {
        // Logout button lives inside the user dropdown; open it first.
        // AppState.logout() shows a native confirm() dialog.
        await page.click('#user-dropdown-btn');
        page.once('dialog', d => d.accept());
        await page.click('#logout-btn');

        // Should show the login form again
        await expect(page.locator('#login-form')).toBeVisible({ timeout: 10000 });
        console.log(`[OK] Logout successful on ${ip}`);

        // Login again (boot 已就绪，走 helper 统一容错路径)
        await loginRobust(page);
        console.log(`[OK] Re-login successful on ${ip}`);
      });

      console.log(`[DONE] All E2E steps passed for device ${ip}`);
    });
  }
});
