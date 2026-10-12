import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
const root = resolve(__dirname, '../../..');
const source = (p: string) => readFileSync(resolve(p.startsWith('web-src/') ? (process.env.COM15_RECOVERY_SOURCE_ROOT || root) : root, p), 'utf8');
const fixture = JSON.parse(source('test/browser/fixtures/com15-monitor-cache.json'));

test.beforeEach(async ({ page }) => {
  await page.route('http://actions.test/', r => r.fulfill({ contentType: 'text/html', body: '<meta charset="utf-8"><div id="device-control-page" class="active"><div id="dc-content"></div></div>' }));
  await page.goto('http://actions.test/');
  await page.evaluate(() => {
    const w = window as any;
    w.AppState = { currentPage: 'device-control', _featureActive: { deviceControl: true } };
    w.escapeHtml = (v: any) => String(v ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    w.__posts = []; w.__notices = [];
    w.Notification = { success: (text: string) => w.__notices.push(['success', text]), error: (text: string) => w.__notices.push(['error', text]) };
    w.apiPost = async (url: string, body: object) => { w.__posts.push({ url, body }); return { success: true, state: 'queued', operationId: 31 }; };
  });
  await page.addScriptTag({ content: source('web-src/js/utils.js') });
  for (const file of ['core', 'render', 'layout', 'modbus-ops']) await page.addScriptTag({ content: source(`web-src/modules/runtime/device-control/${file}.js`) });
  await page.evaluate(() => (window as any).AppState.setupDeviceControlEvents());
});

async function render(page: any, data: any, status: any = { tasks: [] }, devices: any[] = []) {
  await page.evaluate(({ data, status, devices }: any) => {
    const a = (window as any).AppState;
    a._controlData = data; a._modbusStatus = status;
    a._modbusConfiguredDevices = devices; a._modbusDevices = devices.filter((d: any) => d.enabled !== false);
    document.getElementById('dc-content')!.innerHTML = a._renderControlPanel(data);
  }, { data, status, devices });
}

test('every captured COM15 rule has one execute button, with no actions on render', async ({ page }) => {
  await render(page, fixture.controls, fixture.status, [{ name: '外接继电器', deviceType: 'relay', _configIndex: 0, channelCount: 4 }]);
  for (const rule of Object.values(fixture.controls).flat() as any[]) {
    await expect(page.locator(`.dc-control-flow > .dc-execution-card button.dc-ctrl-btn[data-id="${rule.id}"]`)).toHaveCount(1);
    await expect(page.locator(`button.dc-ctrl-btn[data-id="${rule.id}"]`)).toHaveText('执行一次');
  }
  await expect(page.locator('.dc-monitor-card')).toHaveCount(9);
  await expect(page.locator('.dc-monitor-grid .dc-ctrl-btn, .dc-modbus-device-panel .dc-ctrl-btn')).toHaveCount(0);
  await expect(page.locator('.dc-execution-card button[data-id="s3_modbus_water_leak"]')).toHaveCount(1);
  await expect(page.locator('[data-dc-sort-key="peripheral-modbus:0"] button[data-id="s3_modbus_relay_off"]')).toHaveCount(1);
  await expect(page.locator('[data-dc-sort-key="combined"] button[data-id="s3_modbus_environment"]')).toHaveCount(1);
  await expect(page.locator('[data-dc-sort-key="peripheral-do1"] button[data-id="s3_photoelectric_relay_toggle"]')).toHaveCount(1);
  await expect(page.locator('[data-id="ai_soil_humidity_soil_humidity"] .dc-monitor-value')).toHaveText('0');
  await expect(page.locator('[data-id="ai_soil_humidity_soil_humidity"]')).toHaveAttribute('data-measurement-valid', 'false');
  expect(await page.evaluate(() => (window as any).__posts)).toEqual([]);
});

test('secondary sensor actions, disabled rules, unknown actions and missing targets stay visible', async ({ page }) => {
  await render(page, {
    gpio: [{ id: 'mixed', name: '先控制后采集', actionType: 0, actionCount: 2, targets: [{ id: 'sensor-a' }], targetPeriphId: 'sensor-a', sensors: [{ key: 'sample', periphId: 'sensor-a', value: 0 }] }],
    modbus: [{ id: 'missing', name: '采集待配置任务', actionType: 18, pollTaskIndex: [88] }],
    other: [{ id: 'off', name: '禁用自动触发', enabled: false, actionType: 99 }, { id: 'empty', name: '空规则', actionType: -1, unavailableReason: '无执行动作' }]
  });
  await expect(page.locator('.dc-ctrl-btn')).toHaveCount(4);
  await expect(page.locator('.dc-monitor-card[data-id="sample"] .dc-monitor-value')).toHaveText('0');
  await expect(page.locator('.dc-execution-card button[data-id="mixed"]')).toHaveCount(1);
  await expect(page.locator('.dc-monitor-card button')).toHaveCount(0);
  await expect(page.locator('button[data-id="empty"]')).toBeDisabled();
  await page.locator('button[data-id="off"]').click();
  expect(await page.evaluate(() => (window as any).__posts)).toEqual([{ url: '/api/periph-exec/run', body: { id: 'off' } }]);
});

test('sensor and Modbus templates stay separate with configured target names and indices', async ({ page }) => {
  await render(page, { sensor: [{ id: 'sample-rule', name: '采集', actionType: 19, targets: [{ id: 'soil' }], sensors: [{ key: 'soil_value', periphId: 'soil', quality: 'low', value: 4 }] }],
    modbus: [{ id: 'dev-two', name: '关灯', actionType: 18, targetPeriphId: 'modbus:1' }, { id: 'disabled-dev', name: '设备已停用', actionType: 18, targetPeriphId: 'modbus:0' }]
  }, { tasks: [] }, [{ enabled: false, _configIndex: 0, name: '未启用设备' }, { _configIndex: 1, name: '灯', deviceType: 'relay', channelCount: 1 }]);
  await expect(page.locator('[data-dc-sort-key="peripheral-soil"] button[data-id="sample-rule"]')).toHaveCount(1);
  await expect(page.locator('.dc-monitor-value')).toHaveText('--');
  await expect(page.locator('[data-dc-sort-key="peripheral-modbus:1"] button[data-id="dev-two"]')).toHaveCount(1);
  await expect(page.locator('[data-dc-sort-key="peripheral-modbus:1"] .dc-control-group-title')).toHaveText('执行一次 · 灯');
  await expect(page.locator('[data-dc-sort-key="modbus-relay-1"] .dc-coil-card')).toHaveCount(1);
  await expect(page.locator('.dc-monitor-grid .dc-ctrl-btn, .dc-modbus-device-panel .dc-ctrl-btn')).toHaveCount(0);
  await expect(page.locator('[data-dc-sort-key="peripheral-modbus:0"] button[data-id="disabled-dev"]')).toHaveCount(1);
});

test('rules shared across categories appear once and measurement updates do not recreate actions', async ({ page }) => {
  const rule = { id: 'shared', name: '采集一次', sensors: [{ key: 'temp', periphId: 'probe', value: 21 }] };
  await render(page, { gpio: [rule], sensor: [rule] });
  await expect(page.locator('.dc-ctrl-btn')).toHaveCount(1);
  const unchanged = await page.evaluate(() => {
    const a = (window as any).AppState;
    const button = document.querySelector('.dc-execution-card button');
    a._dcUpdateMonitorCard(document.querySelector('.dc-monitor-card'), { value: 24, valueValid: true, quality: 'valid' });
    return button === document.querySelector('.dc-execution-card button');
  });
  expect(unchanged).toBe(true);
  await expect(page.locator('.dc-monitor-value')).toHaveText('24');
  await page.locator('.dc-ctrl-btn').click();
  expect(await page.evaluate(() => (window as any).__posts)).toEqual([{ url: '/api/periph-exec/run', body: { id: 'shared' } }]);
});

test('new execution cards avoid all restored cards and preserve saved device geometry', async ({ page }) => {
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.addStyleTag({ content: source('web-src/css/main.css') });
  await render(page, { sensor: [{ id: 'read-soil', name: '采集土壤', sensors: [{ key: 'soil', periphId: 'soil', value: 6.7 }] }] }, { tasks: [] }, [{ name: '继电器', deviceType: 'relay', channelCount: 4 }]);
  const saved = { 'monitor-data': { x: 0, y: 0, w: 695, h: 200 }, 'modbus-devices': { x: 0, y: 215, w: 695, h: 120 }, 'modbus-relay-0': { x: 0, y: 350, w: 340, h: 350 } };
  await page.evaluate(saved => { const a = (window as any).AppState; localStorage.setItem(a._DC_LAYOUT_KEY, JSON.stringify(saved)); a._dcApplyLayout(); }, saved);
  const layout = await page.evaluate(() => {
    const cards = Array.from(document.querySelectorAll('.dc-control-flow > [data-dc-sort-key]')) as HTMLElement[];
    const collisions = [];
    for (let i = 0; i < cards.length; i++) for (let j = i + 1; j < cards.length; j++) {
      const a = cards[i].getBoundingClientRect(), b = cards[j].getBoundingClientRect();
      if (Math.min(a.right, b.right) > Math.max(a.left, b.left) + 1 && Math.min(a.bottom, b.bottom) > Math.max(a.top, b.top) + 1) collisions.push([cards[i].dataset.dcSortKey, cards[j].dataset.dcSortKey]);
    }
    return { collisions, geometry: Object.fromEntries(cards.map(c => [c.dataset.dcSortKey, { x: c.offsetLeft, y: c.offsetTop, w: c.offsetWidth, h: c.offsetHeight }])) };
  });
  expect(layout.collisions).toEqual([]);
  for (const [key, geometry] of Object.entries(saved)) expect(layout.geometry[key]).toEqual(geometry);
  expect(layout.geometry['peripheral-soil'].x).toBeGreaterThanOrEqual(710);
  await page.locator('.dc-execution-card button').click();
  expect(await page.evaluate(() => (window as any).__posts)).toEqual([{ url: '/api/periph-exec/run', body: { id: 'read-soil' } }]);
});

test('busy rule submits exactly once even across rerender; queued is not execution success', async ({ page }) => {
  const data = { modbus: [{ id: 'poll', name: '读一次', actionType: 18 }] };
  await render(page, data);
  await page.evaluate(() => { const w = window as any; w.apiPost = (url: string, body: object) => { w.__posts.push({ url, body }); return new Promise(resolve => { w.__finish = resolve; }); }; });
  await page.locator('button[data-id="poll"]').click();
  await render(page, data);
  await expect(page.locator('button[data-id="poll"]')).toBeDisabled();
  await page.evaluate(() => { const w = window as any; const b = document.querySelector('button[data-id="poll"]') as HTMLButtonElement; b.disabled = false; b.click(); w.__finish({ success: true, state: 'queued' }); });
  await expect(page.locator('button[data-id="poll"]')).toBeEnabled();
  expect(await page.evaluate(() => (window as any).__posts.length)).toBe(1);
  expect(await page.evaluate(() => (window as any).__notices)).toEqual([['success', '执行已提交；输出结果待确认']]);
});

test('whole mixed system rule confirms first; numeric input cancels or sends zero without losing it', async ({ page }) => {
  await render(page, { other: [{ id: 'mixed', name: '组合系统操作', actionType: 30, hasSystemAction: true, hasSetMode: true }] });
  await page.evaluate(() => { const w = window as any; w.__confirm = false; w.confirm = () => w.__confirm; w.__input = null; w.AppState.promptPeriphExecRunValue = async () => w.__input; });
  await page.locator('.dc-ctrl-btn').click();
  expect(await page.evaluate(() => (window as any).__posts.length)).toBe(0);
  await page.evaluate(() => { (window as any).__confirm = true; });
  await page.locator('.dc-ctrl-btn').click();
  await expect(page.locator('.dc-ctrl-btn')).toBeEnabled();
  expect(await page.evaluate(() => (window as any).__posts.length)).toBe(0);
  await page.evaluate(() => { (window as any).__input = '0'; });
  await page.locator('.dc-ctrl-btn').click();
  expect(await page.evaluate(() => (window as any).__posts)).toEqual([{ url: '/api/periph-exec/run', body: { id: 'mixed', value: '0' } }]);
});

test('dialog lock prevents duplicate prompts and leaving the page cancels submission', async ({ page }) => {
  await render(page, { other: [{ id: 'value', name: '设值', hasSetMode: true }] });
  await page.evaluate(() => { const w = window as any; w.__prompts = 0; w.AppState.promptPeriphExecRunValue = () => { w.__prompts++; return new Promise(r => { w.__inputDone = r; }); }; });
  await page.locator('.dc-ctrl-btn').click();
  await expect(page.locator('.dc-ctrl-btn')).toBeDisabled();
  await page.evaluate(() => { const w = window as any; const b = document.querySelector('.dc-ctrl-btn') as HTMLButtonElement; b.disabled = false; b.click(); w.AppState._dcLifecycleGeneration++; w.__inputDone('10'); });
  await expect.poll(() => page.evaluate(() => (window as any).AppState._dcRuleRuns.size)).toBe(0);
  expect(await page.evaluate(() => [(window as any).__prompts, (window as any).__posts.length])).toEqual([1, 0]);
});

test('every backend action category remains visible and repeated event setup submits only once', async ({ page }) => {
  const data: Record<string, any[]> = { gpio: [], modbus: [], system: [], script: [], sensor: [], other: [] };
  for (let type = 0; type <= 32; type++) {
    const group = type <= 5 || type === 13 || type === 14 ? 'gpio' : type >= 16 && type <= 18 ? 'modbus' : type >= 6 && type <= 12 ? 'system' : type === 15 ? 'script' : type === 19 ? 'sensor' : 'other';
    data[group].push({ id: 'type-' + type, name: '动作 ' + type, actionType: type, enabled: false });
  }
  await render(page, data);
  await expect(page.locator('.dc-ctrl-btn')).toHaveCount(33);
  await page.evaluate(() => (window as any).AppState.setupDeviceControlEvents());
  await page.locator('button[data-id="type-18"]').click();
  expect(await page.evaluate(() => (window as any).__posts)).toEqual([{ url: '/api/periph-exec/run', body: { id: 'type-18' } }]);
});

test('a late execution response after navigation cannot notify or overwrite a different page', async ({ page }) => {
  await render(page, { gpio: [{ id: 'relay', name: '操作继电器', actionType: 0 }] });
  await page.evaluate(() => { const w = window as any; w.apiPost = () => new Promise(r => { w.__finish = r; }); });
  await page.locator('.dc-ctrl-btn').click();
  await page.evaluate(() => { const w = window as any; w.AppState._dcLifecycleGeneration++; w.AppState.currentPage = 'dashboard'; document.getElementById('dc-content')!.innerHTML = '<p>其他页面</p>'; w.__finish({ success: true, state: 'success' }); });
  await expect.poll(() => page.evaluate(() => (window as any).AppState._dcRuleRuns.size)).toBe(0);
  await expect(page.locator('#dc-content')).toHaveText('其他页面');
  expect(await page.evaluate(() => (window as any).__notices)).toEqual([]);
});

test('saved short cards can scroll to every action and button clicks do not drag cards', async ({ page }) => {
  await page.addStyleTag({ content: source('web-src/css/main.css') });
  await render(page, { other: Array.from({ length: 8 }, (_, i) => ({ id: 'saved-' + i, name: '操作 ' + i })) });
  await page.evaluate(() => { const a = (window as any).AppState; localStorage.setItem(a._DC_LAYOUT_KEY, JSON.stringify({ other: { x: 0, y: 0, w: 330, h: 120 } })); a._dcApplyLayout(); a._dcInitFreeLayout(); });
  const card = page.locator('[data-dc-sort-key="other"]');
  expect(await card.evaluate(e => getComputedStyle(e).overflowY)).toBe('auto');
  const geometry = (e: HTMLElement) => ({left:e.offsetLeft,top:e.offsetTop,width:e.offsetWidth,height:e.offsetHeight});
  const before = await card.evaluate(geometry);
  await page.locator('button[data-id="saved-7"]').click();
  expect(await card.evaluate(e => e.scrollTop)).toBeGreaterThan(0);
  expect(await card.evaluate(geometry)).toEqual(before);
  expect(await page.evaluate(() => (window as any).__posts)).toEqual([{ url: '/api/periph-exec/run', body: { id: 'saved-7' } }]);
});

for (const failure of ['http', 'rejected', 'failed']) test(`execution ${failure} keeps values unchanged and unlocks retry`, async ({ page }) => {
  await render(page, { sensor: [{ id: 'read', name: '读取温度', sensors: [{ key: 'temp', value: 21 }] }] });
  await page.evaluate(failure => { const w = window as any; w.apiPost = async () => { if (failure === 'http') throw { data: { message: 'HTTP 429: busy' } }; return { success: failure === 'failed', state: failure, message: '执行失败' }; }; }, failure);
  await page.locator('.dc-ctrl-btn').click();
  await expect(page.locator('.dc-ctrl-btn')).toBeEnabled();
  await expect(page.locator('.dc-monitor-value')).toHaveText('21');
  expect(await page.evaluate(() => (window as any).__notices[0][0])).toBe('error');
});

for (const width of [1060, 1420]) test('execution cards share a desktop column at ' + width + 'px', async ({ page }) => {
  await page.setViewportSize({ width: width + 40, height: 1200 });
  await page.addStyleTag({ content: source('web-src/css/main.css') });
  await page.addStyleTag({ content: '.dc-control-flow{width:' + width + 'px}' });
  await render(page, fixture.controls, fixture.status, [{ name: 'Modbus继电器', deviceType: 'relay', channelCount: 4 }]);
  await page.evaluate(() => (window as any).AppState._dcApplyLayout());
  const geometry = await page.evaluate(() => {
    const cards = Array.from(document.querySelectorAll('.dc-control-flow > [data-dc-sort-key]')) as HTMLElement[];
    const actions = cards.filter(c => c.classList.contains('dc-execution-card'));
    return { positions: actions.map(c => c.offsetLeft),
      collisions: actions.filter(a => cards.some(b => b !== a && Math.min(a.offsetLeft + a.offsetWidth, b.offsetLeft + b.offsetWidth) > Math.max(a.offsetLeft, b.offsetLeft) + 1 && Math.min(a.offsetTop + a.offsetHeight, b.offsetTop + b.offsetHeight) > Math.max(a.offsetTop, b.offsetTop) + 1)).length };
  });
  expect(new Set(geometry.positions).size).toBe(1);
  expect(geometry.positions[0]).toBeGreaterThanOrEqual(710);
  expect(geometry.collisions).toBe(0);
});

test('offline preview groups actions outside all native Modbus control types', async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.addStyleTag({ content: source('web-src/css/main.css') });
  await page.addStyleTag({ content: 'body{margin:0}#dc-content{padding:16px;max-width:1100px;margin:auto}*{transition:none!important}' });
  const devices = ['relay', 'pwm', 'pid', 'motor'].map((deviceType, index) => ({ name: deviceType === 'relay' ? 'Modbus继电器' : deviceType.toUpperCase(), deviceType, _configIndex: index, channelCount: 4, slaveAddress: index + 1 }));
  const data = structuredClone(fixture.controls);
  for (let i = 1; i < devices.length; i++) data.modbus.push({ id: 'control-template-' + i, name: devices[i].name + '模板', targetPeriphId: 'modbus:' + i });
  await render(page, data, fixture.status, devices);
  await expect(page.locator('.dc-modbus-device-panel')).toHaveCount(4);
  await expect(page.locator('.dc-monitor-grid .dc-rule-actions, .dc-modbus-device-panel .dc-rule-actions')).toHaveCount(0);
  for (let i = 0; i < devices.length; i++) {
    const actual = await page.locator('.dc-modbus-device-panel[data-dev-idx="' + i + '"]').evaluate(el => el.outerHTML);
    const expected = await page.evaluate(({ i, dev }) => (window as any).AppState._renderSingleModbusPanel(i, dev), { i, dev: devices[i] });
    // DOM serialization may normalize attributes; compare browser-parsed markup.
    expect(actual).toBe(await page.evaluate(html => { const el = document.createElement('div'); el.innerHTML = html; return el.firstElementChild!.outerHTML; }, expected));
  }
  expect(await page.evaluate(() => (window as any).__posts)).toEqual([]);
  // Use the captured COM15 configuration for the screenshot; test-only PWM/PID/motor
  // entries above establish that templates do not extend any native control panel.
  await render(page, fixture.controls, fixture.status, [devices[0]]);
  await page.evaluate(() => {
    const note = document.createElement('p'); note.textContent = '离线布局预览 · 历史样本，非设备当前读数'; note.style.cssText = 'text-align:center;color:#666;font:14px sans-serif'; document.body.prepend(note);
    (window as any).AppState._dcApplyLayout();
  });
  await page.screenshot({ path: testInfo.outputPath('execution-cards-preview.png'), fullPage: true });
});

