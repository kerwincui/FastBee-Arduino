import { test, expect } from '../fixtures/base.fixture';
import { enableOptionalUiFeatures } from '../fixtures/optional-features.fixture';
import type { Route } from '@playwright/test';

const payload = (route: Route) => {
  const text = route.request().postData() || '';
  return text.startsWith('{') ? JSON.parse(text) : Object.fromEntries(new URLSearchParams(text));
};
test.beforeEach(async ({ authPage }) => {
  expect(new URL(authPage.url()).hostname).toBe('127.0.0.2');
  await enableOptionalUiFeatures(authPage, ['deviceControl', 'ruleScript', 'logViewer', 'userAdmin', 'modbus', 'mqtt']);
});

test('all eleven menus remain usable after repeated navigation without page errors', async ({authPage, navigateTo}) => {
  const errors: string[]=[];
  authPage.on('pageerror', e=>errors.push(e.message));
  const menus=['dashboard','device','network','peripheral','periph-exec','protocol','device-control','rule-script','logs','users','features'];
  for (let round=0;round<3;round++) for(const menu of menus) {
    await navigateTo(menu);
    await expect(authPage.locator('#'+menu+'-page')).toBeVisible();
    expect(await authPage.locator('.page.active').count()).toBe(1);
    expect(await authPage.locator('.modal.show').count()).toBe(0);
  }
  expect(errors).toEqual([]);
});

test('device tabs save independently and remain available after OTA module loads', async ({authPage,navigateTo})=>{
  await navigateTo('device');
  await expect(authPage.locator('#dev-id')).toHaveValue('OFFLINE-UI');
  const writes:any[]=[];
  await authPage.route('**/api/device/config*', async route=>{
    if(route.request().method()==='GET') return route.continue();
    writes.push(payload(route));await route.fulfill({json:{success:true}});
  });
  await authPage.locator('#dev-name').fill('离线配置验证');
  await authPage.locator('#device-basic-form button[type=submit]').click();
  await expect.poll(()=>writes.length).toBe(1);
  expect(Object.keys(writes[0]).sort()).toEqual(['description','deviceId','deviceName','logLevel','productNumber','userId'].sort());
  await authPage.locator('[data-tab=dev-ntp]').click();
  await authPage.locator('#dev-ntp-enable').selectOption('0');
  await authPage.locator('#dev-sync-interval').fill('600');
  await authPage.locator('#device-ntp-form button[type=submit]').click();
  await expect.poll(()=>writes.length).toBe(2);
  expect(writes[1]).toMatchObject({syncInterval:600,enableNTP:false});
  await authPage.locator('[data-tab=dev-ota]').click();
  await expect(authPage.locator('#ota-upload-form')).toBeAttached();
  await authPage.locator('[data-tab=dev-advanced]').click();
  await authPage.locator('#dev-sec-max-attempts').fill('3');
  await authPage.locator('#dev-sec-lockout-time').fill('120');
  await authPage.locator('#dev-sec-min-pwd-len').fill('8');
  await authPage.locator('#device-security-form button[type=submit]').click();
  await expect.poll(()=>writes.length).toBe(3);
  expect(writes[2]).toMatchObject({maxLoginAttempts:3,loginLockoutTime:120000,minPasswordLength:8});
  await authPage.locator('[data-action=saveCacheDuration]').click();
  await expect.poll(()=>writes.length).toBe(4);
  expect(Object.keys(writes[3])).toEqual(['cacheDuration']);
});

for(const form of ['wifi','ap','advanced','ethernet','cellular']) test('network '+form+' saves its fields and recovers from rejected save',async({authPage,navigateTo})=>{
  await navigateTo('network');
  await expect(authPage.locator('#network-config-load-status')).toContainText('已加载');
  const writes:any[]=[];
  let fail=true;
  await authPage.route('**/api/network/config*',async route=>{
    if(route.request().method()==='GET') return route.continue();
    writes.push(payload(route));await route.fulfill({json:fail?{success:false,error:'离线注入：保存失败'}:{success:true,data:{restartRequired:false}}});
  });
  await authPage.evaluate((form)=>{
    const values:Record<string,string>={'wifi-ssid':'test-network','wifi-password':'test-pass-123','ap-ssid':'test-hotspot','ap-password':'test-pass-123','ap-ip':'192.168.4.1','cell-apn':'test.apn'};
    for(const [id,value] of Object.entries(values)) (document.getElementById(id) as HTMLInputElement).value=value;
    document.getElementById(form+'-form')!.dispatchEvent(new Event('submit',{bubbles:true,cancelable:true}));
  },form);
  await expect.poll(()=>writes.length).toBe(1);
  await expect(authPage.locator('#'+form+'-form button[type=submit]')).toBeEnabled();
  expect(await authPage.locator('body').innerText()).toContain('保存失败');
  fail=false;
  await authPage.evaluate((form)=>document.getElementById(form+'-form')!.dispatchEvent(new Event('submit',{bubbles:true,cancelable:true})),form);
  await expect.poll(()=>writes.length).toBe(2);
  expect(writes[1]).toEqual(writes[0]);
  const field={wifi:'staSSID',ap:'apSSID',advanced:'staticIP',ethernet:'networkType',cellular:'cellular.apn'}[form]!;
  expect(writes[1]).toHaveProperty(field);
});

