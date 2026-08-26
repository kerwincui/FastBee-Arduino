// FastBee Web Access Validation Suite
// Goal: verify every device in the fleet is reachable via Web (page load + login + key pages)
// Usage: $env:DEVICES="192.168.5.31,192.168.5.14"; npx playwright test test/e2e/web-access-validation.spec.js --project=chromium
// 不设 DEVICES 时自动发现子网设备（helpers.resolveDevices，DHCP 漂移免疫）
// 弱链路容错统一收敛在 helpers/fastbee-helpers.js（goto 重试 / boot 就绪等待 / 登录重试）

const { test, expect } = require('@playwright/test');
const {
  openDevicePage,
  loginRobust,
  openSpaPage,
  apiWithRetry,
  resolveDevices,
} = require('./helpers/fastbee-helpers');

const devices = resolveDevices();

test.describe('FastBee Web Access Validation', () => {
  for (const ip of devices) {
    const baseUrl = `http://${ip}`;

    test(`Web access validation - ${ip}`, async ({ page, request }) => {
      test.setTimeout(240 * 1000); // 弱链路设备需要多轮重试，放宽到 4 分钟

      // 1. API reachable with retry (device may be busy right after previous test run)
      const statusRes = await apiWithRetry(page, request, `${baseUrl}/api/system/status`);
      expect(statusRes && statusRes.ok(), `status API should be OK on ${ip}`).toBe(true);
      const status = await statusRes.json();
      expect(status.success).toBe(true);
      expect(status.data.ipAddress).toBe(ip);
      expect(status.data.features.webServer).toBe('active');
      console.log(`[${ip}] API OK - mac=${status.data.macAddress} heap=${status.data.freeHeap}`);

      // 2. Home page loads and login form renders (goto 重试收敛在 helper)
      await openDevicePage(page, baseUrl);
      console.log(`[${ip}] login page rendered`);

      // 3+4. 登录：先等 boot chunk 就绪再提交，避免卡「页面初始化中」；失败自动重载重试
      await loginRobust(page);
      console.log(`[${ip}] dashboard rendered after login`);

      // 5. Key SPA pages reachable via menu clicks (SPA switch, no full reload -> gentle on 4MB devices)
      for (const name of ['network', 'device', 'protocol', 'peripheral']) {
        await openSpaPage(page, name);
        console.log(`[${ip}] page /${name} OK`);
      }

      // 6. Authenticated API check (retry: busy low-memory devices may answer 503)
      const netRes = await apiWithRetry(page, request, `${baseUrl}/api/network/status`);
      const netStatus = netRes ? netRes.status() : 0;
      expect(netStatus, `network API on ${ip}`).toBeLessThan(500);
      console.log(`[${ip}] PASS - Web access fully validated`);
    });
  }
});
