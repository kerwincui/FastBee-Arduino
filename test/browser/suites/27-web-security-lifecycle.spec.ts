import { test, expect } from '../fixtures/base.fixture';
import { enableOptionalUiFeatures } from '../fixtures/optional-features.fixture';
const { MAPPING: privateNames } = require('../../../scripts/web-private-properties');

const XSS_PAYLOAD = '<img data-web-xss src=x onerror="window.__fastbeeXss=true">';

async function changePage(page: import('@playwright/test').Page, pageName: string) {
  await page.evaluate((name) => (window as any).AppState.changePage(name), pageName);
  await expect(page.locator(`#${pageName}-page`)).toBeVisible({ timeout: 15_000 });
  const moduleNames: Record<string, string> = {
    dashboard: 'dashboard',
    network: 'network',
    peripheral: 'peripherals',
    features: 'feature-config',
    users: 'users',
  };
  const moduleName = moduleNames[pageName];
  if (moduleName) {
    await page.waitForFunction(
      ({ expectedPage, expectedModule }) => {
        const global = window as any;
        return global.AppState?.currentPage === expectedPage &&
          global.ModuleLoader?.isLoaded?.(expectedModule) === true;
      },
      { expectedPage: pageName, expectedModule: moduleName },
      { timeout: 15_000 }
    );
  }
}

