import { test, expect, Page } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { genericPeripheralTypes, genericPeripherals } from '../generic-peripheral-fixtures.mjs';

const root = resolve(__dirname, '../../..');
const source = (path: string) => readFileSync(resolve(root, path), 'utf8');

async function mountUi(page: Page) {
  await page.setContent('<!doctype html><html lang="zh-CN"><head><meta name="viewport" content="width=device-width,initial-scale=1"></head><body>' +
    source('web-src/pages/modals.html') + source('web-src/pages/fragments/peripheral-io.html') +
    '<div id="features-list"></div></body></html>');
  await page.addStyleTag({ content: source('web-src/css/main.css') });
  await page.evaluate(({ types, peripherals }) => {
    const w = window as any;
    w.__devices = structuredClone(peripherals);
    w.__types = structuredClone(types);
    w.__writes = [];
    w.__notifications = [];
    w.escapeHtml = (value: unknown) => String(value ?? '').replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[char]));
    w.Notification = { success() {}, warning() {}, error(message: string) { w.__notifications.push(message); } };
    w.AppState = {
      registerModule(_name: string, methods: any) { Object.assign(this, methods); },
      guardDeveloperModeAction: () => true, isDeveloperModeEnabled: () => true,
      applyDeveloperModeState() {}, getDeveloperModeDisabledText: () => '',
      _ensureModalForAction: async (id: string) => document.getElementById(id),
      renderPagination() {}, currentPage: 'peripheral',
    };
    w.PageLoader = { unmountModal() {} };
    w.apiGet = w.apiGetFresh = async (path: string, params: any = {}) => {
      if (path.includes('/api/peripherals/types')) return { success: true, data: w.__types, hardware: { chip: 'ESP32-S3', inputPins: [1, 2, 4, 5], outputPins: [1, 2, 4, 5] } };
      if (path.includes('/api/peripherals')) return params.id ? { success: true, data: w.__devices.find((p: any) => p.id === params.id) } : { success: true, data: w.__devices, total: w.__devices.length };
      return { success: true, data: [] };
    };
    w.apiPost = w.apiPostJson = async (path: string, payload: any) => {
      w.__writes.push({ path, payload: structuredClone(payload) });
      if (path.includes('/preflight')) return { success: true, data: { allowed: true, requiresConfirmation: false, issues: [] } };
      const device = { ...payload, id: payload.id || 'created', type: Number(payload.type), enabled: !!payload.enabled, featureActive: true, implemented: true,
        pins: String(payload.pins || '').split(',').filter(Boolean).map(Number), params: structuredClone(payload) };
      if (device.type === 38) { delete device.channel; delete device.params.channel; }
      const index = w.__devices.findIndex((p: any) => p.id === device.id);
      if (index < 0) w.__devices.push(device); else w.__devices[index] = device;
      return { success: true, data: { id: device.id } };
    };
  }, { types: genericPeripheralTypes, peripherals: genericPeripherals });
  for (const file of ['web-src/js/state-ui.js', ...['peripherals', 'peripheral-save', 'peripheral-io', 'feature-config', 'periph-exec', 'periph-exec-form', 'periph-exec-modbus', 'periph-exec-sensor'].map(name => `web-src/modules/runtime/${name}.js`)]) {
    await page.addScriptTag({ content: source(file) });
  }
  await page.evaluate(() => {
    const w = window as any;
    const app = w.AppState;
    app._periphTypeCatalog = Object.values(w.__types).flat();
    app._periphProviders = w.__devices;
    app._pePeripherals = w.__devices;
    app._peTypeCatalog = app._periphTypeCatalog;
    app._featureState = [];
    app._bindPeripheralModalEvents();
    app.loadPeripherals = () => Promise.resolve();
  });
}

async function openType(page: Page, type: number) {
  await page.evaluate(() => (window as any).AppState.openPeripheralModal());
  await page.locator('#peripheral-type-input').selectOption(String(type));
  await page.locator('#peripheral-id-input').fill('test_device');
  await page.locator('#peripheral-name-input').fill('界面测试外设');
}

