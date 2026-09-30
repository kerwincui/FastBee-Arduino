import {test,expect} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
const root=resolve(__dirname,'../../..');
const sourceRoot=process.env.COM15_RECOVERY_SOURCE_ROOT||root;
const fixture=JSON.parse(readFileSync(resolve(root,'test/browser/fixtures/com15-monitor-cache.json'),'utf8'));

async function setup(page:any){
 await page.clock.install();
 await page.setContent('<div id="device-control-page" class="active"><div id="dc-content"></div></div>');
 await page.evaluate(()=>{const w=window as any;w.AppState={};w.escapeHtml=(x:any)=>String(x??'');});
 for(const file of ['core','render','modbus-ops','modbus-ctrl'])
  await page.addScriptTag({path:resolve(sourceRoot,'web-src/modules/runtime/device-control/'+file+'.js')});
 await page.evaluate((fixture:any)=>{
  const w=window as any,a=w.AppState;w.calls=[];w.failControls=false;w.failRelay=false;
  a.currentPage='device-control';a._featureActive={deviceControl:true};a._eventsBound=true;
  a._dcCancelInit=()=>{};a._ensureDeviceControlRuntime=()=>Promise.resolve();a._fetchDeviceInfo=()=>Promise.resolve();
  a._setContentState=(el:any,state:any,message:any)=>{el.dataset.state=state;el.textContent=message||state;};
  a._setDeviceControlRefreshLoading=()=>{};
  a._dcApplyZoom=a._dcApplyLayout=a._dcInitFreeLayout=a._dcInitModbusDeviceStates=a._setupSSE=a._closeSSE=()=>{};
  a._renderControlPanel=(data:any)=>a._renderMonitorDataCard(data);
  w.apiGet=w.apiGetFresh=w.apiGetSilent=w.apiGetSilentFresh=async(path:string)=>{
   w.calls.push(path);
   if(path==='/api/periph-exec/controls'){
    if(w.failControls)throw Error('simulated transport timeout');
    return {success:true,data:structuredClone(fixture.controls)};
   }
   if(path.startsWith('/api/modbus/status'))return {success:true,data:structuredClone(fixture.status)};
   if(path==='/api/modbus/coil/status'){
    if(w.failRelay)throw {status:503};
    return {success:true,data:{states:[true,false,true,false]}};
   }
   return {success:true,data:{modbusRtu:{enabled:true,master:{devices:[]}}}};
  };
 },fixture);
 await page.evaluate(async()=>{await (window as any).AppState.loadDeviceControlPage();});
}

test('failed refresh retains card nodes as invalid and automatically restores a fresh snapshot',async({page})=>{
 await setup(page);await expect(page.locator('.dc-monitor-card')).toHaveCount(9);
 await page.evaluate(()=>{const w=window as any;w.firstCard=document.querySelector('.dc-monitor-card');w.failControls=true;});
 await page.evaluate(async()=>{await (window as any).AppState.loadDeviceControlPage({noCache:true});});
 await expect(page.locator('.dc-monitor-card')).toHaveCount(9);
 expect(await page.evaluate(()=>(window as any).firstCard===document.querySelector('.dc-monitor-card'))).toBe(true);
 await expect(page.locator('.dc-monitor-card[data-measurement-valid="true"]')).toHaveCount(0);
 await expect(page.locator('.dc-recovery-notice')).toContainText('通信失败');
 await page.evaluate(()=>(window as any).failControls=false);
 await page.clock.runFor(2500);
 await expect(page.locator('.dc-recovery-notice')).toHaveCount(0);
 await expect(page.locator('.dc-monitor-card')).toHaveCount(9);
 expect(await page.locator('.dc-monitor-card[data-measurement-valid="true"]').count()).toBeGreaterThan(0);
});

test('relay busy retries once per schedule and restores four actual readback states',async({page})=>{
 await setup(page);
 await page.evaluate(async()=>{
  const w=window as any,a=w.AppState;
  a._modbusDevices=[{slaveAddress:6,channelCount:4,controlProtocol:1,ncMode:true}];
  a._dcUpdateAllCoilUI=a._dcUpdatePanelOnlineState=()=>{};
  w.failRelay=true;await a._dcRefreshCoilStatus(0);
  a._dcScheduleCoilRetry(0);a._dcScheduleCoilRetry(0);
 });
 expect(await page.evaluate(()=>(window as any).AppState._dcCoilStates[0])).toEqual([null,null,null,null]);
 expect(await page.evaluate(()=>Object.keys((window as any).AppState._dcAutoRefreshTimers).filter(x=>x.startsWith('coilRetry')).length)).toBe(1);
 await page.evaluate(()=>(window as any).failRelay=false);await page.clock.runFor(2100);
 expect(await page.evaluate(()=>(window as any).AppState._dcCoilStates[0])).toEqual([true,false,true,false]);
 expect(await page.evaluate(()=>(window as any).calls.filter((x:string)=>x==='/api/modbus/coil/status').length)).toBe(2);
});

test('leaving the page cancels pending recovery and does not issue more reads',async({page})=>{
 await setup(page);await page.evaluate(async()=>{const w=window as any;w.failControls=true;await w.AppState.loadDeviceControlPage();w.AppState.currentPage='other';w.AppState._dcStopAllAutoRefresh();w.before=w.calls.length;});
 await page.clock.runFor(120000);
 expect(await page.evaluate(()=>(window as any).calls.length)).toBe(await page.evaluate(()=>(window as any).before));
});

test('refresh during an old relay read obtains a new lifecycle readback',async({page})=>{
 await setup(page);
 await page.evaluate(async()=>{
  const w=window as any,a=w.AppState;
  a._modbusDevices=[{slaveAddress:6,channelCount:4,controlProtocol:1,ncMode:true}];
  a._dcUpdateAllCoilUI=a._dcUpdatePanelOnlineState=()=>{};
  const get=w.apiGetSilent;
  w.relayReadCount=0;
  w.apiGetSilent=(path:string,...args:any[])=>{
   if(path==='/api/modbus/coil/status'){
    w.relayReadCount++;
    if(w.relayReadCount===1)return new Promise(resolve=>{w.finishOldRelay=resolve;});
   }
   return get(path,...args);
  };
  w.oldRead=a._dcRefreshCoilStatus(0);await Promise.resolve();
  await a.loadDeviceControlPage({noCache:true});
  a._modbusDevices=[{slaveAddress:6,channelCount:4,controlProtocol:1,ncMode:true}];
  w.newRead=a._dcRefreshCoilStatus(0);
  w.finishOldRelay({success:true,data:{states:[false,false,false,false]}});
  await Promise.all([w.oldRead,w.newRead]);
 });
 expect(await page.evaluate(()=>(window as any).relayReadCount)).toBe(2);
 expect(await page.evaluate(()=>(window as any).AppState._dcCoilStates[0])).toEqual([true,false,true,false]);
});
