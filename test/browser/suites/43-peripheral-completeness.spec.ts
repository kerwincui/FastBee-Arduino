import { test, expect } from '../fixtures/base.fixture';
import { enableOptionalUiFeatures } from '../fixtures/optional-features.fixture';
import { sensorDrivers } from '../generic-peripheral-fixtures.mjs';

test.beforeEach(async({authPage})=>{
  expect(new URL(authPage.url()).hostname).toBe('127.0.0.2');
  await enableOptionalUiFeatures(authPage,['deviceControl','ruleScript','modbus']);
});

test('every advertised configurable peripheral type opens an editable form without page errors',async({authPage,navigateTo})=>{
  const errors:string[]=[];authPage.on('pageerror',e=>errors.push(e.message));
  await navigateTo('peripheral');await authPage.locator('#add-peripheral-btn').click();
  const select=authPage.locator('#peripheral-type-input');await expect(select).toBeEnabled();
  const options=await select.locator('option').evaluateAll((items:any[])=>items.filter(e=>e.value!==''&&!e.disabled).map(e=>({value:e.value,name:e.textContent})));
  expect(options.length).toBeGreaterThan(8);
  for(const option of options){
    await select.selectOption(option.value);
    await expect(select).toHaveValue(option.value);
    await expect(authPage.locator('#save-peripheral-btn')).toBeVisible();
    expect(option.name.trim()).not.toBe('');
  }
  expect(errors).toEqual([]);
});

for(const driver of sensorDrivers) test('configured '+driver.name+' sensor persists and reopens with all editable driver values',async({authPage,navigateTo})=>{
  await navigateTo('peripheral');await authPage.locator('#add-peripheral-btn').click();
  const modal=authPage.locator('#peripheral-modal');await expect(modal).toBeVisible();
  await modal.locator('#peripheral-type-input').selectOption('38');
  const id='roundtrip_'+driver.name.toLowerCase();
  await modal.locator('#peripheral-id-input').fill(id);await modal.locator('#peripheral-name-input').fill(driver.name+' 回归');
  await modal.locator('#generic-sensor-driver').selectOption(driver.name.toLowerCase());
  await modal.locator('#generic-sensor-bus-id').selectOption('offline_i2c');
  if(driver.name==='INA226') {
    await modal.locator('#generic-driver-shuntResistance').fill('0.01');await modal.locator('#generic-driver-maxCurrent').fill('5');
  }
  await modal.locator('#save-peripheral-btn').click();await expect(modal).toHaveCount(0);
  await authPage.evaluate(id=>(window as any).AppState.openPeripheralModal(true,id),id);
  await expect(modal.locator('#generic-sensor-driver')).toHaveValue(driver.name.toLowerCase());
  await expect(modal.locator('#generic-sensor-bus-id')).toHaveValue('offline_i2c');
  if(driver.name==='MPU6050') {
    await expect(modal.locator('#generic-sensor-channel')).toHaveValue('accelX');
  }
  for(const param of (driver.params as any[]).filter(p=>!(driver.name==='MPU6050' && p.key==='mode'))) {
    await expect(modal.locator('#generic-driver-'+param.key)).toBeAttached();
    expect(await modal.locator('#generic-driver-'+param.key).inputValue()).not.toBe('');
  }
});

test('execution editor enumerates every supported action and preserves secondary actions',async({authPage,navigateTo})=>{
  await navigateTo('periph-exec');await authPage.locator('#periph-exec-page-add-btn').click();
  const modal=authPage.locator('#periph-exec-modal');await expect(modal.locator('.pe-action-type').first()).toBeAttached();
  const types=await modal.locator('.pe-action-type').first().locator('option').evaluateAll((items:any[])=>items.map(e=>Number(e.value)));
  expect(types).toContain(18);expect(types).toContain(19);expect(types).toContain(34);expect(types).not.toContain(33);
  const errors:string[]=[];authPage.on('pageerror',e=>errors.push(e.message));
  for(const type of types) {
    await authPage.evaluate(type=>{
      const app=(window as any).AppState;
      document.getElementById('periph-exec-actions')!.replaceChildren();
      app._createPeriphExecActionElement({actionType:type,actionValue:'0'},0);
    },type);
    await expect(modal.locator('.pe-action-type')).toHaveValue(String(type));
  }
  expect(errors).toEqual([]);
});

