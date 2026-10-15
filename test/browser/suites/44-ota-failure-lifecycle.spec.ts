import {test,expect} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
const root=resolve(__dirname,'../../..');
test.beforeEach(async({page})=>{
  const html=readFileSync(resolve(root,'web-src/pages/fragments/device-ota.html'),'utf8');
  await page.route('http://ota.test/**',r=>r.fulfill({contentType:'text/html',body:'<meta charset="utf-8">'+html}));
  await page.goto('http://ota.test/');
  await page.evaluate(()=>{
    const w=window as any;w.calls=[];w.notices=[];w.confirm=()=>true;w.FBConfirm=async()=>true;
    w.originalMessage=()=>42;
    w.AppState={currentPage:'device',_showMessage:w.originalMessage,registerModule(_n:string,m:any){Object.assign(this,m);},onPageHide(){}};
    w.Notification={success:(v:string)=>w.notices.push(v)};
    w.apiGet=w.apiGetFresh=async(url:string)=>({success:true,data:url.includes('capabilities')?{ota:true}:url.includes('network')?{connected:true}:url.includes('section=ota')?{otaBaseUrl:'https://firmware.example'}:{device:{firmwareVersion:'original'},flash:{freeSketchSpace:1048576}}});
    w.apiPost=async(url:string,body:any)=>{w.calls.push({url,body});return {success:true};};
  });
  await page.addScriptTag({path:resolve(root,'web-src/modules/runtime/device-ota.js')});
  await page.evaluate(()=>(window as any).AppState.setupDeviceOta());
  await expect(page.locator('#ota-base-url')).toHaveValue('https://firmware.example');
});

test('OTA setup preserves shared notification helper and binds save once',async({page})=>{
  await page.evaluate(()=>{const a=(window as any).AppState;a.setupDeviceOta();a.setupDeviceOta();});
  expect(await page.evaluate(()=>(window as any).AppState._showMessage===(window as any).originalMessage)).toBe(true);
  await page.locator('#ota-config-save-btn').click();
  expect(await page.evaluate(()=>(window as any).calls.length)).toBe(1);
});

for(const failure of ['http500','invalidUploadJson','invalidBeginJson']) test('local upload rejects '+failure+' without reporting completion',async({page})=>{
  await page.evaluate(failure=>{
    const w=window as any;w.fetch=async(url:string)=>{
      w.calls.push({url});const begin=url.endsWith('/begin');
      return {ok:failure!=='http500'||begin,status:failure==='http500'&&!begin?500:200,
        json:async()=>failure==='invalidBeginJson'&&begin||failure==='invalidUploadJson'&&!begin?{}:{success:true}};
    };
  },failure);
  await page.locator('#ota-file').setInputFiles({name:'firmware.bin',mimeType:'application/octet-stream',buffer:Buffer.from([1,2,3,4])});
  await page.evaluate(()=>(window as any).AppState.startLocalUpload());
  await expect(page.locator('#ota-upload-message')).toContainText('上传失败');
  expect(await page.evaluate(()=>(window as any).notices)).toEqual([]);
  expect(await page.evaluate(()=>(window as any).calls.length)).toBe(failure==='invalidBeginJson'?1:2);
  expect(await page.evaluate(()=>(window as any).AppState._otaBusy)).toBe(false);
});

test('navigation during begin response parsing cannot upload firmware later',async({page})=>{
  await page.evaluate(()=>{const w=window as any;w.fetch=async(url:string)=>{w.calls.push({url});return {ok:true,json:()=>new Promise(r=>w.beginDone=r)};};});
  await page.locator('#ota-file').setInputFiles({name:'firmware.bin',mimeType:'application/octet-stream',buffer:Buffer.from([1])});
  await page.evaluate(()=>{(window as any).AppState.startLocalUpload();});
  await expect.poll(()=>page.evaluate(()=>typeof (window as any).beginDone)).toBe('function');
  await page.evaluate(()=>{const w=window as any;w.AppState.currentPage='network';w.AppState._hideDeviceOta();w.beginDone({success:true});});
  expect(await page.evaluate(()=>(window as any).calls.length)).toBe(1);
  expect(await page.evaluate(()=>(window as any).AppState._otaPollTimer)).toBeNull();
});

test('late URL acceptance after navigation cannot restart polling',async({page})=>{
  await page.locator('#ota-url').fill('https://firmware.example/app.bin');
  await page.evaluate(()=>{const w=window as any;w.apiPost=()=>new Promise(r=>w.urlDone=r);w.AppState.startOnlineUpgrade();});
  await expect.poll(()=>page.evaluate(()=>typeof (window as any).urlDone)).toBe('function');
  await page.evaluate(()=>{const w=window as any;w.AppState.currentPage='network';w.AppState._hideDeviceOta();w.urlDone({success:true});});
  expect(await page.evaluate(()=>(window as any).AppState._otaPollTimer)).toBeNull();
});

test('slow polling stays single-flight and stops at the wall clock deadline',async({page})=>{
  await page.clock.install();
  await page.evaluate(()=>{const w=window as any;w.fetch=(url:string)=>{w.calls.push({url});return new Promise(()=>{});};w.AppState._startPolling();});
  await page.clock.runFor(12000);
  expect(await page.evaluate(()=>(window as any).calls.length)).toBe(1);
  await page.clock.fastForward(600000);
  await expect(page.locator('#ota-online-message')).toContainText('轮询超时');
  expect(await page.evaluate(()=>(window as any).AppState._otaPollTimer)).toBeNull();
  expect(await page.evaluate(()=>(window as any).AppState._otaBusy)).toBe(false);
});

test('progress followed by disconnect never becomes confirmed upgrade success',async({page})=>{
  await page.clock.install();
  await page.evaluate(()=>{const w=window as any;w.fetch=async(url:string)=>{w.calls.push({url});if(w.calls.length===1)return {ok:true,json:async()=>({success:true,data:{in_progress:true,progress:25}})};throw new TypeError('offline');};w.AppState._startPolling();});
  for(let i=0;i<7;i++)await page.clock.runFor(1000);
  await expect(page.locator('#ota-online-message')).toContainText('结果尚未确认');
  expect(await page.evaluate(()=>(window as any).notices)).toEqual([]);
  await page.evaluate(()=>{const w=window as any;w.AppState.currentPage='network';w.AppState._hideDeviceOta();});
  const count=await page.evaluate(()=>(window as any).calls.length);
  await page.clock.runFor(10000);
  expect(await page.evaluate(()=>(window as any).calls.length)).toBe(count);
});

