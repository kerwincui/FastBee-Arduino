import {test,expect} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
const root=resolve(__dirname,'../../..'),source=process.env.COM15_RECOVERY_SOURCE_ROOT||root;
const fixture=JSON.parse(readFileSync(resolve(root,'test/browser/fixtures/com15-monitor-cache.json'),'utf8'));
async function setup(page:any){
 await page.clock.install();await page.setContent('<div id="device-control-page" class="active"><div id="dc-content"></div></div>');
 await page.evaluate(()=>{
  const w=window as any;w.AppState={};w.escapeHtml=(x:any)=>String(x??'');w.sources=[];
  w.EventSource=class{closed=false;listeners:any={};onopen:any;onerror:any;
   constructor(){w.sources.push(this);}
   addEventListener(k:string,fn:any){(this.listeners[k]??=[]).push(fn);}
   removeEventListener(k:string,fn:any){this.listeners[k]=(this.listeners[k]||[]).filter((v:any)=>v!==fn);}
   close(){this.closed=true;}
   emit(k:string,data:any){(this.listeners[k]||[]).slice().forEach((fn:any)=>fn({data:JSON.stringify(data)}));}
  };
 });
 for(const f of ['js/utils.js','modules/runtime/device-control/core.js','modules/runtime/device-control/render.js','modules/runtime/device-control/modbus-ops.js','modules/runtime/device-control/modbus-ctrl.js','js/state-sse.js'])await page.addScriptTag({path:resolve(source,'web-src',f)});
 await page.evaluate((fixture:any)=>{
  const w=window as any,a=w.AppState;w.fixture=fixture;w.received=0;w.defer=false;
  a.currentPage='device-control';a._featureActive={deviceControl:true};a._eventsBound=true;
  a._ensureDeviceControlRuntime=()=>Promise.resolve();a._fetchDeviceInfo=()=>Promise.resolve();
  a._setDeviceControlRefreshLoading=a._dcApplyZoom=a._dcApplyLayout=a._dcInitFreeLayout=()=>{};
  a._renderControlPanel=(d:any)=>a._renderMonitorDataCard(d);
  a._handleSSESensorData=()=>w.received++;
  w.apiGet=w.apiGetFresh=w.apiGetSilent=w.apiGetSilentFresh=async(path:string)=>{
   if(path==='/api/periph-exec/controls'){
    if(w.defer)return new Promise((resolve,reject)=>{w.finishControls=resolve;w.failControls=reject;});
    return {success:true,data:structuredClone(fixture.controls)};
   }
   if(path.startsWith('/api/modbus/status'))return {success:true,data:structuredClone(fixture.status)};
   if(path==='/api/protocol/config')return {success:true,data:{modbusRtu:{enabled:true,master:{devices:[]}}}};
   throw Error(path);
  };
 },fixture);
 await page.evaluate(async()=>{const w=window as any;await w.AppState.loadDeviceControlPage();w.sources[0].onopen();});
}
test('reconnected stream survives snapshot refresh and delivers events during reload',async({page})=>{
 await setup(page);
 await page.evaluate(()=>{const w=window as any;w.defer=true;w.sources[0].onerror();});
 await page.clock.runFor(1100);
 await page.evaluate(()=>(window as any).sources[1].onopen());
 await page.clock.runFor(2100);
 expect(await page.evaluate(()=>(window as any).sources[1].closed)).toBe(false);
 await page.evaluate(()=>{const w=window as any;w.sources[1].emit('sensor-data',{key:'probe'});});
 expect(await page.evaluate(()=>(window as any).received)).toBe(1);
 await page.evaluate(async()=>{const w=window as any;const pending=w.AppState._dcPageLoadPromise;w.defer=false;w.finishControls({success:true,data:structuredClone(w.fixture.controls)});await pending;});
 expect(await page.evaluate(()=>(window as any).sources.length)).toBe(2);
 expect(await page.evaluate(()=>(window as any).sources[1].closed)).toBe(false);
 await page.evaluate(()=>{const w=window as any;w.sources[1].emit('sensor-data',{key:'probe'});});
 expect(await page.evaluate(()=>(window as any).received)).toBe(2);
 await page.evaluate(()=>(window as any).AppState.destroyDeviceControlPage());
 expect(await page.evaluate(()=>(window as any).sources[1].closed)).toBe(true);
 await page.clock.runFor(180000);
 expect(await page.evaluate(()=>(window as any).sources.length)).toBe(2);
});
test('failed snapshot retains recovered stream without duplicated handlers',async({page})=>{
 await setup(page);await page.evaluate(()=>{const w=window as any;w.defer=true;w.pending=w.AppState.loadDeviceControlPage({noCache:true});});
 await expect.poll(()=>page.evaluate(()=>typeof (window as any).failControls)).toBe('function');
 await page.evaluate(async()=>{const w=window as any;w.failControls(Error('read timeout'));await w.pending;});
 expect(await page.evaluate(()=>(window as any).sources.length)).toBe(1);
 expect(await page.evaluate(()=>(window as any).sources[0].closed)).toBe(false);
 await page.evaluate(()=>{const w=window as any;w.sources[0].emit('sensor-data',{key:'probe'});});
 expect(await page.evaluate(()=>(window as any).received)).toBe(1);
 await expect(page.locator('.dc-recovery-notice')).toHaveCount(1);
});

test('leaving during an in-flight refresh closes SSE and ignores late completion',async({page})=>{
 await setup(page);
 await page.evaluate(()=>{const w=window as any;w.defer=true;w.pending=w.AppState.loadDeviceControlPage({noCache:true});});
 await expect.poll(()=>page.evaluate(()=>typeof (window as any).finishControls)).toBe('function');
 await page.evaluate(()=>{const w=window as any;w.AppState.currentPage='other';w.AppState.destroyDeviceControlPage();});
 expect(await page.evaluate(()=>(window as any).sources[0].closed)).toBe(true);
 await page.evaluate(async()=>{const w=window as any;w.finishControls({success:true,data:structuredClone(w.fixture.controls)});await w.pending;w.sources[0].emit('sensor-data',{key:'late'});});
 await page.clock.runFor(180000);
 expect(await page.evaluate(()=>(window as any).sources.length)).toBe(1);
 expect(await page.evaluate(()=>(window as any).received)).toBe(0);
});

test('repeated refreshes keep one event handler and page reentry creates one new stream',async({page})=>{
 await setup(page);
 for(let round=1;round<=4;round++){
  await page.evaluate(async()=>{const w=window as any;await w.AppState.loadDeviceControlPage({noCache:true});w.sources[0].emit('sensor-data',{key:'probe'});});
  expect(await page.evaluate(()=>(window as any).received)).toBe(round);
  expect(await page.evaluate(()=>(window as any).sources.length)).toBe(1);
 }
 await page.evaluate(()=>{const w=window as any;w.AppState.destroyDeviceControlPage();w.sources[0].emit('sensor-data',{key:'after-destroy'});});
 expect(await page.evaluate(()=>(window as any).received)).toBe(4);
 await page.evaluate(async()=>{const w=window as any;await w.AppState.loadDeviceControlPage();w.sources[1].onopen();w.sources[1].emit('sensor-data',{key:'reentered'});});
 expect(await page.evaluate(()=>(window as any).sources.length)).toBe(2);
 expect(await page.evaluate(()=>(window as any).received)).toBe(5);
});