async function sensorRule(page:any){
  await page.locator('#periph-exec-page-add-btn').click();
  const modal=page.locator('#periph-exec-modal');
  await expect(modal.locator('.pe-trigger-type').first()).toBeVisible();
  await modal.locator('#periph-exec-name').fill('采集规则回归');
  await modal.locator('.pe-trigger-type').first().selectOption('1');
  await modal.locator('.pe-interval').first().fill('60');
  const block=modal.locator('#periph-exec-actions .periph-exec-config-item').first();
  await block.locator('.pe-action-type').selectOption('19');
  await block.locator('.pe-sensor-category').selectOption('configured');
  await block.locator('.pe-target-periph').selectOption('offline_ina');
  await block.locator('.pe-sensor-channel-field').selectOption('current');
  await block.locator('.pe-sensor-property').fill('current');
  return modal;
}

test('execution save is single-flight and rejected saves retain all user input',async({authPage,navigateTo})=>{
  await navigateTo('periph-exec');const modal=await sensorRule(authPage);
  const writes:any[]=[];let release:()=>void=()=>{};
  const ready=new Promise<void>(r=>release=r);
  await authPage.route('**/api/periph-exec',async route=>{
    if(route.request().method()==='GET')return route.continue();
    writes.push(route.request().postDataJSON());await ready;await route.fulfill({json:{success:false,error:'写入失败，请重试'}});
  });
  await authPage.evaluate(()=>{const a=(window as any).AppState;a.savePeriphExecRule();a.savePeriphExecRule();});
  try {await expect.poll(()=>writes.length).toBe(1);} finally {release();}
  await expect(modal.locator('#periph-exec-error')).toContainText('写入失败');
  await expect(modal.locator('#periph-exec-name')).toHaveValue('采集规则回归');
  await expect(modal.locator('#save-periph-exec-btn')).toBeEnabled();
  expect(writes[0].actions[0].targetPeriphId).toBe('offline_ina');
});

test('execution edit dependency failure cannot silently save a partial rule',async({authPage,navigateTo})=>{
  await navigateTo('periph-exec');
  await authPage.evaluate(()=>(window as any).apiInvalidateCache('/api/peripherals/types'));
  await authPage.route('**/api/peripherals/types*',r=>r.fulfill({json:{success:false,error:'类型目录不可用'}}));
  await authPage.locator('#periph-exec-page-add-btn').click();
  await expect(authPage.locator('#periph-exec-error')).toContainText(/加载|不可用/);
  await expect(authPage.locator('#save-periph-exec-btn')).toBeDisabled();
});

test('peripheral invalid pins and save rejection retain draft and unlock retry',async({authPage,navigateTo})=>{
  await navigateTo('peripheral');await authPage.locator('#add-peripheral-btn').click();
  await authPage.locator('#peripheral-type-input').selectOption('12');
  await authPage.locator('#peripheral-id-input').fill('output_test');
  await authPage.locator('#peripheral-name-input').fill('输出回归');
  await authPage.locator('#peripheral-pins-input').fill('999');
  await authPage.route('**/api/peripherals/preflight',r=>r.fulfill({json:{success:true,data:{valid:false,errors:['无效引脚'],warnings:[]}}}));
  const writes:any[]=[];
  await authPage.route('**/api/peripherals',async route=>{
    if(route.request().method()==='GET')return route.continue();writes.push(route.request().postData());await route.fulfill({json:{success:false,error:'外设保存失败'}});
  });
  await authPage.locator('#save-peripheral-btn').click();
  await expect(authPage.locator('#peripheral-modal')).toBeVisible();
  expect(writes).toEqual([]);
  await authPage.unroute('**/api/peripherals/preflight');
  await authPage.locator('#peripheral-pins-input').fill('15');
  await authPage.locator('#save-peripheral-btn').click();
  await expect.poll(()=>writes.length).toBe(1);
  await expect(authPage.locator('#peripheral-name-input')).toHaveValue('输出回归');
  await expect(authPage.locator('#save-peripheral-btn')).toBeEnabled();
});