async function deferPeripheralRequests(page: Page) {
  await page.evaluate(() => {
    const w = window as any;
    const getter = w.apiGetFresh;
    w.__pendingPeripheralRequests = [];
    w.__devices.push({ ...structuredClone(w.__devices.find((device: any) => device.id === 'offline_ina')), id: 'second_ina', name: '第二个 INA226' });
    w.AppState._ensurePeripheralIoModal = async () => document.getElementById('peripheral-io-modal');
    w.apiGetFresh = (path: string, params: any = {}) => {
      if (!params.id || !['/api/peripherals/', '/api/peripherals/read'].includes(path)) return getter(path, params);
      return new Promise((resolve, reject) => {
        w.__pendingPeripheralRequests.push({ path, params, resolve, reject, settled: false });
      });
    };
  });
}

async function settlePeripheralRequest(page: Page, id: string, path: string, fail = false, value?: number) {
  await page.evaluate(({ id, path, fail, value }) => {
    const w = window as any;
    const request = w.__pendingPeripheralRequests.find((entry: any) => !entry.settled && entry.path === path && entry.params.id === id);
    if (!request) throw new Error(`No pending peripheral request for ${path} ${id}`);
    request.settled = true;
    if (fail) request.reject(new Error('旧会话请求失败'));
    else request.resolve({ success: true, data: value === undefined
      ? structuredClone(w.__devices.find((device: any) => device.id === id))
      : { value, channel: 'voltage', unit: 'V' } });
  }, { id, path, fail, value });
}

async function noOverflow(page: Page, selector: string) {
  const result = await page.locator(selector).evaluate(element => ({
    width: element.clientWidth, scroll: element.scrollWidth,
    escaped: Array.from(element.querySelectorAll('input,select,textarea')).filter((child: any) => {
      const r = child.getBoundingClientRect();
      return r.width > 0 && (r.left < -1 || r.right > window.innerWidth + 1);
    }).map(child => (child as HTMLElement).id || child.className),
  }));
  expect(result.scroll).toBeLessThanOrEqual(result.width + 1);
  expect(result.escaped).toEqual([]);
}

test.beforeEach(async ({ page }) => {
  (page as any).__scriptErrors = [];
  page.on('pageerror', error => (page as any).__scriptErrors.push(error.message));
  await mountUi(page);
});
test.afterEach(async ({ page }) => { expect((page as any).__scriptErrors).toEqual([]); });