test('script create edit disable delete preserves content and displays failed saves',async({authPage,navigateTo})=>{
  const rules:any[]=[];const writes:any[]=[];let fail=true,reads=0;
  await authPage.route('**/api/rule-script**',async route=>{
    const method=route.request().method(),url=new URL(route.request().url());
    if(method==='GET') {reads++;return route.fulfill({json:{success:true,data:rules}});}
    const body=payload(route);writes.push({path:url.pathname,body});
    if(fail)return route.fulfill({json:{success:false,error:'保存未落盘'}});
    if(url.pathname.endsWith('disable')) rules[0].enabled=false;
    else if(method==='DELETE') rules.length=0;
    else if(url.pathname.endsWith('update'))Object.assign(rules[0],body,{enabled:body.enabled==='1'});
    else rules.push({...body,id:'offline-script',enabled:true});
    return route.fulfill({json:{success:true}});
  });
  await navigateTo('rule-script');
  await authPage.locator('[data-action=openRuleScriptModal]').click();
  await authPage.locator('#rule-script-name').fill('规则回归');
  await authPage.locator('#rule-script-content').fill('payload.value = 42;');
  const initialReads=reads;
  await authPage.locator('#save-rule-script-btn').click();
  await expect(authPage.locator('#rule-script-error')).toContainText('保存未落盘');
  await expect(authPage.locator('#rule-script-content')).toHaveValue('payload.value = 42;');
  await expect(authPage.locator('#rule-script-error')).toContainText('核验列表');
  await expect(authPage.locator('#save-rule-script-btn')).toBeDisabled();
  await expect.poll(()=>reads).toBeGreaterThan(initialReads);
  // An unconfirmed write must not be repeated from the same editor session,
  // even when invoked directly rather than through the disabled button.
  await authPage.evaluate(()=>(window as any).AppState.saveRuleScript());
  expect(writes).toHaveLength(1);
  expect(rules).toHaveLength(0);
  await authPage.locator('#close-rule-script-modal').click();
  await expect(authPage.locator('#rule-script-modal')).toHaveCount(0);
  fail=false;
  await authPage.locator('[data-action=openRuleScriptModal]').click();
  await expect(authPage.locator('#save-rule-script-btn')).toBeEnabled();
  await authPage.locator('#rule-script-name').fill('规则回归');
  await authPage.locator('#rule-script-content').fill('payload.value = 42;');
  await authPage.locator('#save-rule-script-btn').click();
  await expect(authPage.locator('#rule-script-modal')).toHaveCount(0);
  await expect(authPage.locator('#rule-script-table-body')).toContainText('规则回归');
  await authPage.locator('#rule-script-table-body button[data-rule-script-action=edit]').first().click();
  await expect(authPage.locator('#rule-script-content')).toHaveValue('payload.value = 42;');
  await authPage.locator('#rule-script-content').fill('payload.value = 0;');
  await authPage.locator('#save-rule-script-btn').click();
  await expect.poll(()=>rules[0]?.scriptContent).toBe('payload.value = 0;');
  await authPage.evaluate(()=>(window as any).AppState.toggleRuleScript('offline-script',false));
  await expect.poll(()=>rules[0]?.enabled).toBe(false);
  await authPage.evaluate(()=>(window as any).AppState.deleteRuleScript('offline-script'));
  await expect.poll(()=>rules.length).toBe(0);
  expect(writes.length).toBe(5);
});

