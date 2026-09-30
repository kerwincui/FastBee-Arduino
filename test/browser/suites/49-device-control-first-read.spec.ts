import {test,expect} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
const root=resolve(__dirname,'../../..');
const sourceRoot=process.env.COM15_RECOVERY_SOURCE_ROOT||root;
const fixture=JSON.parse(readFileSync(resolve(root,'test/browser/fixtures/com15-monitor-cache.json'),'utf8'));
async function setup(page:any,mode='deferred') {
 await page.clock.install();
 await page.setContent('<div id="device-control-page" class="active"><div id="dc-content"></div></div>');
 await page.evaluate(()=>{const w=window as any;w.AppState={};w.escapeHtml=(x:any)=>String(x??'');});
 for(const file of ['core','render','modbus-ops','modbus-ctrl'])
  await page.addScriptTag({path:resolve(sourceRoot,'web-src/modules/runtime/device-control/'+file+'.js')});
 await page.evaluate(({fixture,mode}:any)=>{
  const w=window as any,a=w.AppState;
  a.currentPage='device-control';a._featureActive={deviceControl:true};a._eventsBound=true;
  a._ensureDeviceControlRuntime=()=>Promise.resolve();a._fetchDeviceInfo=()=>Promise.resolve();
  a._setDeviceControlRefreshLoading=a._dcApplyZoom=a._dcApplyLayout=a._dcInitFreeLayout=a._closeSSE=()=>{};
  w.sseStarts=0;a._setupSSE=()=>w.sseStarts++;
  a._renderControlPanel=(data:any)=>a._renderMonitorDataCard(data)+a._modbusDevices.map((d:any,i:number)=>'<div class="dc-modbus-device-panel" data-dev-idx="'+i+'">'+a._renderDcCoilGrid(i,4,a._dcCoilStates[i]||[],true)+'</div>').join('');
  w.reads=[];w.writes=[];w.failRelay=mode==='busy';
  w.apiPost=w.apiPostPriority=(...args:any[])=>{w.writes.push(args);throw Error('no writes permitted');};
  w.apiGet=w.apiGetFresh=w.apiGetSilent=w.apiGetSilentFresh=async(path:string,params:any)=>{
   if(path==='/api/periph-exec/controls')return {success:true,data:structuredClone(fixture.controls)};
   if(path.startsWith('/api/modbus/status'))return {success:true,data:structuredClone(fixture.status)};
   if(path==='/api/protocol/config')return {success:true,data:{modbusRtu:{enabled:true,master:{devices:[{enabled:true,deviceType:'relay',slaveAddress:6,channelCount:4,controlProtocol:1,ncMode:true}]}}}};
   if(path==='/api/modbus/coil/status'){
    w.reads.push(params);
    if(w.failRelay)throw {status:503};
    if(mode==='malformed'&&w.reads.length===1)return {success:true,data:{states:[true,null,false]}};
    if(mode==='deferred'&&w.reads.length===1)return new Promise(resolve=>w.finishRelay=resolve);
    return {success:true,data:{states:[true,false,true,false]}};
   }
   throw Error(path);
  };
  w.firstLoad=a.loadDeviceControlPage();
 },{fixture,mode});
}
test('initial cards wait for actual readback with one read and no control writes',async({page})=>{
 await setup(page);
 await expect.poll(()=>page.evaluate(()=>(window as any).reads.length)).toBe(1);
 await expect(page.locator('.dc-coil-card')).toHaveCount(0);
 expect(await page.evaluate(()=>(window as any).sseStarts)).toBe(0);
 await page.evaluate(async()=>{const w=window as any;w.finishRelay({success:true,data:{states:[true,false,true,false]}});await w.firstLoad;});
 await expect(page.locator('.dc-coil-card')).toHaveCount(4);
 await expect(page.locator('.dc-coil-unknown')).toHaveCount(0);
 expect(await page.locator('.dc-coil-st').allTextContents()).toEqual(['OFF','ON','OFF','ON']);
 await page.clock.runFor(5000);
 expect(await page.evaluate(()=>(window as any).reads.length)).toBe(1);
 expect(await page.evaluate(()=>(window as any).writes)).toEqual([]);
 expect(await page.evaluate(()=>(window as any).sseStarts)).toBe(1);
});
test('failed first read stays unknown and retries without hiding failure',async({page})=>{
 await setup(page,'busy');await page.evaluate(async()=>await (window as any).firstLoad);
 await expect(page.locator('.dc-coil-unknown')).toHaveCount(4);
 expect(await page.evaluate(()=>(window as any).AppState._dcDeviceOnline[0])).toBe(null);
 await page.evaluate(()=>(window as any).failRelay=false);
 await page.clock.runFor(2100);
 await expect(page.locator('.dc-coil-unknown')).toHaveCount(0);
 expect(await page.evaluate(()=>(window as any).reads.length)).toBe(2);
 expect(await page.evaluate(()=>(window as any).writes)).toEqual([]);
});
test('leaving during first read cannot render stale controls or start SSE',async({page})=>{
 await setup(page);await expect.poll(()=>page.evaluate(()=>(window as any).reads.length)).toBe(1);
 await page.evaluate(async()=>{const w=window as any;w.AppState.currentPage='other';w.AppState._dcCancelInit(false);w.AppState._dcStopAllAutoRefresh();w.finishRelay({success:true,data:{states:[true,true,true,true]}});await w.firstLoad;});
 await expect(page.locator('.dc-coil-card')).toHaveCount(0);
 expect(await page.evaluate(()=>(window as any).sseStarts)).toBe(0);
});
test('refresh replaces old relay state using a new real readback',async({page})=>{
 await setup(page,'ready');await page.evaluate(async()=>await (window as any).firstLoad);
 await page.evaluate(async()=>{const w=window as any;w.failRelay=true;await w.AppState.loadDeviceControlPage({noCache:true});});
 await expect(page.locator('.dc-coil-unknown')).toHaveCount(4);
 expect(await page.evaluate(()=>(window as any).reads.length)).toBe(2);
});