for (const width of [1280, 390]) {
  test(`GENERIC-001: INA226 saves and reopens calibrated fields at ${width}px`, async ({ page }, info) => {
    await page.setViewportSize({ width, height: 844 });
    await openType(page, 38);
    await expect(page.locator('#generic-sensor-driver option')).toHaveCount(8);
    await page.locator('#generic-sensor-driver').selectOption('ina226');
    await page.locator('#generic-sensor-bus-id').selectOption('offline_i2c');
    await expect(page.locator('#peripheral-pins-input')).toBeDisabled();
    await expect(page.locator('#generic-sensor-address')).toHaveValue('0x40');
    await expect(page.locator('#generic-sensor-channel option')).toHaveText(['voltage (V)', 'current (A)', 'power (W)']);
    await page.locator('#generic-sensor-channel').selectOption('current');
    await page.locator('#generic-driver-shuntResistance').fill('0.01');
    await page.locator('#generic-driver-maxCurrent').fill('5');
    await page.locator('#generic-driver-averaging').selectOption('64');
    await noOverflow(page, '#peripheral-modal .modal-content');
    await page.screenshot({ path: info.outputPath(`ina226-${width}.png`) });
    await page.locator('#save-peripheral-btn').click();
    await expect.poll(() => page.evaluate(() => (window as any).__writes.filter((w: any) => w.path === '/api/peripherals').length)).toBe(1);
    const payload = await page.evaluate(() => (window as any).__writes.find((w: any) => w.path === '/api/peripherals').payload);
    expect(payload).toMatchObject({ driver: 'INA226', busPeripheralId: 'offline_i2c', address: 64, pins: '', shuntResistance: 0.01, maxCurrent: 5, conversionTimeUs: 1100, averaging: 64 });
    expect(payload).not.toHaveProperty('channel');
    await page.evaluate(() => (window as any).AppState.openPeripheralModal(true, 'test_device'));
    await expect(page.locator('#generic-sensor-driver')).toHaveValue('ina226');
    await expect(page.locator('#generic-driver-shuntResistance')).toHaveValue('0.01');
    await expect(page.locator('#generic-driver-averaging')).toHaveValue('64');
    await expect(page.locator('#generic-sensor-channel')).toHaveValue('voltage');
    await expect(page.locator('#generic-sensor-bus-id')).toHaveValue('offline_i2c');
  });

  test(`GENERIC-002: PCF8574 parent and logical output preserve references at ${width}px`, async ({ page }, info) => {
    await page.setViewportSize({ width, height: 844 });
    await openType(page, 55);
    await page.locator('#io-expander-bus-id').selectOption('offline_i2c');
    await expect(page.locator('#io-expander-driver')).toHaveValue('pcf8574');
    await noOverflow(page, '#peripheral-modal .modal-content');
    await page.locator('#save-peripheral-btn').click();
    await expect.poll(() => page.evaluate(() => (window as any).__writes.filter((w: any) => w.path === '/api/peripherals').length)).toBe(1);
    const parent = await page.evaluate(() => (window as any).__writes.find((w: any) => w.path === '/api/peripherals').payload);
    expect(parent).toMatchObject({ type: '55', driver: 'PCF8574', address: 32, busPeripheralId: 'offline_i2c', pins: '', sampleInterval: 20 });
    await openType(page, 12);
    await page.locator('#gpio-io-source').selectOption('expander');
    await page.locator('#gpio-provider-id').selectOption('offline_pcf');
    await page.locator('#gpio-provider-channel').selectOption('7');
    await page.locator('#gpio-invert-logic').check();
    await expect(page.locator('#peripheral-pins-input')).toBeDisabled();
    await expect(page.locator('#gpio-input-mode-group')).toBeHidden();
    await noOverflow(page, '#peripheral-modal .modal-content');
    await page.screenshot({ path: info.outputPath(`logical-io-${width}.png`) });
    await page.locator('#save-peripheral-btn').click();
    await expect.poll(() => page.evaluate(() => (window as any).__writes.filter((w: any) => w.path === '/api/peripherals').length)).toBe(2);
    expect(await page.evaluate(() => (window as any).__writes.filter((w: any) => w.path === '/api/peripherals')[1].payload)).toMatchObject({ ioSource: 'expander', providerId: 'offline_pcf', channel: 7, pins: '', invertLogic: true, inputMode: 0, passive: false });
  });
}

test('GENERIC-003: invalid calibration and unavailable drivers never save', async ({ page }) => {
  await openType(page, 38);
  await page.locator('#generic-sensor-driver').selectOption('ina226');
  await page.locator('#generic-sensor-bus-id').selectOption('offline_i2c');
  await page.locator('#save-peripheral-btn').click();
  await expect(page.locator('#peripheral-error')).toContainText('不在驱动允许范围');
  await page.locator('#generic-driver-shuntResistance').fill('0.01');
  await page.locator('#generic-driver-maxCurrent').fill('5');
  await page.locator('#generic-driver-conversionTimeUs').selectOption('8244');
  await page.locator('#generic-driver-averaging').selectOption('1024');
  await page.locator('#save-peripheral-btn').click();
  await expect(page.locator('#peripheral-error')).toContainText('500 ms');
  await page.evaluate(() => {
    const app = (window as any).AppState;
    app._updateGenericSensorDriverFields(false, { driver: 'UNKNOWN_CHIP', address: 64, busPeripheralId: 'offline_i2c' });
  });
  await expect(page.locator('#generic-sensor-driver')).toHaveValue('unknown_chip');
  await page.locator('#save-peripheral-btn').click();
  await expect(page.locator('#peripheral-error')).toContainText('当前固件');
  expect(await page.evaluate(() => (window as any).__writes.length)).toBe(0);
});