test('users create edit disable unlock delete reports errors and keeps disabled state',async({authPage,navigateTo})=>{
  const users:any[]=[];const writes:any[]=[];let fail=true;
  await authPage.route('**/api/users**',async route=>{
    const method=route.request().method(),url=new URL(route.request().url());
    if(method==='GET')return route.fulfill({json:{success:true,data:{users}}});
    const body=payload(route);writes.push({path:url.pathname,body});
    if(fail)return route.fulfill({json:{success:false,error:'用户保存失败'}});
    if(method==='DELETE')users.length=0;
    else if(url.pathname.endsWith('unlock-account')) users[0].isLocked=false;
    else if(url.pathname.endsWith('update'))Object.assign(users[0],body,{enabled:body.enabled==='true'});
    else users.push({...body,enabled:true,isLocked:false});
    return route.fulfill({json:{success:true}});
  });
  await navigateTo('users');await authPage.locator('#add-user-btn').click();
  await authPage.locator('#add-username-input').fill('operator');
  await authPage.locator('#add-password-input').fill('test-pass-123');
  await authPage.locator('#add-confirm-password-input').fill('test-pass-123');
  await authPage.locator('#confirm-add-user-btn').click();
  await expect(authPage.locator('#add-user-error')).toContainText('用户保存失败');
  await expect(authPage.locator('#confirm-add-user-btn')).toBeEnabled();fail=false;
  await authPage.locator('#confirm-add-user-btn').click();
  await expect.poll(()=>users.length).toBe(1);
  await expect(authPage.locator('#users-table-body')).toContainText('operator');
  await authPage.evaluate(()=>(window as any).AppState.toggleUserStatus('operator',false));
  await expect.poll(()=>users[0].enabled).toBe(false);
  await authPage.locator('#users-table-body button').filter({hasText:'修改密码'}).click();
  await authPage.locator('#add-description-input').fill('保留密码且停用');
  await authPage.locator('#confirm-add-user-btn').click();
  await expect.poll(()=>users[0].description).toBe('保留密码且停用');
  expect(writes.at(-1).body).not.toHaveProperty('password');expect(users[0].enabled).toBe(false);
  await authPage.evaluate(()=>(window as any).AppState.unlockUser('operator'));
  await expect.poll(()=>writes.at(-1).path).toContain('unlock-account');
  await authPage.evaluate(()=>(window as any).AppState.deleteUser('operator'));
  await expect.poll(()=>users.length).toBe(0);
});

test('log selection refresh clear and failed loads recover without replacing errors with empty success',async({authPage,navigateTo})=>{
  let fail=false,cleared=false;
  await authPage.route('**/api/logs**',async route=>{
    const url=new URL(route.request().url());
    if(url.pathname.endsWith('/clear')){cleared=true;return route.fulfill({json:{success:true}});}
    if(fail)return route.fulfill({json:{success:false,error:'日志读取失败'}});
    if(url.pathname.endsWith('/list'))return route.fulfill({json:{success:true,data:[{name:'system.log',size:23},{name:'boot.log',size:13}]}});
    return route.fulfill({json:{success:true,data:{content:cleared?'':url.searchParams.get('file')+' 测试日志',size:23}}});
  });
  await navigateTo('logs');await expect(authPage.locator('#log-content')).toContainText('system.log');
  await authPage.locator('#log-file-select').selectOption('boot.log');await expect(authPage.locator('#log-content')).toContainText('boot.log');
  fail=true;await authPage.locator('#refresh-logs-btn').click();await expect(authPage.locator('#log-content')).toContainText('加载失败');
  fail=false;await authPage.locator('#refresh-logs-btn').click();await expect(authPage.locator('#log-content')).toContainText('boot.log');
  await authPage.locator('#clear-logs-btn').click();await expect.poll(()=>cleared).toBe(true);await expect(authPage.locator('#log-content')).toContainText('暂无日志');
});

test('feature save posts JSON, preserves dirty choices on failure, and enforces locked features',async({authPage,navigateTo})=>{
  await navigateTo('features');const writes:any[]=[];
  await authPage.route('**/api/features/apply',async route=>{writes.push({body:payload(route),type:route.request().headers()['content-type']});await route.fulfill({json:{success:false,error:'预算不足，配置未改变'}});});
  await expect(authPage.locator('[data-feature-toggle=webServer]')).toBeDisabled();
  const toggle=authPage.locator('[data-feature-toggle=mdns]');await expect(toggle).toBeEnabled();await authPage.locator('[data-feature-key=mdns] .toggle-slider').click();await expect(toggle).not.toBeChecked();
  await authPage.locator('#features-save-btn').click();await expect.poll(()=>writes.length).toBe(1);
  expect(writes[0].type).toContain('application/json');expect(writes[0].body.features.mdns.enabled).toBe(false);
  await expect(authPage.locator('#features-save-btn')).toBeEnabled();await expect(toggle).not.toBeChecked();
  await expect(authPage.locator('body')).toContainText('预算不足');
});

test('MQTT configuration supports validation and rejected-save retry without touching OTA base',async({authPage,navigateTo})=>{
  await navigateTo('protocol');const writes:any[]=[];let fail=true;
  await authPage.route('**/api/protocol/mqtt/config',async route=>{
    if(route.request().method()==='GET')return route.continue();writes.push(payload(route));await route.fulfill({json:fail?{success:false,error:'协议保存失败'}:{success:true}});
  });
  await expect(authPage.locator('#mqtt-broker')).toBeAttached();await authPage.locator('#mqtt-broker').fill('broker.example');
  await authPage.locator('#mqtt-client-id').fill('offline-client');await authPage.locator('#mqtt-port').fill('1883');
  await authPage.locator('#mqtt-form button[type=submit]').click();await expect.poll(()=>writes.length).toBe(1);
  await expect(authPage.locator('#mqtt-form button[type=submit]')).toBeEnabled();
  fail=false;await authPage.locator('#mqtt-form button[type=submit]').click();await expect.poll(()=>writes.length).toBe(2);
  expect(writes[1]).toEqual(writes[0]);expect(writes[1]).not.toHaveProperty('mqtt_otaBaseUrl');
});
