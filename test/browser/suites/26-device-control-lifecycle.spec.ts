import { test, expect } from '../fixtures/base.fixture';
import { uiScreenshotPath } from '../evidence-paths';
import type { Page, Request, Route } from '@playwright/test';

const DEVICE_CONTROL_APIS = [
  '/api/device/config',
  '/api/periph-exec/controls',
  '/api/modbus/status',
  '/api/protocol/config',
];

async function waitForFeatureState(page: Page) {
  await page.waitForFunction(() => (window as any).AppState?._featureActive !== null);
  await page.waitForFunction(() => {
    const state = (window as any).apiGetGovernorState?.();
    return !state || (state.inflight === 0 && state.queueLength === 0);
  });
}

async function enableDeviceControlForFixture(page: Page) {
  await page.evaluate(() => {
    const state = (window as any).AppState;
    state._featureActive = { ...(state._featureActive || {}), deviceControl: true };
    const menu = document.querySelector<HTMLElement>('.menu-item[data-page="device-control"]');
    if (menu) {
      menu.style.display = '';
      menu.setAttribute('aria-hidden', 'false');
    }
    localStorage.setItem('dc_zoom_level', '1');
  });
}

function isDeviceControlApi(request: Request) {
  const pathname = new URL(request.url()).pathname;
  return DEVICE_CONTROL_APIS.includes(pathname);
}