for (const width of [360, 1440]) test(`actions remain accessible at ${width}px without overlaps or injected markup`, async ({ page }, testInfo) => {
  await page.setViewportSize({ width, height: 1000 });
  await page.addStyleTag({ content: source('web-src/css/main.css') });
  await page.addStyleTag({ content: 'body{margin:0}#dc-content{padding:12px}*{transition:none!important}' });
  const data = structuredClone(fixture.controls);
  data.other.push({ id: 'escape"<x>', name: '<img src=x onerror=alert(1)>很长的外设规则名称'.repeat(2), actionType: 99 });
  await render(page, data, fixture.status);
  await page.evaluate(() => { const a = (window as any).AppState; a._dcApplyLayout(); a._dcInitFreeLayout(); });
  expect(await page.locator('#dc-content img').count()).toBe(0);
  const overflow = await page.evaluate(() => {
    const flow = document.querySelector('.dc-control-flow')!.getBoundingClientRect();
    return Array.from(document.querySelectorAll('.dc-rule-row')).filter(row => {
      const r = row.getBoundingClientRect(); const b = row.querySelector('button')!.getBoundingClientRect();
      return b.width < 40 || b.height < 25 || b.right > flow.right + 1 || b.bottom > r.bottom + 1;
    }).length;
  });
  expect(overflow).toBe(0);
  await page.screenshot({ path: testInfo.outputPath('device-control-actions.png'), fullPage: true });
});
