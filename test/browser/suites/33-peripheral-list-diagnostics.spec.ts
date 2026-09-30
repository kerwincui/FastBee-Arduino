import { test, expect, Page } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
const root = resolve(__dirname, '../../..');
const source = (name: string) => readFileSync(resolve(root, name), 'utf8');

async function mount(page: Page) {
  await page.setContent(source('web-src/pages/peripheral.html') + source('web-src/pages/fragments/peripheral-io.html'));
  await page.addStyleTag({ content: '.fb-hidden { display:none } .page { display:block }' });
  await page.evaluate(() => {
    const w = window as any;
    w.confirm = () => true;
    w.escapeHtml = (s: any) => String(s ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    w.Notification = { success() {}, warning() {}, error() {} };
    w.AppState = { registerModule(_name: string, methods: any) { Object.assign(this, methods); },
      isDeveloperModeEnabled: () => true, guardDeveloperModeAction: () => true,
      applyDeveloperModeState() {}, getDeveloperModeDisabledText: () => '', currentPage: 'periph-exec' };
    w.__rows = Array.from({ length: 11 }, (_, i) => ({ id: `item${i}`, name: `Item ${i}`, type: 1, enabled: true, implemented: true, status: 2,
      category: i === 10 ? 'communication' : 'gpio', triggerSummary: i === 10 ? 1 : 4, flowSummary: '测试流程', pins: [4, 5] }));
    w.__requests = []; w.__clamp = true;
    w.apiGet = w.apiGetFresh = async (path: string, params: any = {}) => {
      const u = new URL(path, 'http://localhost');
      Object.entries(params).forEach(([k,v]) => u.searchParams.set(k, String(v)));
      w.__requests.push(u.pathname + u.search);
      if (params.id && ['/api/peripherals', '/api/peripherals/'].includes(u.pathname)) return {success:true,data:w.__detail};
      if (/\/(read|status)$/.test(u.pathname)) return {success:true,data:{value:23,port:1,connected:true}};
      let rows = w.__rows;
      if (u.searchParams.get('category')) rows = rows.filter((r: any) => r.category === u.searchParams.get('category'));
      if (u.searchParams.get('filter')) rows = rows.filter((r: any) => 'trigger:' + r.triggerSummary === u.searchParams.get('filter'));
      const size = Number(u.searchParams.get('pageSize') || 10);
      const wanted = Number(u.searchParams.get('page') || 1);
      const current = w.__clamp ? Math.min(wanted, Math.max(1, Math.ceil(rows.length / size))) : wanted;
      return {success:true,total:rows.length,page:current,pageSize:size,data:rows.slice((current-1)*size,current*size)};
    };
    w.apiDelete = async (_path: string, params: any) => { w.__rows = w.__rows.filter((r: any) => r.id !== params.id); return {success:true}; };
  });
  for (const file of ['web-src/js/state-ui.js', ...['peripherals','periph-exec','peripheral-io'].map(n => `web-src/modules/runtime/${n}.js`)]) await page.addScriptTag({content:source(file)});
  await page.evaluate(() => {
    const a = (window as any).AppState;
    a._ensurePeripheralIoModal = async () => document.getElementById('peripheral-io-modal');
    a.showModal = (modal: HTMLElement) => { modal.style.display = 'block'; };
    a._periphTypeCatalog = null;
    a.setupPeripheralsEvents(); a.setupPeriphExecEvents();
  });
}

test.beforeEach(async ({page}) => mount(page));

test('diagnostics stays in more menu and remains available to read-only users', async ({page}) => {
  await page.evaluate(async () => {
    const w = window as any;
    w.AppState.isDeveloperModeEnabled = () => false;
    w.AppState.guardDeveloperModeAction = () => false;
    w.__detail = { id: 'item0', name: '串口', type: 1, enabled: true, implemented: true, params: {} };
    await w.AppState.loadPeripherals();
  });
  const row = page.locator('#peripheral-table-body tr').first();
  const diagnose = row.locator('[data-peripheral-action="diagnose"]');
  await expect(diagnose).toBeHidden();
  await row.locator('summary').click();
  await expect(diagnose).toBeVisible();
  await expect(diagnose).toBeEnabled();
  await expect(row.locator('[data-peripheral-action="edit"]')).toBeDisabled();
});

test('execution data options use the whole dialog width and preserve draft when managing history', async ({page}) => {
  const html = source('web-src/pages/modals.html');
  const start = html.indexOf('<div class="pe-rule-options"');
  const end = html.indexOf('<!-- 第二部分', start);
  await page.setContent('<style>' + source('web-src/css/main.css') + '</style><div style="width:min(900px,calc(100vw - 32px));margin:16px"><input id="draft" value="未保存的规则">' + html.slice(start, end) + '</div>');
  const options = page.locator('.pe-rule-options');
  await expect(options.locator('input[type="checkbox"]')).toHaveCount(3);
  await expect(options.locator('button')).toHaveCount(1);
  for (const width of [1440, 390]) {
    await page.setViewportSize({ width, height: 900 });
    const geometry = await options.evaluate(el => ({
      columns: getComputedStyle(el).gridTemplateColumns.split(' ').length,
      overflow: document.documentElement.scrollWidth > innerWidth,
      direct: el.children.length
    }));
    expect(geometry).toEqual({ columns: width === 1440 ? 2 : 1, overflow: false, direct: 4 });
  }
  await page.evaluate(() => {
    const w = window as any;
    w.__opened = [];
    w.open = (...args: any[]) => { w.__opened.push(args); };
    w.AppState._peModalBound = false;
    w.AppState._bindPeriphExecModalEvents();
  });
  await page.locator('#periph-exec-history-settings-link').click();
  await expect(page.locator('#draft')).toHaveValue('未保存的规则');
  expect(await page.evaluate(() => (window as any).__opened[0].slice(1))).toEqual(['_blank', 'noopener']);
});

for (const kind of ['peripheral','exec']) for (const clamp of [true,false]) {
  test(`${kind}: delete last page row 11 to 10 (${clamp ? 'server clamp' : 'older server fallback'})`, async ({page}) => {
    await page.evaluate(async ({kind,clamp}) => {
      const w = window as any, a = w.AppState; w.__clamp = clamp;
      if (kind === 'peripheral') { await a.loadPeripherals(); a._periphCurrentPage = 2; await a.loadPeripherals(); }
      else { await a.loadPeriphExecPage(); a._peCurrentPage = 2; await a.loadPeriphExecPage(); }
    }, {kind,clamp});
    const body = page.locator(kind === 'peripheral' ? '#peripheral-table-body' : '#periph-exec-table-body');
    await expect(body.locator('tr')).toHaveCount(1);
    await body.locator('[data-' + (kind === 'peripheral' ? 'peripheral' : 'pe') + '-action="delete"]').click();
    await expect(body.locator('tr')).toHaveCount(10);
    expect(await page.evaluate(k => (window as any).AppState[k === 'peripheral' ? '_periphCurrentPage' : '_peCurrentPage'],kind)).toBe(1);
    await expect(page.locator(kind === 'peripheral' ? '#periph-pagination' : '#periph-exec-pagination')).toBeHidden();
  });
}

for (const kind of ['peripheral','exec']) {
  test(`${kind}: filter all rows before paging, then refresh shrink synchronizes page`, async ({page}) => {
    await page.evaluate(async kind => {
      const a = (window as any).AppState;
      if (kind === 'peripheral') { await a.loadPeripherals(); a._periphCurrentPage=2; await a.loadPeripherals(); }
      else { await a.loadPeriphExecPage(); a._peCurrentPage=2; await a.loadPeriphExecPage(); }
    },kind);
    const filter = page.locator(kind === 'peripheral' ? '#peripheral-filter-type' : '#periph-exec-filter-periph');
    await filter.selectOption(kind === 'peripheral' ? 'communication' : 'trigger:1');
    // App's delegated data-change-action dispatcher is outside these source modules.
    if (kind === 'exec') await page.evaluate(() => (window as any).AppState.loadPeriphExecPage());
    const body = page.locator(kind === 'peripheral' ? '#peripheral-table-body' : '#periph-exec-table-body');
    await expect(body.locator('tr')).toHaveCount(1);
    await expect(body).toContainText('Item 10');
    await filter.selectOption('');
    await page.evaluate(async kind => {
      const w=window as any,a=w.AppState;
      if(kind==='peripheral'){await a.loadPeripherals();a._periphCurrentPage=2;await a.loadPeripherals();}
      else{await a.loadPeriphExecPage();a._peCurrentPage=2;await a.loadPeriphExecPage();}
      w.__rows.pop();
      if(kind==='peripheral')a._refreshPeripheralList();else a._refreshPeriphExecList();
    },kind);
    await expect(body.locator('tr')).toHaveCount(10);
    expect(await page.evaluate(k=>(window as any).AppState[k==='peripheral'?'_periphCurrentPage':'_peCurrentPage'],kind)).toBe(1);
  });
}

for (const type of [1,2]) test(`diagnostics type ${type} opens from detail without catalog`, async ({page}) => {
  await page.evaluate(async type => {
    const w=window as any; w.__detail={id:'test',name:'诊断设备',type,enabled:true,implemented:true,params:{}};
    await w.AppState.openPeripheralIoModal('test');
  },type);
  await expect(page.locator('#peripheral-io-error')).toBeHidden();
  await expect(page.locator(type===1?'#peripheral-uart-panel':'#peripheral-i2c-sensor-panel')).toBeVisible();
  expect(await page.evaluate(() => (window as any).__requests.some((r:string)=>r.includes('/status')))).toBe(true);
  if (type === 2) {
    await expect(page.locator('#peripheral-i2c-scan-btn')).toBeEnabled();
    await expect(page.locator('#peripheral-i2c-probe-btn')).toBeDisabled();
    await expect(page.locator('#peripheral-i2c-read-btn')).toBeDisabled();
    await expect(page.locator('#peripheral-i2c-release-btn')).toBeDisabled();
    await page.evaluate(async () => {
      const a = (window as any).AppState;
      await a.runPeripheralI2cOperation('read'); await a.runPeripheralI2cOperation('probe');
    });
    expect(await page.evaluate(() => (window as any).__requests.some((r:string)=>r.includes('/read')))).toBe(false);
  }
});

for (const reason of ['disabled','protocol.modbus']) test(`diagnostics ${reason} explains and blocks bus actions`, async ({page}) => {
  await page.evaluate(async reason => {
    const w=window as any;w.__detail={id:'rs485',name:'串口',type:1,enabled:reason!=='disabled',implemented:true,diagnosticBlockedReason:reason,params:{}};
    await w.AppState.openPeripheralIoModal('rs485');
    (document.getElementById('peripheral-uart-payload') as HTMLInputElement).value='test';
    w.AppState.updatePeripheralUartPayloadState();
    await w.AppState.sendPeripheralUart(); await w.AppState.refreshPeripheralIo(true);w.AppState._schedulePeripheralIoPolling();
  },reason);
  await expect(page.locator('#peripheral-io-error')).toContainText(reason==='disabled'?'外设已禁用':'Modbus');
  await expect(page.locator('#peripheral-uart-send-btn')).toBeDisabled();
  await expect(page.locator('#peripheral-uart-read-btn')).toBeDisabled();
  expect(await page.evaluate(() => (window as any).__requests)).toEqual(['/api/peripherals/?id=rs485']);
});
