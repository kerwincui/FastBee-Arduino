import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const root = resolve(__dirname, '../../..');
const source = (file: string) => readFileSync(resolve(root, file), 'utf8');

for (const width of [390, 1280]) {
  test(`NC03 actual form creates only disabled local drafts (${width})`, async ({page}) => {
    await page.setViewportSize({width, height: 900});
    const errors: string[] = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.route('**/*', route => route.abort());
    await page.setContent(source('web-src/pages/fragments/protocol-modbus-rtu.html'));
    await page.evaluate(() => {
      const w = window as any;
      w.__writes = 0;
      w.apiPost = w.apiGet = () => { w.__writes++; throw new Error('No device calls allowed'); };
      w.Notification = {warning: (value: string) => {w.__warning = value;}};
      w.AppState = {_masterTasks: [], _modbusDevices: [], _protocolConfig: {},
        guardDeveloperModeAction: () => true, isDeveloperModeEnabled: () => true,
        applyDeveloperModeState() {}};
      document.addEventListener('click', event => {
        const button = (event.target as Element).closest('[data-action]') as HTMLElement;
        if (button && !button.hasAttribute('disabled')) w.AppState[button.dataset.action!]?.();
      });
      const uart = document.getElementById('rtu-peripheral-id') as HTMLSelectElement;
      uart.add(new Option('Fixture UART: parameters already checked', 'uart1'));
      uart.value = 'uart1';
    });
    for (const file of ['web-src/js/utils.js', 'web-src/modules/runtime/protocol/modbus-config.js',
      'web-src/modules/runtime/protocol/modbus-instruments.js']) await page.addScriptTag({content: source(file)});
    await expect(page.locator('#mb-template-address')).toHaveCount(0);
    await page.getByRole('button', {name: '导入只读仪表模板', exact: true}).click();
    await expect(page.getByLabel('实际从站地址')).toBeVisible();
    await expect(page.locator('details > summary').filter({hasText: /^导入只读仪表模板$/})).toBeFocused();
    await page.evaluate(() => (window as any).AppState.openModbusInstrument());
    await expect(page.locator('#mb-template-kind')).toHaveCount(1);
    await page.locator('#mb-template-address').fill('3');
    await page.locator('[data-action="previewModbusInstrument"]').click();
    await expect(page.locator('#mb-template-preview')).toContainText('实物未验证');
    await expect(page.locator('#mb-template-preview')).toContainText('V2.4');
    await page.locator('#mb-template-apply').click();
    expect(await page.evaluate(() => (window as any).AppState._masterTasks.length)).toBe(0);
    await page.locator('#mb-template-confirm').check();
    await page.locator('#mb-template-address').fill('4');
    await page.locator('#mb-template-apply').click();
    expect(await page.evaluate(() => (window as any).AppState._masterTasks.length)).toBe(0);
    await page.locator('[data-action="previewModbusInstrument"]').click();
    await page.locator('#mb-template-confirm').check();
    await page.locator('#mb-template-apply').click();
    const state = await page.evaluate(() => ({tasks: (window as any).AppState._masterTasks, calls: (window as any).__writes}));
    expect(state.tasks).toHaveLength(1);
    expect(state.tasks[0]).toMatchObject({enabled: false, slaveAddress: 4, functionCode: 4, startAddress: 0, quantity: 2});
    expect(state.tasks[0].mappings[0]).toMatchObject({dataType: 4, wordOrder: 0, scaleFactor: 1, unit: 'V'});
    expect(state.calls).toBe(0);
    await expect(page.locator('#all-devices-body')).toContainText('OFF');
    expect(errors).toEqual([]);
  });
}

test('NC03 actual mapping inputs retain zero and reject invalid values without corrupting the draft', async ({page}) => {
  await page.setContent(source('web-src/pages/fragments/protocol-modbus-rtu.html'));
  await page.evaluate(() => {
    const w = window as any;
    w.Notification = {warning() {}};
    w.AppState = {guardDeveloperModeAction: () => true, applyDeveloperModeState() {},
      _masterTasks: [{quantity: 2}], _currentMappingTaskIdx: 0,
      _currentMappings: [{regOffset: 0, dataType: 4, wordOrder: 1, scaleFactor: 0, decimalPlaces: 0, sensorId: 'voltage', unit: 'V'}]};
  });
  for (const file of ['web-src/js/utils.js','web-src/modules/runtime/protocol/modbus-config.js'])
    await page.addScriptTag({content: source(file)});
  await page.evaluate(() => (window as any).AppState._renderMappingTable());
  expect(await page.evaluate(() => (window as any).AppState._collectMappingValues())).toBe(true);
  expect(await page.evaluate(() => (window as any).AppState._currentMappings[0])).toMatchObject({scaleFactor: 0, decimalPlaces: 0, wordOrder: 1});
  await page.locator('#mapping-table-body input').nth(1).fill('');
  expect(await page.evaluate(() => (window as any).AppState._collectMappingValues())).toBe(false);
  expect(await page.evaluate(() => (window as any).AppState._currentMappings[0].scaleFactor)).toBe(0);
  await page.evaluate(() => {
    const a = (window as any).AppState;
    a._currentMappings[0].sensorId = '"><img src=x onerror="window.__injected=1">';
    a._currentMappings[0].unit = '" autofocus="'; a._renderMappingTable();
  });
  expect(await page.locator('#mapping-table-body img').count()).toBe(0);
  expect(await page.evaluate(() => (window as any).__injected)).toBeUndefined();
});
