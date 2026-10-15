import { test, expect } from '../fixtures/base.fixture';
import { enableOptionalUiFeatures } from '../fixtures/optional-features.fixture';


test('old account save cannot close a newer user draft',async({authPage,navigateTo})=>{
 await navigateTo('users');await authPage.locator('#add-user-btn').click();
 await authPage.locator('#add-username-input').fill('old_operator');
 await authPage.locator('#add-password-input').fill('Abcdef12345');await authPage.locator('#add-confirm-password-input').fill('Abcdef12345');
 let release:()=>void=()=>{},writes=0;const gate=new Promise<void>(r=>release=r);
 await authPage.route('**/api/users',async route=>{if(route.request().method()==='GET')return route.continue();writes++;await gate;await route.fulfill({json:{success:true}});});
 await authPage.evaluate(()=>{const a=(window as any).AppState;(window as any).__userSave=a.addUser();a.addUser();});
 try{await expect.poll(()=>writes).toBe(1);await authPage.locator('#close-add-user-modal').click();await authPage.locator('#add-user-btn').click();await authPage.locator('#add-username-input').fill('new_operator');}finally{release();}
 await authPage.evaluate(()=>(window as any).__userSave);
 expect(writes).toBe(1);await expect(authPage.locator('#add-user-modal')).toBeVisible();await expect(authPage.locator('#add-username-input')).toHaveValue('new_operator');
 await expect(authPage.locator('#confirm-add-user-btn')).toBeEnabled();
});

test('saving password policy invalidates previously cached user requirements',async({authPage,navigateTo})=>{
 await navigateTo('users');await navigateTo('device');await expect(authPage.locator('#dev-id')).toHaveValue('OFFLINE-UI');
 await authPage.evaluate(()=>{const w=window as any;w.__invalidated=[];const old=w.apiInvalidateCache;w.apiInvalidateCache=(key:string)=>{w.__invalidated.push(key);return old(key);};});
 await authPage.locator('[data-tab=dev-advanced]').click();await authPage.locator('#dev-sec-min-pwd-len').fill('12');
 await authPage.route('**/api/device/config',async r=>{if(r.request().method()==='GET')return r.continue();await r.fulfill({json:{success:true}});});
 await authPage.locator('#device-security-form button[type=submit]').click();
 await expect.poll(()=>authPage.evaluate(()=>(window as any).__invalidated)).toContain('/api/users');
});

test.beforeEach(async ({authPage}) => {
  expect(new URL(authPage.url()).hostname).toBe('127.0.0.2');
  await enableOptionalUiFeatures(authPage, ['userAdmin','ruleScript']);
});

for (const policy of [{minLength:4,maxLength:64,requireStrong:false},{minLength:12,maxLength:64,requireStrong:true}]) {
 test('user form follows configured password policy '+policy.minLength,async({authPage,navigateTo})=>{
  const writes:string[]=[];
  await authPage.route('**/api/users**',async route=>{
   if(route.request().method()==='GET')return route.fulfill({json:{success:true,data:{users:[],passwordPolicy:policy}}});
   writes.push(route.request().postData() || '');return route.fulfill({json:{success:false,error:'离线保存拒绝'}});
  });
  await navigateTo('users');await expect(authPage.locator('#users-list-summary')).toContainText('0 个用户');
  await authPage.locator('#add-user-btn').click();
  await expect(authPage.locator('#add-password-input')).toHaveAttribute('minlength',String(policy.minLength));
  await authPage.locator('#add-username-input').fill('offline_operator');
  for(const value of [policy.requireStrong?'abcdefghijkl':'abc',policy.requireStrong?'Abcdefghij12':'abcd']){
   await authPage.locator('#add-password-input').fill(value);await authPage.locator('#add-confirm-password-input').fill(value);
   await authPage.evaluate(()=>(window as any).AppState.addUser());
   if(value==='abc'||value==='abcdefghijkl'){expect(writes.length).toBe(0);await expect(authPage.locator('#add-user-error')).toContainText(/密码/);}
  }
  await expect.poll(()=>writes.length).toBe(1);
  await expect(authPage.locator('#add-user-error')).toContainText('离线保存拒绝');
  await expect(authPage.locator('#confirm-add-user-btn')).toBeEnabled();
 });
}