test.describe('Suite-27: Web security and lifecycle', () => {
  test('WEB-SEC-001: dashboard device strings render as text at desktop and mobile @quick', async ({ authPage }) => {
    await authPage.waitForFunction(() =>
      typeof (window as any).AppState?._renderDashboardConnectionStatus === 'function'
    );
    await authPage.evaluate((payload) => {
      (window as any).__fastbeeXss = false;
      const state = (window as any).AppState;
      state._renderDashboardConnectionStatus('monitor-network-status', true, payload);
      state._applyNetworkStatus({
        success: true,
        data: { status: payload, ssid: payload, rssi: payload, signalStrength: payload },
      });
    }, XSS_PAYLOAD);

    for (const width of [1280, 390]) {
      await authPage.setViewportSize({ width, height: 844 });
      await expect(authPage.locator('#monitor-network-status')).toContainText(XSS_PAYLOAD);
      await expect(authPage.locator('#ns-status')).toContainText(XSS_PAYLOAD);
      expect(await authPage.locator('[data-web-xss]').count()).toBe(0);
      expect(await authPage.evaluate(() => (window as any).__fastbeeXss)).toBe(false);
      const overflow = await authPage.locator('#dashboard-page').evaluate((element) =>
        element.scrollWidth > element.clientWidth + 1
      );
      expect(overflow, `${width}px dashboard should not overflow`).toBe(false);
    }
  });

  test('WEB-SEC-002: standalone fullscreen rejects SSID and status markup @quick', async ({ page }) => {
    await page.addInitScript(() => { (window as any).__fastbeeXss = false; });
    await page.route('**/api/system/info*', async (route) => {
      await route.fulfill({
        json: {
          success: true,
          data: {
            device: { chipModel: 'ESP32-S3', cpuFreqMHz: 240, sdkVersion: 'test' },
            uptime: { formatted: '1m' },
            network: { connected: true, ssid: XSS_PAYLOAD, ipAddress: '192.0.2.1' },
            flash: {}, memory: {}, filesystem: {},
          },
        },
      });
    });
    await page.route('**/api/network/status*', async (route) => {
      await route.fulfill({
        json: { success: true, data: { status: XSS_PAYLOAD, ssid: XSS_PAYLOAD } },
      });
    });

    await page.goto('/pages/fullscreen.html', { waitUntil: 'domcontentloaded' });
    await expect(page.locator('#monitor-network-status')).toContainText(XSS_PAYLOAD);
    await expect(page.locator('#ns-status')).toContainText(XSS_PAYLOAD);
    expect(await page.locator('[data-web-xss]').count()).toBe(0);
    expect(await page.evaluate(() => (window as any).__fastbeeXss)).toBe(false);

    for (const width of [1280, 390]) {
      await page.setViewportSize({ width, height: 844 });
      const overflow = await page.locator('.fullscreen-container').evaluate((element) =>
        element.scrollWidth > element.clientWidth + 1
      );
      expect(overflow, `${width}px fullscreen should not overflow`).toBe(false);
    }
  });

  test('WEB-SEC-003: network save reflection stays text-only and timers stop on exit @quick', async ({ authPage, navigateTo }) => {
    await authPage.route('**/api/network/config*', async (route) => {
      if (route.request().method() !== 'PUT') {
        await route.continue();
        return;
      }
      await route.fulfill({
        json: {
          success: true,
          data: {
            restartRequired: true,
            mode: 1,
            modeText: 'AP',
            apSSID: XSS_PAYLOAD,
            apIP: XSS_PAYLOAD,
          },
        },
      });
    });

    await navigateTo('network');
    await authPage.waitForFunction(() =>
      typeof (window as any).AppState?.saveNetworkConfig === 'function' && (window as any).AppState._networkConfigLoaded === true
    );
    await authPage.evaluate((payload) => {
      (window as any).__fastbeeXss = false;
      const mode = document.getElementById('wifi-mode') as HTMLSelectElement | null;
      if (mode) mode.value = '1';
      return (window as any).AppState.saveNetworkConfig(payload);
    }, XSS_PAYLOAD);

    const notice = authPage.locator('.notification-success').last();
    await expect(notice).toContainText(XSS_PAYLOAD);
    await expect(authPage.locator('#wifi-mode-notice-text')).toContainText(XSS_PAYLOAD);
    expect(await authPage.locator('[data-web-xss]').count()).toBe(0);
    expect(await authPage.evaluate(() => (window as any).__fastbeeXss)).toBe(false);

    await changePage(authPage, 'dashboard');
    const timerState = await authPage.evaluate(() => {
      const state = (window as any).AppState;
      return {
        poll: state._networkStatusPollingTimer,
        pollCountdown: state._networkStatusPollingCountdown,
        saveCountdown: state._networkSaveCountdownTimer,
        refresh: state._networkRefreshTimer,
        deferred: state._networkDeferredTimers?.length || 0,
      };
    });
    expect(timerState).toEqual({
      poll: null,
      pollCountdown: null,
      saveCountdown: null,
      refresh: null,
      deferred: 0,
    });
  });

  test('WEB-SEC-004: repeated network mounts bind one refresh and developer lock remains global @quick', async ({ authPage }) => {
    for (let round = 0; round < 4; round++) {
      await changePage(authPage, 'network');
      await changePage(authPage, 'dashboard');
    }
    await changePage(authPage, 'network');
    await authPage.waitForTimeout(700);

    const refreshCalls = await authPage.evaluate(() => {
      const state = (window as any).AppState;
      const original = state.loadNetworkStatus;
      let calls = 0;
      state.loadNetworkStatus = function() {
        calls++;
        return Promise.resolve();
      };
      (document.getElementById('refresh-status-btn') as HTMLButtonElement).click();
      state.loadNetworkStatus = original;
      return calls;
    });
    expect(refreshCalls).toBe(1);

    await changePage(authPage, 'features');
    await authPage.waitForFunction(() =>
      typeof (window as any).AppState?.applyFeatureDeveloperModeState === 'function'
    );
    await authPage.evaluate(() => (window as any).AppState.setDeveloperModeState(false));
    await expect(authPage.locator('[data-feature-toggle]').first()).toBeDisabled();

    await changePage(authPage, 'peripheral');
    await expect(authPage.locator('#add-peripheral-btn')).toBeDisabled();
    await authPage.evaluate(() => (window as any).AppState.setDeveloperModeState(true));
    await expect(authPage.locator('#add-peripheral-btn')).toBeEnabled();

    await changePage(authPage, 'features');
    await expect(authPage.locator('[data-feature-locked="true"]').first()).toBeDisabled();
    await expect(authPage.locator('[data-feature-toggle]:not([data-feature-locked="true"])').first()).toBeEnabled();
  });

  test('WEB-UX-001: editing a disabled user keeps status and allows password to stay unchanged @quick', async ({ authPage }) => {
    await enableOptionalUiFeatures(authPage, ['userAdmin']);
    let updatePayload: URLSearchParams | null = null;
    await authPage.route('**/api/users?*', async route => {
      await route.fulfill({ json: { success: true, data: { users: [{
        username: 'disabled-user', enabled: false, isLocked: false,
        description: 'before', lastLogin: 0,
      }] } } });
    });
    await authPage.route('**/api/users/update*', async (route) => {
      updatePayload = new URLSearchParams(route.request().postData() || '');
      await route.fulfill({ json: { success: true, message: 'updated' } });
    });

    await changePage(authPage, 'users');
    await expect(authPage.locator('#users-page [data-page-fallback]')).toHaveCount(0);
    await expect(authPage.locator('#users-list-summary')).toHaveText('共 1 个用户 · 0 个可用');
    await expect(authPage.locator('#users-table-body')).toContainText('disabled-user');
    await authPage.locator('#users-table-body .fb-btn-action-edit').click();
    await expect(authPage.locator('#add-user-modal')).toBeVisible();
    await expect(authPage.locator('#add-password-input')).not.toHaveAttribute('required', '');
    await expect(authPage.locator('#add-password-input')).toHaveAttribute('placeholder', '留空则不修改密码');
    await authPage.locator('#add-description-input').fill('after');
    await authPage.locator('#confirm-add-user-btn').click();
    await expect.poll(() => updatePayload).not.toBeNull();

    expect(updatePayload!.get('username')).toBe('disabled-user');
    expect(updatePayload!.get('description')).toBe('after');
    expect(updatePayload!.get('enabled')).toBe('false');
    expect(updatePayload!.has('password')).toBe(false);
    await expect(authPage.locator('#add-user-modal')).toHaveCount(0);
  });

  test('WEB-UX-002: runtime-managed peripheral types stay out of the create form @quick', async ({ authPage }) => {
    await authPage.route('**/api/peripherals/types*', async (route) => {
      await route.fulfill({
        json: {
          success: true,
          data: {
            gpio: [{ value: 12, name: '数字输出', implemented: true, configurable: true }],
            sensor: [{ value: 22, name: 'DHT', implemented: true, configurable: true }],
            protocol: [{
              value: 51,
              name: 'Modbus Device',
              implemented: false,
              configurable: false,
              runtimeManaged: true,
              owner: 'protocol.modbus',
            }],
          },
          hardware: {
            chip: 'ESP32-S3',
            inputPins: [1, 2, 4, 5],
            outputPins: [1, 2, 4, 5],
            adcPins: [1, 2],
            dacPins: [],
            touchPins: [],
          },
        },
      });
    });

    await changePage(authPage, 'peripheral');
    await authPage.locator('#add-peripheral-btn').click();
    await expect(authPage.locator('#peripheral-modal')).toBeVisible();
    const typeSelect = authPage.locator('#peripheral-type-input');
    await expect(typeSelect).toBeEnabled();
    await expect(typeSelect.locator('option[value="51"]')).toHaveCount(0);
    await expect(typeSelect.locator('option[value="22"]')).toHaveCount(1);

    await typeSelect.selectOption('22');
    await authPage.evaluate(names => {
      const state = (window as any).AppState;
      const key = (name: string) => names[name] in state ? names[name] : name;
      state[key('_syncPeripheralTypeSelect')](state[key('_periphTypeCatalog')]);
    }, privateNames);
    await expect(typeSelect).toHaveValue('22');
    await expect(typeSelect.locator('option[value="51"]')).toHaveCount(0);
  });
});
