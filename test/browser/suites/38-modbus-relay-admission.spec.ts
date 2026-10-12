import {test,expect} from '@playwright/test';
import path from 'node:path';
const runtime=path.resolve(__dirname,'../../../web-src/modules/runtime');
async function setup(page:any){
 await page.setContent('<style>.dc-coil-card{width:80px;height:40px}</style><div id="dc-content"><div class="dc-coil-card" data-dev="0" data-ch="0"></div><div class="dc-coil-card" data-dev="0" data-ch="1"></div><button class="dc-coil-batch" data-dev="0" data-action="allOn"></button><button class="dc-coil-refresh" data-dev="0"></button></div><div id="coil-status-grid"></div>');
 await page.evaluate(()=>{const w=window as any;w.AppState={};w.requests=[];w.resolvers=[];w.aborts=0;w.warnings=[];
 w.Notification={warning:(text:any)=>w.warnings.push(text),error:()=>{},success:()=>{}};
 w.apiAbortPageRequests=()=>w.aborts++;
 const request=(method:any,url:any,params:any)=>{w.requests.push({method,url,params});return new Promise((resolve,reject)=>w.resolvers.push({resolve,reject}));};
 w.apiGetSilent=(url:any,params:any)=>request('GET',url,params);
 w.apiPostPriority=(url:any,params:any)=>request('POST',url,params);
 });
 await page.addScriptTag({path:path.resolve(runtime,'../../js/utils.js')});
 for(const module of ['device-control/core.js','device-control/modbus-ctrl.js','protocol/modbus-relay-motor.js'])await page.addScriptTag({path:path.join(runtime,module)});
 await page.evaluate(()=>{const a=(window as any).AppState;Object.assign(a,{
  _modbusDevices:[{slaveAddress:6,channelCount:2,controlProtocol:1}],_dcCoilStates:{0:[false,false]},_coilStates:[false,false],_deviceCoilCache:{},_activeDeviceIdx:0,_dcLifecycleGeneration:1,
  _dcIsLifecycleActive:(g:any)=>g===a._dcLifecycleGeneration,_dcUpdateSingleCoilUI:()=>{},_dcUpdateAllCoilUI:()=>{},_dcUpdatePanelOnlineState:()=>{},_renderCoilGrid:()=>{},
  _isCtrlModalOffline:()=>false,_markCtrlModalOnline:()=>{},_stopAutoRefreshOnErrors:()=>{},_setCtrlModalOffline:()=>{},_appendDebugLog:()=>{},_appendDebugError:()=>{},_dcOnFullscreenChange:()=>{}});});
}
async function settle(page:any,reject=false){await page.evaluate(async(reject:any)=>{const w=window as any;const pending=w.AppState._modbusRelayRequest;const r=w.resolvers.shift();if(reject)r.reject(Error('timeout'));else r.resolve({success:true,data:{state:true,states:[true,false]}});await pending;},reject);}

test('actual click bindings registered twice admit only one cross-channel/batch write, without page abort',async({page})=>{
 await setup(page);await page.evaluate(()=>{const a=(window as any).AppState;a.setupDeviceControlEvents();a.setupDeviceControlEvents();const root=document.querySelector('#dc-content')!;root.querySelectorAll('.dc-coil-card').forEach(e=>(e as HTMLElement).click());(root.querySelector('.dc-coil-batch') as HTMLElement).click();});
 expect(await page.evaluate(()=>(window as any).requests)).toHaveLength(1);
 expect(await page.evaluate(()=>(window as any).aborts)).toBe(0);
 await settle(page);await page.locator('.dc-coil-card[data-ch="1"]').click();
 expect(await page.evaluate(()=>(window as any).requests)).toHaveLength(2);await settle(page);
});

test('protocol delay preserves the selected register or coil mode and matches fullscreen params',async({page})=>{
 await setup(page);
 await page.evaluate(()=>{
  const a=(window as any).AppState;
  document.body.insertAdjacentHTML('beforeend','<input id="modbus-ctrl-delay-ch" value="1"><input id="modbus-ctrl-delay-units" value="15">');
  Object.assign(a._modbusDevices[0],{controlProtocol:1,coilBase:100,ncMode:true});
  a.startCoilDelay();
 });
 expect(await page.evaluate(()=>(window as any).requests[0])).toEqual({method:'POST',url:'/api/modbus/coil/delay',params:{
  slaveAddress:6,channel:1,delayBase:512,delayUnits:15,ncMode:true,coilBase:100,mode:'register'
 }});
 await settle(page,true);
 await page.evaluate(()=>{const a=(window as any).AppState;a._dcStartCoilDelay(0,1,15);});
 expect(await page.evaluate(()=>(window as any).requests[1].params)).toEqual(await page.evaluate(()=>(window as any).requests[0].params));
 await settle(page,true);
 await page.evaluate(()=>{const a=(window as any).AppState;Object.assign(a._modbusDevices[0],{controlProtocol:0,ncMode:false});a.startCoilDelay();});
 expect(await page.evaluate(()=>(window as any).requests[2].params.mode)).toBe('coil');
 expect(await page.evaluate(()=>(window as any).requests[2].params.ncMode)).toBe(false);
 await settle(page,true);
 expect(await page.evaluate(()=>(window as any).aborts)).toBe(0);
});

test('pending status coalesces manual refreshes and rejects writes until settled',async({page})=>{
 await setup(page);await page.evaluate(()=>{const a=(window as any).AppState;a._dcRefreshCoilStatus(0);a._dcRefreshCoilStatus(0);a._dcToggleCoil(0,0);a._dcBatchCoil(0,'allOn');a._dcStartCoilDelay(0,0,10);});
 expect(await page.evaluate(()=>(window as any).requests.map((r:any)=>r.method))).toEqual(['GET']);
 await settle(page);await page.evaluate(()=>(window as any).AppState._dcToggleCoil(0,0) && undefined);
 expect(await page.evaluate(()=>(window as any).requests.map((r:any)=>r.method))).toEqual(['GET','POST']);await settle(page);
});

test('protocol and fullscreen entry points share admission and do not queue rapid commands',async({page})=>{
 await setup(page);await page.evaluate(()=>{const a=(window as any).AppState;a.toggleCoil(0);a.toggleCoil(1);a.batchCoil('allOn');a.refreshCoilStatus();a._dcToggleCoil(0,1);});
 expect(await page.evaluate(()=>(window as any).requests)).toHaveLength(1);
 await settle(page);expect(await page.evaluate(()=>(window as any).requests)).toHaveLength(1);
 await page.evaluate(()=>(window as any).AppState.batchCoil('allOff') && undefined);
 expect(await page.evaluate(()=>(window as any).requests)).toHaveLength(2);await settle(page);
});

test('timeout and obsolete generation release admission without replaying a write',async({page})=>{
 await setup(page);await page.evaluate(()=>{const a=(window as any).AppState;a._dcCoilPending['0_0']=true;a._dcToggleCoil(0,0);});
 await page.evaluate(()=>(window as any).AppState._dcLifecycleGeneration++);await settle(page,true);
 expect(await page.evaluate(()=>(window as any).AppState._dcCoilPending['0_0'])).toBe(false);
 expect(await page.evaluate(()=>(window as any).AppState._modbusRelayRequest)).toBeNull();
 expect(await page.evaluate(()=>(window as any).requests)).toHaveLength(1);
 await page.evaluate(()=>(window as any).AppState._dcRefreshCoilStatus(0) && undefined);await settle(page);
});
