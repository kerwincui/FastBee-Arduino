import { test, expect, Page } from '@playwright/test';
import { readFileSync } from 'node:fs';
import path from 'node:path';

const root = path.resolve(__dirname, '../../..');
const fragment = readFileSync(path.join(root, 'web-src/pages/fragments/protocol-modbus-rtu.html'), 'utf8');
const source = ['common', 'protocol-config', 'modbus-config'].map(name => readFileSync(path.join(root, `web-src/modules/runtime/protocol/${name}.js`), 'utf8')).join('\n');
const task = (i: number) => ({ name: `Sensor ${i}`, slaveAddress: i + 1, functionCode: 3, startAddress: 0, quantity: 1, mappings: [] });
const relay = { name: 'Relay 6', deviceType: 'relay', slaveAddress: 6, channelCount: 2 };

async function setup(page: Page, tasks: object[], devices: object[]) {
  const saved = { modbusRtu: { enabled: false, master: { tasks, devices } } };
  let failure = '';
  const posts: Record<string, string>[] = [];
  await page.route('http://modbus.test/**', async route => {
    const request = route.request();
    const url = new URL(request.url());
    if (url.pathname === '/api/protocol/modbus-rtu/config') {
      const body = request.postDataJSON();
      posts.push(body);
      if (failure) return route.fulfill({ status: 409, json: { success: false, message: failure } });
      saved.modbusRtu.master = { tasks: JSON.parse(body.modbusRtu_master_tasks), devices: JSON.parse(body.modbusRtu_master_devices) };
      return route.fulfill({ json: { success: true, data: { modbusRestarted: true } } });
    }
    if (url.pathname === '/api/protocol/config') return route.fulfill({ json: { success: true, data: saved } });
    if (url.pathname.startsWith('/api/')) return route.fulfill({ json: { success: true, data: [] } });
    await route.fulfill({ contentType: 'text/html', body: `<!doctype html><meta charset="utf-8">${fragment}<script>
      window.notifications = [];
      window.Notification = Object.fromEntries(['error','warning','success'].map(k => [k, m => notifications.push({ kind:k, message:m })]));
      window.AppState = {
        guardDeveloperModeAction: () => true, isDeveloperModeEnabled: () => true,
        applyDeveloperModeState: () => {}, hideElement: () => {}, showElement: () => {},
        _setValue: (id, value) => { const el = document.getElementById(id); if(el) el.value = value; },
        _setCheckbox: (id, value) => { const el = document.getElementById(id); if(el) el.checked = value; }
      };
      window.t = (key, text) => text;
      window.escapeHtml = text => { const el = document.createElement('span'); el.textContent = String(text); return el.innerHTML; };
      window.apiGet = window.apiGetFresh = url => fetch(url).then(r => r.json());
      window.apiPost = (url, body) => fetch(url, { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify(body) }).then(r => r.json());
      window.apiInvalidateCache = () => {};
      ${source}
      AppState._startMasterStatusRefresh = () => {};
      AppState.setupProtocolEvents();
      localStorage.setItem('modbus_devices', JSON.stringify([{ name:'Stale local relay', type:'relay' }]));
      AppState._loadProtocolConfigData('modbus-rtu', {noCache:true}).then(() => window.ready = true);
    </script>` });
  });
  page.on('dialog', dialog => dialog.accept());
  await page.goto('http://modbus.test/');
  await page.waitForFunction(() => (window as any).ready);
  return { posts, saved, fail: (message: string) => { failure = message; } };
}

async function clickDelete(page: Page, source: string, index: number) {
  await page.locator(`[data-protocol-action="delete-device"][data-source="${source}"][data-index="${index}"]`).click();
  await page.waitForFunction(() => !(window as any).AppState._modbusDeletePending);
}

async function reload(page: Page) {
  await page.reload();
  await page.waitForFunction(() => (window as any).ready);
}

test('mixed legacy sensors render once and retain original control/task indices', async ({ page }) => {
  const tasks = Array.from({length: 5}, (_, i) => task(i));
  const state = await setup(page, tasks, [...tasks.map(x => ({ ...x, deviceType: 'sensor' })), relay]);
  await expect(page.locator('#all-devices-body tr')).toHaveCount(6);
  await expect(page.locator('[data-protocol-action="open-control-modal"]')).toHaveAttribute('data-index', '5');
  await expect(page.locator('[data-protocol-action="open-mapping"]').last()).toHaveAttribute('data-index', '4');
  expect(await page.evaluate(() => (window as any).AppState.saveProtocolConfig('modbus-rtu-form'))).toBe(true);
  expect(JSON.parse(state.posts[0].modbusRtu_master_devices)).toHaveLength(6);
});

test('delete saves immediately and an empty control list survives refresh and reload', async ({ page }) => {
  const state = await setup(page, [], [relay]);
  await clickDelete(page, 'control', 0);
  expect(state.posts).toHaveLength(1);
  expect(state.saved.modbusRtu.master.devices).toEqual([]);
  await expect(page.locator('#all-devices-body')).toContainText('暂无子设备');
  await page.evaluate(() => (window as any).AppState._loadProtocolConfigData('modbus-rtu', {noCache:true}));
  await reload(page);
  await expect(page.locator('#all-devices-body')).toContainText('暂无子设备');
  await expect(page.locator('#all-devices-body')).not.toContainText('Stale local relay');
});

test('delete task persists new indices without modifying control devices', async ({ page }) => {
  const state = await setup(page, [task(0), task(1)], [relay]);
  await clickDelete(page, 'sensor', 0);
  expect(state.saved.modbusRtu.master.tasks).toEqual([task(1)]);
  expect(state.saved.modbusRtu.master.devices).toEqual([relay]);
  await reload(page);
  await expect(page.locator('[data-protocol-action="open-mapping"]')).toHaveAttribute('data-index', '0');
  await expect(page.locator('#all-devices-body')).toContainText('Sensor 1');
  await expect(page.locator('#all-devices-body')).not.toContainText('Sensor 0');
});

for (const message of ['删除会改变外设执行规则的 Modbus 索引，请先调整规则：Relay rule (exec1)', 'Modbus tasks/devices must be a JSON array']) {
  test(`rejected save restores deleted row: ${message}`, async ({ page }) => {
    const state = await setup(page, [task(0)], [relay]);
    state.fail(message);
    await clickDelete(page, 'control', 0);
    await expect(page.locator('#all-devices-body tr')).toHaveCount(2);
    expect(state.saved.modbusRtu.master.devices).toEqual([relay]);
    expect(await page.evaluate(() => (window as any).notifications.some((n: any) => n.kind === 'error'))).toBe(true);
    expect(await page.locator('#modbus-rtu-form').evaluate(el => (el as HTMLElement).inert)).toBe(false);
    await reload(page);
    await expect(page.locator('[data-protocol-action="open-control-modal"]')).toHaveAttribute('data-index', '0');
  });
}

test('network rejection restores original task and cached server configuration', async ({ page }) => {
  await setup(page, [task(0)], []);
  await page.evaluate(() => { (window as any).apiPost = () => Promise.reject(new TypeError('Failed to fetch')); });
  await clickDelete(page, 'sensor', 0);
  await expect(page.locator('#all-devices-body')).toContainText('Sensor 0');
  expect(await page.evaluate(() => (window as any).AppState._protocolConfig.modbusRtu.master.tasks.length)).toBe(1);
});