test('GENERIC-004: configured measurement differs from reporting field and survives editor remount', async ({ page }, info) => {
  await page.evaluate(() => {
    const app = (window as any).AppState;
    const config = { sensorCategory: 'INA226', periphId: 'offline_ina', channelField: 'current', dataField: 'batteryCurrent', scaleFactor: 1, offset: 0, decimalPlaces: 3, unit: 'A', sensorLabel: '负载电流' };
    const container = document.getElementById('periph-exec-actions')!;
    container.innerHTML = '<div class="periph-exec-config-item" data-index="0"><select class="pe-action-type"><option value="19">传感器读取</option></select><div class="fb-form-group pe-target-group"><select class="pe-target-periph"></select></div><div class="pe-action-lazy-host"></div></div>';
    app._mountPeriphExecLazyActionPanel(container.firstElementChild, 19, { targetPeriphId: 'offline_ina', actionValue: JSON.stringify(config) });
    container.addEventListener('change', event => app._handlePeriphExecActionChange(event));
    app.showModal('periph-exec-modal');
  });
  await expect(page.locator('.pe-sensor-category')).toHaveValue('configured');
  await expect(page.locator('.pe-sensor-channel-field')).toHaveValue('current');
  await expect(page.locator('.pe-sensor-property')).toHaveValue('batteryCurrent');
  await expect(page.locator('.pe-sensor-unit')).toHaveValue('A');
  await expect(page.locator('.pe-sensor-scale')).toBeHidden();
  await page.locator('.pe-sensor-channel-field').selectOption('power');
  await page.locator('.pe-sensor-property').fill('loadPower');
  await expect(page.locator('.pe-sensor-unit')).toHaveValue('W');
  for (const width of [1280, 390]) {
    await page.setViewportSize({ width, height: 844 });
    await noOverflow(page, '#periph-exec-modal .modal-content');
    await page.screenshot({ path: info.outputPath(`configured-sensor-${width}.png`) });
  }
  const action = await page.evaluate(() => {
    const app = (window as any).AppState;
    const actions = app._collectPeriphExecActions();
    return { action: JSON.parse(actions[0].actionValue), error: app._peConfiguredSensorError };
  });
  expect(action.error).toBe('');
  expect(action.action).toMatchObject({ periphId: 'offline_ina', sensorCategory: 'INA226', channelField: 'power', dataField: 'loadPower', unit: 'W', scaleFactor: 1, offset: 0, decimalPlaces: 3 });
  await page.evaluate(config => {
    const app = (window as any).AppState;
    app._mountPeriphExecLazyActionPanel(document.querySelector('#periph-exec-actions .periph-exec-config-item'), 19, { targetPeriphId: config.periphId, actionValue: JSON.stringify(config) }, true);
  }, action.action);
  await expect(page.locator('.pe-sensor-channel-field')).toHaveValue('power');
  await expect(page.locator('.pe-sensor-property')).toHaveValue('loadPower');
  await expect(page.locator('.pe-sensor-unit')).toHaveValue('W');
  await page.evaluate(() => {
    const app = (window as any).AppState;
    app._pePeripherals.find((p: any) => p.id === 'offline_ina').featureActive = false;
    app._collectPeriphExecActions();
  });
  expect(await page.evaluate(() => (window as any).AppState._peConfiguredSensorError)).toContain('未启用');
});

