import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
const root = resolve(__dirname, '../../..');
const source = (file: string) => readFileSync(resolve(root, file), 'utf8');

test.beforeEach(async ({ page }) => {
  await page.route('http://config.test/**', route => route.fulfill({contentType: 'text/html', body: '<meta charset="utf-8">' + source('web-src/pages/device.html')}));
  await page.goto('http://config.test/');
  await page.evaluate(() => {
    const w = window as any;
    w.__writes = []; w.__notices = []; w.__hides = {};
    w.__config = { deviceId: 'COM15', deviceName: '采集器', productNumber: 123, userId: 'owner', description: '原值', ntpServer1: 'time.example', ntpServer2: 'backup.example', timezone: 'CST-8', enableNTP: false, syncInterval: 7200, cacheDuration: 86400, logLevel: 'INFO', maxLoginAttempts: 5, loginLockoutTime: 300000, minPasswordLength: 8, requireStrongPasswords: true };
    w.AppState = { currentPage: 'device', registerModule(_n: string, m: any) { Object.assign(this, m); }, onPageHide(n: string, fn: any) { w.__hides[n] = fn; }, _setValue(id: string, value: string) { const e = document.getElementById(id) as HTMLInputElement; if(e) e.value = value; }, _showMessage() {}, setDeveloperModeState(v: boolean) { this.devMode = v; }, isDeveloperModeEnabled() { return this.devMode; } };
    w.Notification = Object.fromEntries(['success','error','warning','info'].map(k => [k, (message: string) => w.__notices.push([k, message])]));
    w.confirm = () => true;
    w.FBConfirm = async () => true;
    w.apiGet = w.apiGetFresh = async (url: string) => ({success: true, data: url === '/api/device/config' ? w.__config : url.includes('capabilities') ? {ota: true} : url.includes('network') ? {internetAvailable: false} : {}});
    w.apiPut = w.apiPost = async (url: string, body: object) => { w.__writes.push({url, body}); return {success: true}; };
    w.apiRestart = (body: object) => w.apiPost('/restart', body); w.apiFactoryReset = (body: object) => w.apiPost('/reset', body);
  });
  await page.addScriptTag({ content: source('web-src/modules/runtime/device-config.js') });
  await page.evaluate(() => (window as any).AppState.loadDeviceConfig({deferHardware: false}));
  await expect(page.locator('#dev-id')).toHaveValue('COM15');
});

test('basic save patches only its own fields and duplicate bindings submit once', async ({page}) => {
  await page.locator('#dev-name').fill('新名称');
  await page.evaluate(() => { const a=(window as any).AppState; a.setupDeviceConfigEvents(); a.setupDeviceConfigEvents(); document.getElementById('device-basic-form')!.dispatchEvent(new Event('submit', {cancelable:true})); });
  await expect.poll(() => page.evaluate(() => (window as any).__writes.length)).toBe(1);
  expect(await page.evaluate(() => (window as any).__writes[0].body)).toEqual({deviceId:'COM15',deviceName:'新名称',productNumber:123,userId:'owner',description:'原值',logLevel:'INFO'});
});

test('failed load blocks saving defaults, preserves entries, and allows retry', async ({page}) => {
  await page.evaluate(async () => { const w=window as any; w.apiGet = async () => ({success:false,error:'HTTP 503'}); await w.AppState.loadDeviceConfig(); await w.AppState.saveDeviceBasic(); });
  expect(await page.evaluate(() => (window as any).__writes)).toEqual([]);
  await expect(page.locator('#dev-name')).toHaveValue('采集器');
  expect(await page.evaluate(() => (window as any).__notices)).toContainEqual(['warning', '加载设备配置失败：HTTP 503，请刷新页面重试']);
  await page.evaluate(async () => {const w=window as any; w.apiGet=async()=>({success:true,data:w.__config}); await w.AppState.loadDeviceConfig(); await w.AppState.saveDeviceBasic();});
  await expect.poll(() => page.evaluate(() => (window as any).__writes.length)).toBe(1);
});

test('pending save locks repeated calls and failures retain entered values for retry', async ({page}) => {
  await page.evaluate(() => { const w=window as any; w.apiPut=(url:string,body:object)=>{w.__writes.push({url,body});return new Promise(r=>w.__done=r);}; w.AppState.saveDeviceBasic(); w.AppState.saveDeviceBasic(); });
  expect(await page.evaluate(() => (window as any).__writes.length)).toBe(1);
  await page.evaluate(()=>(window as any).__done({success:false,error:'storage full'}));
  await expect(page.locator('#device-basic-form button[type=submit]')).toBeEnabled();
  expect(await page.evaluate(() => (window as any).__notices.at(-1))).toEqual(['error','storage full']);
});

test('invalid numbers cannot be silently truncated or saved', async ({page}) => {
  for (const [id, value, method] of [['dev-product-number','1.5','saveDeviceBasic'],['dev-sync-interval','2','saveDeviceNTP'],['dev-sec-max-attempts','256','saveSecurityPolicy'],['dev-sec-lockout-time','-1','saveSecurityPolicy'],['dev-sec-min-pwd-len','100','saveSecurityPolicy']]) {
    await page.evaluate(({id,value,method})=>{(document.getElementById(id) as HTMLInputElement).value=value; (window as any).AppState[method]();},{id,value,method});
  }
  expect(await page.evaluate(()=>(window as any).__writes)).toEqual([]);
});

test('responses after leaving device page cannot replace form or send follow-up requests', async ({page}) => {
  await page.evaluate(()=>{const w=window as any; w.apiGet=()=>new Promise(r=>w.__loadDone=r); w.AppState.loadDeviceConfig(); w.AppState.currentPage='network'; Object.values(w.__hides).forEach((f:any)=>f.call(w.AppState)); w.__loadDone({success:true,data:{deviceName:'stale'}});});
  await expect(page.locator('#dev-name')).toHaveValue('采集器');
});

for (const action of ['restartDevice','factoryReset']) test(action+' disconnect is indeterminate, never success', async ({page}) => {
  await page.evaluate(async (action)=>{ const w=window as any; (document.getElementById('dev-factory-confirm') as HTMLInputElement).value='RESET'; (document.getElementById('dev-factory-password') as HTMLInputElement).value='secret'; w.apiRestart=w.apiFactoryReset=()=>Promise.reject(new TypeError('Failed to fetch')); await w.AppState[action](); },action);
  await expect.poll(()=>page.evaluate(()=>(window as any).__notices.length)).toBeGreaterThan(0);
  expect(await page.evaluate(()=>(window as any).__notices.some((n:any)=>n[0]==='success'))).toBe(false);
  expect(await page.evaluate(()=>(window as any).__notices.at(-1)[0])).toBe('warning');
});