test('partial first read cannot invent relay states and recovers without output writes',async({page})=>{
 await setup(page,'malformed');await page.evaluate(async()=>await (window as any).firstLoad);
 await expect(page.locator('.dc-coil-card')).toHaveCount(4);
 await expect(page.locator('.dc-coil-unknown')).toHaveCount(4);
 expect(await page.evaluate(()=>(window as any).writes)).toEqual([]);
 await page.clock.runFor(2100);
 await expect(page.locator('.dc-coil-unknown')).toHaveCount(0);
 expect(await page.locator('.dc-coil-st').allTextContents()).toEqual(['OFF','ON','OFF','ON']);
 expect(await page.evaluate(()=>(window as any).writes)).toEqual([]);
});

test('repeated refreshes invalidate stale states and finish with each real readback',async({page})=>{
 await setup(page,'ready');await page.evaluate(async()=>await (window as any).firstLoad);
 await page.evaluate(()=>{
  const w=window as any,original=w.apiGetSilent;
  w.apiGetSilent=(path:string,params:any)=>{
   if(path!=='/api/modbus/coil/status')return original(path,params);
   w.reads.push(params);return new Promise(resolve=>w.finishRefresh=resolve);
  };
 });
 for(let round=0;round<4;round++){
  await page.evaluate(()=>{const w=window as any;delete w.finishRefresh;w.refresh=w.AppState.loadDeviceControlPage({noCache:true});});
  await expect.poll(()=>page.evaluate(()=>typeof (window as any).finishRefresh)).toBe('function');
  await expect(page.locator('.dc-coil-unknown')).toHaveCount(4);
  const states=round%2 ? [true,false,true,false] : [false,true,false,true];
  await page.evaluate(async(states:boolean[])=>{const w=window as any;w.finishRefresh({success:true,data:{states}});await w.refresh;},states);
  await expect(page.locator('.dc-coil-card')).toHaveCount(4);
  await expect(page.locator('.dc-coil-unknown')).toHaveCount(0);
  expect(await page.locator('.dc-coil-st').allTextContents()).toEqual(states.map(value=>value?'OFF':'ON'));
  expect(await page.evaluate(()=>(window as any).reads.length)).toBe(round+2);
 }
 expect(await page.evaluate(()=>(window as any).writes)).toEqual([]);
});