test.describe('Suite-26: deviceControl lifecycle', () => {
  test('UI-027A: disabled direct navigation stays lightweight', async ({ authPage }) => {
    await waitForFeatureState(authPage);
    await authPage.waitForTimeout(150);

    const requests: string[] = [];
    const recordRequest = (request: Request) => {
      if (isDeviceControlApi(request)) requests.push(new URL(request.url()).pathname);
    };
    authPage.on('request', recordRequest);

    await authPage.evaluate(() => (window as any).AppState.changePage('device-control'));
    await expect(authPage.locator('#device-control-page')).toBeVisible();
    await expect(authPage.locator('[data-page-fallback="feature-disabled"]'))
      .toContainText('该功能当前未启用');
    await authPage.waitForTimeout(250);
    authPage.off('request', recordRequest);

    expect(requests, 'disabled direct navigation must not call device-control data APIs').toEqual([]);
    const state = await authPage.evaluate(() => ({
      page: (window as any).AppState.currentPage,
      runtimeLoaded: (window as any).ModuleLoader?.isLoaded?.('device-control') === true,
      controlData: (window as any).AppState._controlData,
    }));
    expect(state).toEqual({ page: 'device-control', runtimeLoaded: false, controlData: undefined });
  });

  test('UI-027B: page exit cancels stale load and releases runtime state', async ({ authPage }) => {
    await waitForFeatureState(authPage);
    await enableDeviceControlForFixture(authPage);

    let releaseFirst!: () => void;
    let markFirstSeen!: () => void;
    const firstHeld = new Promise<void>((resolve) => { releaseFirst = resolve; });
    const firstSeen = new Promise<void>((resolve) => { markFirstSeen = resolve; });
    let controlsAttempt = 0;
    const controlsPayload = {
      success: true,
      data: {
        gpio: [{ id: 'offline-control', name: 'Offline control', actionType: 0 }],
        modbus: [], system: [], script: [], sensor: [], other: [],
      },
    };

    await authPage.route('**/api/periph-exec/controls*', async (route: Route) => {
      controlsAttempt += 1;
      if (controlsAttempt === 1) {
        markFirstSeen();
        await firstHeld;
      }
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(controlsPayload) })
        .catch(() => {});
    });

    await authPage.evaluate(() => (window as any).AppState.changePage('device-control'));
    await Promise.race([
      firstSeen,
      new Promise((_, reject) => setTimeout(() => reject(new Error('controls request did not start')), 15_000)),
    ]);

    await authPage.evaluate(() => {
      const state = (window as any).AppState;
      state._controlData = { retained: true };
      state._modbusStatus = { tasks: [{ retained: true }] };
      state._modbusConfiguredDevices = [{ retained: true }];
      state._modbusDevices = [{ retained: true }];
      state._dcCoilStates = { 0: [true] };
      state._dcPwmStates = { 0: [42] };
      state._dcPidValues = { 0: { pv: 12 } };
      state._dcDeviceOnline = { 0: true };
      state._dcAutoRefreshTimers.contract = setInterval(() => {}, 10_000);
      state._dcDragEl = document.getElementById('dc-content');
      state._dcResizeEl = document.getElementById('dc-content');
    });

    await authPage.evaluate(() => (window as any).AppState.changePage('dashboard'));
    releaseFirst();
    await authPage.waitForTimeout(350);

    const released = await authPage.evaluate(() => {
      const state = (window as any).AppState;
      return {
        page: state.currentPage,
        controlData: state._controlData,
        modbusStatus: state._modbusStatus,
        modbusConfiguredDevices: state._modbusConfiguredDevices,
        modbusDevices: state._modbusDevices,
        coilKeys: Object.keys(state._dcCoilStates || {}),
        pwmKeys: Object.keys(state._dcPwmStates || {}),
        pidKeys: Object.keys(state._dcPidValues || {}),
        onlineKeys: Object.keys(state._dcDeviceOnline || {}),
        timerKeys: Object.keys(state._dcAutoRefreshTimers || {}),
        pendingDelays: (state._dcPendingDelays || []).length,
        pageLoadPending: state._dcPageLoadPromise !== null,
        sseHandlers: Boolean(state._sseModbusHandler || state._sseSensorHandler),
        layoutRefs: Boolean(state._dcDragEl || state._dcResizeEl || state._dcDragOffset || state._dcResizeStart),
      };
    });
    expect(released).toEqual({
      page: 'dashboard',
      controlData: null,
      modbusStatus: null,
      modbusConfiguredDevices: [],
      modbusDevices: [],
      coilKeys: [], pwmKeys: [], pidKeys: [], onlineKeys: [], timerKeys: [],
      pendingDelays: 0,
      pageLoadPending: false,
      sseHandlers: false,
      layoutRefs: false,
    });

    await authPage.evaluate(() => (window as any).AppState.changePage('device-control'));
    await authPage.waitForFunction(() => {
      const state = (window as any).AppState;
      return state.currentPage === 'device-control' && state._dcPageLoadPromise === null && !!state._controlData;
    });
    await expect(authPage.locator('#dc-zoom-in-btn')).toBeVisible();
    await authPage.locator('#dc-zoom-in-btn').click();
    await expect(authPage.locator('.dc-zoom-wrapper')).toHaveCSS('transform', /matrix\(1\.1,/);

    await authPage.setViewportSize({ width: 390, height: 844 });
    await authPage.waitForTimeout(100);
    const mobileToolbar = await authPage.evaluate(() => {
      const card = document.querySelector('.dc-header-card')?.getBoundingClientRect();
      const actions = document.querySelector('.dc-header-group')?.getBoundingClientRect();
      const labelElement = document.querySelector('.dc-sid-toggle');
      const label = labelElement?.getBoundingClientRect();
      if (!card || !actions || !label || !labelElement) return null;
      const within = (child: DOMRect, parent: DOMRect) =>
        child.left >= parent.left - 1 && child.right <= parent.right + 1 &&
        child.top >= parent.top - 1 && child.bottom <= parent.bottom + 1;
      const textNode = Array.from(labelElement.childNodes)
        .find((node) => node.nodeType === Node.TEXT_NODE);
      const textRange = document.createRange();
      if (textNode) textRange.selectNodeContents(textNode);
      const textLines = new Set(Array.from(textRange.getClientRects())
        .map((rect) => Math.round(rect.top)));
      return {
        actionsInsideCard: within(actions, card),
        labelInsideCard: within(label, card),
        labelTextLines: textLines.size,
      };
    });
    expect(mobileToolbar).toEqual({
      actionsInsideCard: true,
      labelInsideCard: true,
      labelTextLines: 1,
    });

    await authPage.evaluate(() => (window as any).AppState.changePage('dashboard'));
    await expect(authPage.locator('#dashboard-page')).toBeVisible();
    await authPage.unroute('**/api/periph-exec/controls*');
  });

  test('UI-027C: configured disabled Modbus devices stay visible without polling', async ({ authPage }) => {
    await waitForFeatureState(authPage);
    await enableDeviceControlForFixture(authPage);

    const protocolPayload = {
      success: true,
      data: {
        modbusRtu: {
          enabled: true,
          master: {
            devices: [
              { name: '温湿度', sensorId: 'modbus_th', deviceType: 'sensor', slaveAddress: 1, channelCount: 2, enabled: false },
              { name: '继电器', sensorId: 'modbus_relay', deviceType: 'relay', slaveAddress: 5, channelCount: 2, enabled: false },
            ],
          },
        },
      },
    };
    const controlsPayload = {
      success: true,
      data: { gpio: [], modbus: [], system: [], script: [], sensor: [], other: [] },
    };
    const controlRequests: string[] = [];
    const recordRequest = (request: Request) => {
      const pathname = new URL(request.url()).pathname;
      if (pathname.startsWith('/api/modbus/') && pathname !== '/api/modbus/status') controlRequests.push(pathname);
    };
    authPage.on('request', recordRequest);
    await authPage.route('**/api/periph-exec/controls*', (route: Route) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(controlsPayload) }));
    await authPage.route('**/api/modbus/status*', (route: Route) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ success: true, data: { tasks: [] } }) }));
    await authPage.route('**/api/protocol/config*', (route: Route) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(protocolPayload) }));

    await authPage.evaluate(() => (window as any).AppState.changePage('device-control'));
    await expect(authPage.locator('.dc-modbus-device-inventory')).toBeVisible();
    await expect(authPage.locator('.dc-modbus-inventory-row')).toHaveCount(2);
    await expect(authPage.locator('.dc-modbus-inventory-state.is-disabled')).toHaveCount(2);
    await expect(authPage.locator('.dc-modbus-device-panel')).toHaveCount(0);
    for (const width of [1280, 390]) {
      await authPage.setViewportSize({ width, height: 900 });
      const inventory = authPage.locator('.dc-modbus-device-inventory');
      const overflow = await inventory.evaluate((element) =>
        element.scrollWidth > element.clientWidth + 1
      );
      expect(overflow, `${width}px Modbus 子设备清单不应横向溢出`).toBeFalsy();
      await authPage.screenshot({
        path: uiScreenshotPath(`UI-027C-modbus-inventory-${width}.png`),
        fullPage: true,
      });
    }
    await authPage.waitForTimeout(1800);
    expect(controlRequests).toEqual([]);

    authPage.off('request', recordRequest);
    await authPage.unroute('**/api/periph-exec/controls*');
    await authPage.unroute('**/api/modbus/status*');
    await authPage.unroute('**/api/protocol/config*');
  });
  test('WEB-PANEL-MOTOR: one click submits once after remount and dirty parameters require setting', async ({ authPage }) => {
    await waitForFeatureState(authPage);
    await enableDeviceControlForFixture(authPage);
    const actions: string[] = [];
    const protocol = { success: true, data: { modbusRtu: { enabled: true, master: { devices: [
      { name: '测试电机', sensorId: 'motor', deviceType: 'motor', enabled: true, slaveAddress: 1,
        motorRegs: [0, 1, 2, 5, 7], motorDecimals: 0 },
    ] } } } };
    await authPage.route('**/api/protocol/config*', route => route.fulfill({ json: protocol }));
    await authPage.route('**/api/periph-exec/controls*', route => route.fulfill({ json: {
      success: true, data: { gpio: [], modbus: [], system: [], script: [], sensor: [], other: [] },
    } }));
    await authPage.route('**/api/modbus/motor/control', async route => {
      const params = new URLSearchParams(route.request().postData() || '');
      const action = params.get('action') || '';
      actions.push(action);
      await route.fulfill({ json: { success: true, data: { pending: false, success: true,
        motionKnown: false, speed: 50, pulse: 1600 } } });
    });
    for (let index = 0; index < 3; index++) {
      await authPage.evaluate(() => (window as any).AppState.changePage('device-control'));
      const forward = authPage.locator('.dc-motor-action[data-action="forward"]');
      await expect(forward).toBeVisible();
      await forward.click();
      await expect.poll(() => actions.filter(action => action === 'forward').length).toBe(index + 1);
      // A command acknowledgement is not physical motion feedback. Keep the
      // shared motor widget's explicit unconfirmed label and neutral state.
      await expect(authPage.locator('#dc-motor-run-0')).toHaveText('运动未确认');
      await expect(authPage.locator('#dc-motor-run-0')).toHaveClass('motor-run-badge motor-run-idle');
      if (index < 2) await authPage.evaluate(() => (window as any).AppState.changePage('dashboard'));
    }
    await authPage.locator('#dc-motor-speed-in-0').fill('25');
    await authPage.locator('.dc-motor-action[data-action="reverse"]').click();
    await expect(authPage.locator('body')).toContainText('请先设置修改后的速度和脉冲');
    expect(actions.filter(action => action === 'reverse')).toHaveLength(0);
    await authPage.locator('.dc-motor-set[data-param="speed"]').click();
    await expect.poll(() => actions.filter(action => action === 'setSpeed').length).toBe(1);
    await expect(authPage.locator('#dc-motor-speed-in-0')).toHaveAttribute('data-dirty', 'false');
    await authPage.locator('.dc-motor-action[data-action="reverse"]').click();
    await expect.poll(() => actions.filter(action => action === 'reverse').length).toBe(1);
    expect(actions.filter(action => action === 'forward')).toHaveLength(3);
  });

});