test('GENERIC-005: boot state, budget denial and unavailable measurements remain explicit', async ({ page }) => {
  await page.evaluate(() => {
    const app = (window as any).AppState;
    app._featureState = [{ key: 'ioExpander', name: '通用 IO 扩展器', group: 'peripheral', compiled: true, userEnabled: true, active: false, requiresRestart: true, bootEnabled: false, budgetPassed: true }];
    app.renderFeatureList();
  });
  await expect(page.locator('[data-feature-key="ioExpander"]')).toContainText('重启后启用');
  await page.evaluate(() => {
    const app = (window as any).AppState;
    Object.assign(app._featureState[0], { bootEnabled: true, budgetPassed: false });
    app.renderFeatureList();
    app._populatePeripheralDiagnosticChannels({ params: { mode: 1, channel: 'gyroZ' } }, 'MPU6050');
    app._renderPeripheralI2cData({ valid: false, value: 0, error: 'timeout' });
  });
  await expect(page.locator('[data-feature-key="ioExpander"]')).toContainText('内存预算不足');
  await expect(page.locator('#peripheral-i2c-channel option')).toHaveText(['gyroX (rad/s)', 'gyroY (rad/s)', 'gyroZ (rad/s)', 'temperature (°C)']);
  await expect(page.locator('#peripheral-i2c-channel')).toHaveValue('gyroZ');
  await expect(page.locator('#peripheral-i2c-value')).toHaveText('读数无效');
  await openType(page, 38);
  await page.evaluate(() => {
    const app = (window as any).AppState;
    app._periphTypeCatalog.find((entry: any) => entry.value === 38).featureActive = false;
    app._updateGenericSensorDriverFields(false, { driver: 'INA226', channel: 'power', address: 64 });
  });
  await expect(page.locator('#generic-sensor-driver')).toHaveValue('ina226');
  await expect(page.locator('#generic-sensor-feature-status')).toContainText('功能尚未启用');
});

test('GENERIC-008: segmented I2C scans aggregate addresses and stop when no progress is possible', async ({ page }) => {
  const scans = await page.evaluate(async () => {
    const w = window as any;
    const app = w.AppState;
    app._periphIoKind = 'i2c-sensor';
    app._periphIoId = 'offline_ina';
    const requests: number[] = [];
    const partial: string[] = [];
    w.apiGetFresh = async (_path: string, params: any) => {
      requests.push(params.startAddress);
      partial.push(document.getElementById('peripheral-i2c-output')!.textContent || '');
      return { success: true, data: requests.length === 1 ?
        { addresses: [32, 64], truncated: true, nextAddress: 65 } :
        { addresses: [64, 72], truncated: false } };
    };
    await app.runPeripheralI2cOperation('scan');
    return { requests, partial };
  });
  expect(scans.requests).toEqual([1, 65]);
  expect(scans.partial[1]).toContain('扫描未完成');
  await expect(page.locator('#peripheral-i2c-output')).toHaveText('0x20, 0x40, 0x48\n[扫描完成]');
  const stalledRequests = await page.evaluate(async () => {
    const w = window as any;
    let requests = 0;
    w.apiGetFresh = async () => { requests++; return { success: true, data: { addresses: [], truncated: true, nextAddress: 1 } }; };
    await w.AppState.runPeripheralI2cOperation('scan');
    return requests;
  });
  expect(stalledRequests).toBe(1);
  await expect(page.locator('#peripheral-i2c-output')).toContainText('已扫描范围内未发现设备');
  await expect(page.locator('#peripheral-i2c-output')).toContainText('扫描未完成');
  await expect(page.locator('#peripheral-io-error')).toContainText('扫描没有进展');
});

test('GENERIC-009: closing diagnostics stops scan continuation and ignores its late response', async ({ page }) => {
  const result = await page.evaluate(async () => {
    const w = window as any;
    const app = w.AppState;
    app._periphIoKind = 'i2c-sensor';
    app._periphIoId = 'offline_ina';
    let resolveScan: (result: any) => void = () => {};
    let requests = 0;
    w.apiGetFresh = () => { requests++; return new Promise(resolve => { resolveScan = resolve; }); };
    const scanning = app.runPeripheralI2cOperation('scan');
    app.closePeripheralIoModal();
    app._periphIoKind = 'i2c-sensor';
    app._periphIoId = 'offline_ina';
    app._setPeripheralIoBusy(true);
    app._setPeripheralIoOutput('peripheral-i2c-output', '新会话');
    resolveScan({ success: true, data: { addresses: [64], truncated: true, nextAddress: 65 } });
    await scanning;
    return { requests, busy: app._periphIoBusy };
  });
  expect(result).toEqual({ requests: 1, busy: true });
  await expect(page.locator('#peripheral-i2c-output')).toHaveText('新会话');
});