test('failed network load blocks every save until successful explicit retry',async({authPage,navigateTo})=>{
 let failed=true;const writes:string[]=[];
 await authPage.route('**/api/network/config*',async route=>{
  if(route.request().method()!=='GET'){writes.push(route.request().method());return route.fulfill({json:{success:true}});}
  if(failed)return route.fulfill({json:{success:false,error:'离线读取失败'}});return route.continue();
 });
 await navigateTo('network');await expect(authPage.locator('#network-config-load-status')).toContainText('失败');
 for(const name of ['wifi','ap','advanced','ethernet','cellular'])await expect(authPage.locator('#'+name+'-form button[type=submit]')).toBeDisabled();
 await authPage.evaluate(()=>{const a=(window as any).AppState;for(const name of ['saveNetworkConfig','saveAPConfig','saveAdvancedConfig','saveEthernetConfig','saveCellularConfig'])if(typeof a[name]==='function')a[name]();});
 expect(writes).toEqual([]);failed=false;
 await authPage.locator('#network-config-load-status button').click();await expect(authPage.locator('#network-config-load-status')).toContainText('已加载');
 for(const name of ['wifi','ap','advanced','ethernet','cellular'])await expect(authPage.locator('#'+name+'-form button[type=submit]')).toBeEnabled();
});

async function outputDraft(page:any,id:string){
 await page.locator('#add-peripheral-btn').click();await page.locator('#peripheral-type-input').selectOption('12');
 await page.locator('#peripheral-id-input').fill(id);await page.locator('#peripheral-name-input').fill(id);await page.locator('#peripheral-pins-input').fill('15');
}

test('closed preflight cannot save or contaminate a newly opened peripheral editor',async({authPage,navigateTo})=>{
 await navigateTo('peripheral');await outputDraft(authPage,'old_draft');
 let release:()=>void=()=>{},calls=0;const gate=new Promise<void>(r=>release=r);const writes:string[]=[];
 await authPage.route('**/api/peripherals/preflight',async route=>{calls++;await gate;await route.fulfill({json:{success:true,data:{valid:false,errors:['OLD_PREFLIGHT_RESULT'],warnings:[]}}});});
 await authPage.route('**/api/peripherals',async route=>{if(route.request().method()==='GET')return route.continue();writes.push(route.request().postData()||'');await route.fulfill({json:{success:true}});});
 await authPage.evaluate(()=>{(window as any).__oldSave=(window as any).AppState.savePeripheralConfig();});
 try{await expect.poll(()=>calls).toBe(1);await authPage.locator('#close-peripheral-modal').click();await expect(authPage.locator('#peripheral-modal')).toBeHidden();await outputDraft(authPage,'new_draft');}finally{release();}
 await authPage.evaluate(()=> (window as any).__oldSave);
 expect(writes).toEqual([]);await expect(authPage.locator('#peripheral-modal')).toBeVisible();await expect(authPage.locator('#peripheral-name-input')).toHaveValue('new_draft');
 await expect(authPage.locator('#peripheral-modal')).not.toContainText('OLD_PREFLIGHT_RESULT');
});

test('old peripheral save success leaves the newer draft open and unchanged',async({authPage,navigateTo})=>{
 await navigateTo('peripheral');await outputDraft(authPage,'old_save');
 let release:()=>void=()=>{},calls=0;const gate=new Promise<void>(r=>release=r);
 await authPage.route('**/api/peripherals',async route=>{if(route.request().method()==='GET')return route.continue();calls++;await gate;await route.fulfill({json:{success:true}});});
 await authPage.evaluate(()=>{(window as any).__oldSave=(window as any).AppState.savePeripheralConfig();});
 try{await expect.poll(()=>calls).toBe(1);await authPage.locator('#close-peripheral-modal').click();await expect(authPage.locator('#peripheral-modal')).toBeHidden();await outputDraft(authPage,'new_save');}finally{release();}
 await authPage.evaluate(()=>(window as any).__oldSave);
 expect(calls).toBe(1);await expect(authPage.locator('#peripheral-modal')).toBeVisible();await expect(authPage.locator('#peripheral-name-input')).toHaveValue('new_save');
});
