import { test, expect } from '@playwright/test';
import path from 'node:path';
const runtime=process.env.COM15_RECOVERY_SOURCE_ROOT?path.resolve(process.env.COM15_RECOVERY_SOURCE_ROOT,'web-src/modules/runtime'):path.resolve(__dirname,'../../../web-src/modules/runtime');
async function setup(page:any,nc=true,protocol=1){
 await page.setContent('<div class="dc-modbus-device-panel" data-dev-idx="0"><div class="dc-card-header"></div><div id="dc-coil-grid-0"><div class="dc-coil-card" data-dev="0" data-ch="0"><span class="dc-coil-st"></span></div><div class="dc-coil-card" data-dev="0" data-ch="1"><span class="dc-coil-st"></span></div></div></div><div id="coil-status-grid"></div>');
 await page.evaluate(({nc,protocol}:any)=>{
  const w=window as any;w.requests=[];w.debug=[];w.Notification={error:()=>{},success:()=>{}};
  w.response={success:true,data:{states:[false,true]}};
  w.apiGetSilent=async(url:any,params:any)=>{w.requests.push({url,params});if(w.rejectObject)throw w.rejectObject;if(w.reject)throw Error('offline');return w.response;};
  w.AppState={_modbusDevices:[{slaveAddress:6,channelCount:2,controlProtocol:protocol,ncMode:nc}],_activeDeviceIdx:0,_dcCoilStates:{},_dcDeviceOnline:{},_coilStates:[],_deviceCoilCache:{},_dcLifecycleGeneration:1,_dcIsLifecycleActive:()=>true,
   _markCtrlModalOnline:()=>{},_stopAutoRefreshOnErrors:()=>{},_setCtrlModalOffline:()=>{},_appendDebugLog:(_:any,title:any)=>w.debug.push(title),_appendDebugError:()=>{}};
 },{nc,protocol});
 await page.addScriptTag({path:path.join(runtime,'device-control/modbus-ctrl.js')});
 await page.addScriptTag({path:path.join(runtime,'protocol/modbus-relay-motor.js')});
 await page.evaluate(()=>{const w=window as any;w.AppState._appendDebugLog=(_:any,title:any)=>w.debug.push(title);});
}
for(const nc of [false,true]){
 test(`initial unknown stays unknown with NC=${nc}`,async({page})=>{
  await setup(page,nc);await page.evaluate(()=>{const a=(window as any).AppState;a._dcUpdateAllCoilUI(0);a._renderCoilGrid();});
  await expect(page.locator('.dc-coil-st')).toHaveText(['--','--']);await expect(page.locator('.coil-st')).toHaveText(['--','--']);
 });
 test(`failed read clears prior states with NC=${nc}`,async({page})=>{
  await setup(page,nc);await page.evaluate(async()=>{const w=window as any;await w.AppState._dcRefreshCoilStatus(0);await w.AppState.refreshCoilStatus();w.reject=true;await w.AppState._dcRefreshCoilStatus(0);await w.AppState.refreshCoilStatus();});
  await expect(page.locator('.dc-coil-st')).toHaveText(['--','--']);await expect(page.locator('.coil-st')).toHaveText(['--','--']);
  expect(await page.evaluate(()=>(window as any).AppState._deviceCoilCache)).toEqual({});
 });
}
for(const protocol of [0,1]){
 test(`read mode and debug match controlProtocol=${protocol}`,async({page})=>{
  await setup(page,true,protocol);await page.evaluate(async()=>{await (window as any).AppState._dcRefreshCoilStatus(0);await (window as any).AppState.refreshCoilStatus();});
  await expect(page.locator('.dc-coil-st')).toHaveText(['ON','OFF']);await expect(page.locator('.coil-st')).toHaveText(['开','关']);
  const result=await page.evaluate(()=>({requests:(window as any).requests,debug:(window as any).debug}));
  expect(result.requests.every((r:any)=>r.params.mode===(protocol?'register':'coil'))).toBe(true);
  expect(result.debug).toEqual([protocol?'ReadRegisters FC03':'ReadCoils FC01']);
 });
}
for(const states of [[],[false],[false,null],[false,'false']]){
 test(`invalid partial states remain unknown ${JSON.stringify(states)}`,async({page})=>{
  await setup(page);await page.evaluate(async(states)=>{const w=window as any;w.response={success:true,data:{states}};await w.AppState._dcRefreshCoilStatus(0);await w.AppState.refreshCoilStatus();},states);
  await expect(page.locator('.dc-coil-st')).toHaveText(['--','--']);await expect(page.locator('.coil-st')).toHaveText(['--','--']);
 });
}
test('unsuccessful envelope clears stale display and recovery restores valid states',async({page})=>{
 await setup(page);await page.evaluate(async()=>{const w=window as any;w.response={success:false,error:'timeout'};await w.AppState._dcRefreshCoilStatus(0);await w.AppState.refreshCoilStatus();});
 await expect(page.locator('.dc-coil-st')).toHaveText(['--','--']);await expect(page.locator('.coil-st')).toHaveText(['--','--']);
 await page.evaluate(async()=>{const w=window as any;w.response={success:true,data:{states:[true,false]}};await w.AppState._dcRefreshCoilStatus(0);await w.AppState.refreshCoilStatus();});
 await expect(page.locator('.dc-coil-st')).toHaveText(['OFF','ON']);await expect(page.locator('.coil-st')).toHaveText(['关','开']);
});

for(const status of [503,429]) {
 test(`HTTP ${status} defers status without reporting slave offline or exhausting refresh`,async({page})=>{
  await setup(page);
  await page.evaluate(async(status)=>{
   const w=window as any,a=w.AppState;w.offlineMessages=[];
   a._coilAutoRefreshErrors=0;a._setCtrlModalOffline=(_:any,message:any)=>w.offlineMessages.push(message);
   await a._dcRefreshCoilStatus(0);await a.refreshCoilStatus();
   w.rejectObject={status,data:{success:false,error:'Resource busy'}};
   await a._dcRefreshCoilStatus(0);await a.refreshCoilStatus();
  },status);
  await expect(page.locator('.dc-coil-st')).toHaveText(['--','--']);
  await expect(page.locator('.coil-st')).toHaveText(['--','--']);
  await expect(page.locator('.dc-offline-badge')).toHaveText('忙碌，待刷新');
  expect(await page.evaluate(()=>(window as any).AppState._dcDeviceOnline[0])).toBeNull();
  expect(await page.evaluate(()=>(window as any).AppState._coilAutoRefreshErrors)).toBe(0);
  expect(await page.evaluate(()=>(window as any).offlineMessages)).toEqual(['服务忙碌，等待刷新']);
  await page.evaluate(async()=>{const w=window as any;delete w.rejectObject;await w.AppState._dcRefreshCoilStatus(0);await w.AppState.refreshCoilStatus();});
  await expect(page.locator('.dc-offline-badge')).toHaveCount(0);
  await expect(page.locator('.dc-coil-st')).toHaveText(['ON','OFF']);
 });
}

test('BUSY envelope remains unknown and retains automatic refresh budget',async({page})=>{
 await setup(page);await page.evaluate(async()=>{
  const w=window as any,a=w.AppState;a._coilAutoRefreshErrors=0;
  w.response={success:false,errorCode:'BUSY'};
  await a._dcRefreshCoilStatus(0);await a.refreshCoilStatus();
 });
 await expect(page.locator('.dc-offline-badge')).toHaveText('忙碌，待刷新');
 expect(await page.evaluate(()=>(window as any).AppState._coilAutoRefreshErrors)).toBe(0);
});