for (const outcome of ['success', 'failure']) {
  test(`GENERIC-013: a late editor ${outcome} cannot replace the newly selected peripheral`, async ({ page }) => {
    await deferPeripheralRequests(page);
    await page.evaluate(() => (window as any).AppState.openPeripheralModal(true, 'offline_ina'));
    await page.evaluate(() => {
      const app = (window as any).AppState;
      app.closePeripheralModal();
      return app.openPeripheralModal(true, 'second_ina');
    });
    await settlePeripheralRequest(page, 'second_ina', '/api/peripherals/');
    await expect(page.locator('#peripheral-original-id')).toHaveValue('second_ina');
    await page.locator('#peripheral-name-input').fill('保留新的编辑内容');
    await settlePeripheralRequest(page, 'offline_ina', '/api/peripherals/', outcome === 'failure');
    await expect(page.locator('#peripheral-original-id')).toHaveValue('second_ina');
    await expect(page.locator('#peripheral-name-input')).toHaveValue('保留新的编辑内容');
    expect(await page.evaluate(() => (window as any).__notifications)).toEqual([]);
    await page.locator('#save-peripheral-btn').click();
    await expect.poll(() => page.evaluate(() => (window as any).__writes.filter((entry: any) => entry.path === '/api/peripherals/update').length)).toBe(1);
    expect(await page.evaluate(() => (window as any).__writes.find((entry: any) => entry.path === '/api/peripherals/update').payload)).toMatchObject({ id: 'second_ina', name: '保留新的编辑内容' });
  });

  test(`GENERIC-014: a late diagnostic detail ${outcome} cannot relabel or unlock the next peripheral`, async ({ page }) => {
    await deferPeripheralRequests(page);
    await page.evaluate(() => { const w = window as any; w.__oldOpening = w.AppState.openPeripheralIoModal('offline_ina'); });
    await page.evaluate(() => { const w = window as any; w.AppState.closePeripheralIoModal(); w.__newOpening = w.AppState.openPeripheralIoModal('second_ina'); });
    await settlePeripheralRequest(page, 'offline_ina', '/api/peripherals/', outcome === 'failure');
    await page.evaluate(() => (window as any).__oldOpening);
    await expect(page.locator('#peripheral-io-id')).toHaveText('second_ina');
    await expect(page.locator('#peripheral-io-name')).toHaveText('加载中...');
    await expect(page.locator('#peripheral-io-error')).toHaveText('');
    expect(await page.evaluate(() => ({ busy: (window as any).AppState._periphIoBusy, reads: (window as any).__pendingPeripheralRequests.filter((entry: any) => entry.path === '/api/peripherals/read').length }))).toEqual({ busy: true, reads: 0 });
    await settlePeripheralRequest(page, 'second_ina', '/api/peripherals/');
    await expect(page.locator('#peripheral-io-name')).toHaveText('第二个 INA226');
    await settlePeripheralRequest(page, 'second_ina', '/api/peripherals/read', false, 222);
    await page.evaluate(() => (window as any).__newOpening);
    await expect(page.locator('#peripheral-i2c-value')).toHaveText('222 V');
  });

  for (const action of ['refresh', 'read']) {
    test(`GENERIC-015: a late ${action} ${outcome} cannot overwrite or unlock a reopened diagnostic`, async ({ page }) => {
      await deferPeripheralRequests(page);
      await page.evaluate(() => { const w = window as any; w.__firstOpening = w.AppState.openPeripheralIoModal('offline_ina'); });
      await settlePeripheralRequest(page, 'offline_ina', '/api/peripherals/');
      await settlePeripheralRequest(page, 'offline_ina', '/api/peripherals/read', false, 0);
      await page.evaluate(() => (window as any).__firstOpening);
      await page.evaluate(action => {
        const w = window as any;
        w.__oldReading = action === 'refresh' ? w.AppState.refreshPeripheralIo(true) : w.AppState.runPeripheralI2cOperation('read');
        w.AppState.closePeripheralIoModal();
        w.__newOpening = w.AppState.openPeripheralIoModal('offline_ina');
      }, action);
      await settlePeripheralRequest(page, 'offline_ina', '/api/peripherals/');
      await settlePeripheralRequest(page, 'offline_ina', '/api/peripherals/read', outcome === 'failure', 111);
      await page.evaluate(() => (window as any).__oldReading);
      await expect(page.locator('#peripheral-i2c-value')).toHaveText('--');
      await expect(page.locator('#peripheral-io-error')).toHaveText('');
      const pending = await page.evaluate(async () => {
        const w = window as any;
        await w.AppState.refreshPeripheralIo(true);
        await w.AppState.runPeripheralI2cOperation('read');
        return { busy: w.AppState._periphIoBusy, reads: w.__pendingPeripheralRequests.filter((entry: any) => entry.path === '/api/peripherals/read').length };
      });
      expect(pending).toEqual({ busy: true, reads: 3 });
      await settlePeripheralRequest(page, 'offline_ina', '/api/peripherals/read', false, 222);
      await page.evaluate(() => (window as any).__newOpening);
      await expect(page.locator('#peripheral-i2c-value')).toHaveText('222 V');
      expect(await page.evaluate(() => (window as any).AppState._periphIoBusy)).toBe(false);
    });
  }
}

test('GENERIC-016: closing while a modal template loads prevents a late open and request', async ({ page }) => {
  const result = await page.evaluate(async () => {
    const w = window as any;
    const app = w.AppState;
    let resolveForm: (modal: Element | null) => void = () => {};
    let resolveIo: (modal: Element | null) => void = () => {};
    let requests = 0;
    const getter = w.apiGetFresh;
    w.apiGetFresh = (...args: any[]) => { requests++; return getter(...args); };
    app._ensureModalForAction = () => new Promise(resolve => { resolveForm = resolve; });
    const editing = app.openPeripheralModal(true, 'offline_ina');
    app.closePeripheralModal();
    resolveForm(document.getElementById('peripheral-modal'));
    await editing;
    app._ensurePeripheralIoModal = () => new Promise(resolve => { resolveIo = resolve; });
    const diagnosing = app.openPeripheralIoModal('offline_ina');
    app.closePeripheralIoModal();
    resolveIo(document.getElementById('peripheral-io-modal'));
    await diagnosing;
    return { requests, diagnosticId: app._periphIoId };
  });
  expect(result).toEqual({ requests: 0, diagnosticId: '' });
  await expect(page.locator('#peripheral-modal')).toBeHidden();
  await expect(page.locator('#peripheral-io-modal')).toBeHidden();
});

test('GENERIC-006: late descriptors preserve saved calibration and invalid GPIO channels', async ({ page }) => {
  await openType(page, 38);
  await page.evaluate(() => {
    const w = window as any;
    const app = w.AppState;
    app._periphTypeCatalog = null;
    app._updateGenericSensorDriverFields(false, { driverName: 'INA226', address: 67, busPeripheralId: 'offline_i2c', shuntResistance: 0.02, maxCurrent: 3, conversionTimeUs: 588, averaging: 4 });
    app._periphTypeCatalog = Object.values(w.__types).flat();
    app._syncPeripheralTypeSelect(app._periphTypeCatalog);
  });
  await expect(page.locator('#generic-sensor-driver')).toHaveValue('ina226');
  await expect(page.locator('#generic-sensor-channel')).toHaveValue('voltage');
  await expect(page.locator('#generic-driver-shuntResistance')).toHaveValue('0.02');
  await expect(page.locator('#generic-driver-maxCurrent')).toHaveValue('3');
  await expect(page.locator('#generic-driver-conversionTimeUs')).toHaveValue('588');
  await expect(page.locator('#generic-driver-averaging')).toHaveValue('4');
  await page.locator('#peripheral-type-input').selectOption('12');
  await page.locator('#gpio-io-source').selectOption('expander');
  await page.locator('#gpio-provider-id').selectOption('offline_pcf');
  await page.evaluate(() => (window as any).AppState._updateGpioSourceFields(9));
  await expect(page.locator('#gpio-provider-channel')).toHaveValue('9');
  await page.locator('#save-peripheral-btn').click();
  await expect(page.locator('#peripheral-error')).toContainText('逻辑通道');
  expect(await page.evaluate(() => (window as any).__writes.length)).toBe(0);
});

test('GENERIC-010: MPU mode survives reopen and temperature selection without a persisted channel', async ({ page }) => {
  await page.evaluate(() => {
    const w = window as any;
    w.__devices.push({ id: 'saved_mpu', name: '陀螺仪', type: 38, enabled: false, pins: [4, 5], params: { driver: 'MPU6050', address: 104, mode: 1, frequency: 400000, sampleInterval: 1000 } });
  });
  await page.evaluate(() => (window as any).AppState.openPeripheralModal(true, 'saved_mpu'));
  await expect(page.locator('#generic-sensor-channel')).toHaveValue('gyroX');
  await page.locator('#generic-sensor-channel').selectOption('temperature');
  await page.locator('#save-peripheral-btn').click();
  await expect.poll(() => page.evaluate(() => (window as any).__writes.filter((item: any) => !item.path.includes('preflight')).length)).toBe(1);
  const payload = await page.evaluate(() => (window as any).__writes.find((item: any) => !item.path.includes('preflight')).payload);
  expect(payload).toMatchObject({ mode: 1, frequency: 400000, driver: 'MPU6050' });
  expect(payload).not.toHaveProperty('channel');
  await page.evaluate(() => (window as any).AppState.openPeripheralModal(true, 'saved_mpu'));
  await expect(page.locator('#generic-sensor-channel')).toHaveValue('gyroX');
  await page.locator('#generic-sensor-channel').selectOption('accelZ');
  await page.locator('#save-peripheral-btn').click();
  const edited = await page.evaluate(() => (window as any).__writes.filter((item: any) => !item.path.includes('preflight')).pop().payload);
  expect(edited.mode).toBe(0);
});

test('GENERIC-011: PCF8574 interval is rejected before posting above the firmware limit', async ({ page }) => {
  await openType(page, 55);
  await page.locator('#io-expander-bus-id').selectOption('offline_i2c');
  await page.locator('#io-expander-interval').fill('1001');
  await page.locator('#save-peripheral-btn').click();
  await expect(page.locator('#peripheral-error')).toContainText('采样间隔');
  expect(await page.evaluate(() => (window as any).__writes.length)).toBe(0);
});

test('GENERIC-012: MCU pull and interrupt types cannot select an expander', async ({ page }) => {
  await openType(page, 11);
  await page.locator('#gpio-io-source').selectOption('expander');
  for (const type of [13, 14, 18, 19, 20]) {
    await page.locator('#peripheral-type-input').selectOption(String(type));
    await expect(page.locator('#gpio-source-fields')).toBeHidden();
    await expect(page.locator('#gpio-expander-fields')).toBeHidden();
    await expect(page.locator('#gpio-io-source')).toHaveValue('gpio');
    await expect(page.locator('#gpio-io-source option[value="expander"]')).toBeDisabled();
  }
});

test('GENERIC-007: logical expander outputs exclude PWM and breathing actions', async ({ page }) => {
  await page.evaluate(() => {
    const app = (window as any).AppState;
    app._pePeripherals.push({ id: 'logical_out', name: '逻辑输出', type: 12, enabled: true, pins: [], params: { ioSource: 'expander', providerId: 'offline_pcf', channel: 2 } });
    app._createPeriphExecActionElement({ actionType: 3, targetPeriphId: 'logical_out', actionValue: '1000' }, 0);
    app.showModal('periph-exec-modal');
  });
  for (const value of [3, 4, 5, 11, 12, 24, 25, 26, 27, 28, 29]) {
    await expect(page.locator(`.pe-action-type option[value="${value}"]`)).toBeDisabled();
  }
  for (const value of [0, 1, 2, 10, 13, 14, 30]) {
    await expect(page.locator(`.pe-action-type option[value="${value}"]`)).toBeEnabled();
  }
  await expect(page.locator('.pe-action-type')).toHaveValue('3');
});
