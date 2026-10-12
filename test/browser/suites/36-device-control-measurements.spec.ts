import {test,expect} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
const root=resolve(__dirname,'../../..');
const source=(p:string)=>readFileSync(resolve(p.startsWith('web-src/')?(process.env.COM15_RECOVERY_SOURCE_ROOT||root):root,p),'utf8');
test.beforeEach(async({page})=>{
 await page.setContent('<div id="device-control-page" class="active"><div id="dc-content"></div></div>');
 await page.addStyleTag({content:source('web-src/css/main.css')});
 await page.evaluate(()=>{const w=window as any;w.AppState={};w.escapeHtml=(v:any)=>String(v??'');});
 await page.addScriptTag({content:source('web-src/js/utils.js')});
 for(const file of ['core','render','modbus-ops'])await page.addScriptTag({content:source(`web-src/modules/runtime/device-control/${file}.js`)});
 await page.evaluate(()=>{const a=(window as any).AppState;a.currentPage='device-control';a._featureActive={deviceControl:true};a._filterByActionType=(items:any[],types:number[])=>(items||[]).filter(i=>types.includes(i.actionType));});
});
test('page load requests full cached Modbus status and renders seven mapped channels including zero',async({page})=>{
 await page.evaluate(async()=>{
  const w=window as any,a=w.AppState;w.__requests=[];
  w.apiGet=w.apiGetSilent=w.apiGetFresh=w.apiGetSilentFresh=async(path:string)=>{
   w.__requests.push(path);
   if(path==='/api/periph-exec/controls')return {success:true,data:{sensor:[],modbus:[{id:'rule',actionType:18,name:'Not a sample'}]}};
   if(path.startsWith('/api/modbus/status'))return {success:true,data:{tasks:path.includes('full=1')?[{enabled:true,name:'Environment',status:'ok',mappings:['water','tvoc','hcho','pm25','pm10','temp','humidity'].map((sensorId,i)=>({sensorId,regOffset:i,dataType:0,scaleFactor:i>=5?.1:1,decimalPlaces:1})),cachedData:{values:[0,134,1,12,25,242,524]}}]:[]}};
   return {success:true,data:{modbusRtu:{enabled:true,master:{devices:[]}}}};
  };
  a._eventsBound=true;a._dcCancelInit=()=>{};a._dcStopAllAutoRefresh=()=>{};
  a._ensureDeviceControlRuntime=()=>Promise.resolve();a._fetchDeviceInfo=()=>Promise.resolve();
  a._setContentState=(el:HTMLElement)=>el.innerHTML='';a._setDeviceControlRefreshLoading=()=>{};
  a._renderControlPanel=(data:any)=>a._renderMonitorDataCard(data);
  a._dcApplyZoom=a._dcApplyLayout=a._dcInitFreeLayout=a._dcInitModbusDeviceStates=a._setupSSE=()=>{};
  await a.loadDeviceControlPage();
  await a.loadDeviceControlPage({noCache:true});
 });
 await expect(page.locator('.dc-monitor-card')).toHaveCount(7);
 await expect(page.locator('[data-id="water"] .dc-monitor-value')).toHaveText('0.0');
 await expect(page.locator('[data-id="temp"] .dc-monitor-value')).toHaveText('24.2');
 expect(await page.evaluate(()=>(window as any).__requests)).toContain('/api/modbus/status?full=1');
 await expect(page.locator('[data-id="rule"]')).toHaveCount(0);
});
test('degraded control metadata retries fresh and never becomes an empty sensor configuration',async({page})=>{
 const fixture=JSON.parse(source('test/browser/fixtures/com15-monitor-cache.json'));
 await page.evaluate(async fixture=>{
  const w=window as any,a=w.AppState;w.__controlsCalls=0;w.__freshCalls=0;w.__delays=[];w.__alwaysDegraded=false;
  const getter=async(path:string)=>{
   if(path==='/api/periph-exec/controls'){
    w.__controlsCalls++;
    if(w.__alwaysDegraded || [2,3].includes(w.__controlsCalls))return {success:true,degraded:true,data:{sensor:[]}};
    return {success:true,data:fixture.controls};
   }
   if(path.startsWith('/api/modbus/status'))return {success:true,data:fixture.status};
   return {success:true,data:{modbusRtu:{enabled:true,master:{devices:[]}}}};
  };
  w.apiGet=w.apiGetSilent=w.apiGetSilentFresh=getter;
  w.apiGetFresh=async(path:string)=>{w.__freshCalls++;return getter(path);};
  a._eventsBound=true;a._dcCancelInit=()=>{};a._dcStopAllAutoRefresh=()=>{};
  a._ensureDeviceControlRuntime=()=>Promise.resolve();a._fetchDeviceInfo=()=>Promise.resolve();
  a._setContentState=(el:HTMLElement,state:string,message:string)=>{el.dataset.state=state;el.textContent=message||'';};
  a._setDeviceControlRefreshLoading=()=>{};a._renderControlPanel=(data:any)=>a._renderMonitorDataCard(data);
  a._dcApplyZoom=a._dcApplyLayout=a._dcInitFreeLayout=a._dcInitModbusDeviceStates=a._setupSSE=()=>{};
  a._dcDelay=async(ms:number)=>{w.__delays.push(ms);return true;};
  await a.loadDeviceControlPage();
  await a.loadDeviceControlPage();
 },fixture);
 await expect(page.locator('.dc-monitor-card')).toHaveCount(9);
 expect(await page.evaluate(()=>({calls:(window as any).__controlsCalls,fresh:(window as any).__freshCalls,delays:(window as any).__delays}))).toEqual({calls:4,fresh:2,delays:[1000,2000]});
 await page.evaluate(async()=>{(window as any).__alwaysDegraded=true;await (window as any).AppState.loadDeviceControlPage();});
 await expect(page.locator('.dc-recovery-notice')).toContainText('通信失败');
 await expect(page.locator('.dc-monitor-card')).toHaveCount(9);
 await expect(page.locator('.dc-monitor-card[data-measurement-valid="true"]')).toHaveCount(0);
 expect(await page.evaluate(()=>(window as any).__controlsCalls)).toBe(8);
});

test('leaving the page cancels degraded metadata retries',async({page})=>{
 await page.evaluate(async()=>{
  const w=window as any,a=w.AppState;w.__controlsCalls=0;
  w.apiGet=w.apiGetSilent=w.apiGetFresh=w.apiGetSilentFresh=async()=>{w.__controlsCalls++;return {success:true,degraded:true,data:{sensor:[]}};};
  a._eventsBound=true;a._dcCancelInit=()=>{};a._dcStopAllAutoRefresh=()=>{};
  a._ensureDeviceControlRuntime=()=>Promise.resolve();a._fetchDeviceInfo=()=>Promise.resolve();
  a._setContentState=(el:HTMLElement)=>el.innerHTML='';a._setDeviceControlRefreshLoading=()=>{};
  a._dcDelay=async()=>{a._dcLifecycleGeneration++;return false;};
  await a.loadDeviceControlPage();
 });
 expect(await page.evaluate(()=>(window as any).__controlsCalls)).toBe(1);
 await expect(page.locator('.dc-monitor-card')).toHaveCount(0);
});

test('register decoding, missing/quality values and SSE preserve zero and scaled signed samples',async({page})=>{
 const values=await page.evaluate(()=>{
  const a=(window as any).AppState;
  const read=(words:any[],dataType:number,scaleFactor:any=1,status='ok')=>a._dcMappedRegisterValue({status,cachedData:{values:words}},{regOffset:0,dataType,scaleFactor,decimalPlaces:1});
  return [read([65526],1,.1),read([65535,65535],2),read([65535,65526],3),read([0x41c2,0],4),read([123],0,0),read([null],0),read([1],2),read([123],0,1,'stale'),a._dcMonitorValue({value:0}),a._dcMonitorValue({value:52,quality:'low'}),a._dcMonitorValue({value:null})];
 });
 expect(values).toEqual(['-1.0','4294967295.0','-10.0','24.3','0.0','--','--','--','0','--','--']);
 await page.evaluate(()=>{
  const a=(window as any).AppState;
  a._modbusStatus={tasks:[{enabled:true,status:'ok',mappings:[{sensorId:'temp',regOffset:0,dataType:1,scaleFactor:.1,decimalPlaces:1}],cachedData:{values:[65526]}}]};
  document.getElementById('dc-content')!.innerHTML=a._renderMonitorDataCard({sensor:[{actionType:19,sensors:[{key:'soil',value:0}]}]});
  a._handleSSEModbusData([{sensorId:'temp',value:'-2.5'}]);
  // Rerender from SSE cache must not decode -25 as an unsigned register.
  document.getElementById('dc-content')!.innerHTML=a._renderMonitorDataCard({});
 });
 await expect(page.locator('[data-id="temp"] .dc-monitor-value')).toHaveText('-2.5');
 await page.evaluate(()=>{const a=(window as any).AppState;a._handleSSEModbusData([{sensorId:'temp',value:null,quality:'low'}]);document.getElementById('dc-content')!.innerHTML=a._renderMonitorDataCard({});});
 await expect(page.locator('[data-id="temp"] .dc-monitor-value')).toHaveText('--');
});
test('unknown relay state remains unknown with NC until valid SSE arrives',async({page})=>{
 await page.evaluate(()=>{const a=(window as any).AppState;a._modbusDevices=[{ncMode:true,channelCount:2}];a._renderSidTag=()=>'';document.getElementById('dc-content')!.innerHTML=a._renderDcCoilGrid(0,2,[],true);});
 await expect(page.locator('.dc-coil-unknown')).toHaveCount(2);
 await page.evaluate(()=>(window as any).AppState._updateCoilUIFromSSE(0,0,'invalid'));
 await expect(page.locator('[data-ch="0"] .dc-coil-st')).toHaveText('--');
 await page.evaluate(()=>(window as any).AppState._updateCoilUIFromSSE(0,0,'0'));
 await expect(page.locator('[data-ch="0"] .dc-coil-st')).toHaveText('ON');
});

test('captured COM15 cache shows all seven Modbus readings beside distinct soil channels',async({page})=>{
 const fixture=JSON.parse(source('test/browser/fixtures/com15-monitor-cache.json'));
 await page.evaluate(({status,controls})=>{const a=(window as any).AppState;a._modbusStatus=status;document.getElementById('dc-content')!.innerHTML=a._renderMonitorDataCard(controls);},fixture);
 await expect(page.locator('.dc-monitor-card')).toHaveCount(9);
 for(const task of fixture.status.tasks)for(const mapping of task.mappings){
  const expected=(task.cachedData.values[mapping.regOffset]*mapping.scaleFactor).toFixed(mapping.decimalPlaces);
  await expect(page.locator(`[data-id="${mapping.sensorId}"] .dc-monitor-value`)).toHaveText(expected);
 }
 await expect(page.locator('[data-id="ai_soil_temp_soil_temperature"] .dc-monitor-value')).toHaveText('11.8');
 await expect(page.locator('[data-id="ai_soil_humidity_soil_humidity"] .dc-monitor-value')).toHaveText('0');
 await expect(page.locator('[data-id="ai_soil_humidity_soil_humidity"]')).toHaveAttribute('data-measurement-valid','false');
 await expect(page.locator('[data-id="s3_modbus_relay"]')).toHaveCount(0);
});

test('missing soil humidity displays zero without changing cached measurement or other sensors',async({page})=>{
 for(const sample of [{value:null,quality:'low'},{},{value:' '},{value:true},{value:0,valueValid:false},{value:6.9,quality:'disconnected'}]) {
  const result=await page.evaluate(sample=>{
   const a=(window as any).AppState;
   const sensor:any={key:'ai_soil_humidity_soil_humidity',unit:'%',...sample};
   a._controlData={sensor:[{actionType:19,sensors:[sensor,{key:'ai_soil_temp_soil_temperature',value:null,unit:'°C'}]}]};
   document.getElementById('dc-content')!.innerHTML=a._renderMonitorDataCard(a._controlData);
   const receivedAt=sensor._dcSampleReceivedAt;
   document.getElementById('dc-content')!.innerHTML=a._renderMonitorDataCard(a._controlData);
   const {_dcSampleReceivedAt,...measurement}=sensor;
   return {unchanged:JSON.stringify(measurement)===JSON.stringify({key:'ai_soil_humidity_soil_humidity',unit:'%',...sample}),
    receiptPreserved:Number.isFinite(receivedAt)&&receivedAt===_dcSampleReceivedAt,rawValue:a._dcMonitorValue(sensor)};
  },sample);
  const card=page.locator('[data-id="ai_soil_humidity_soil_humidity"]');
  await expect(card.locator('.dc-monitor-value')).toHaveText('0');
  await expect(card.locator('.dc-monitor-unit')).toHaveText('%');
  await expect(card).toHaveAttribute('data-value-placeholder','true');
  await expect(card).toHaveAttribute('data-measurement-valid','false');
  await expect(card.locator('.dc-monitor-status')).toBeVisible();
  await expect(card.locator('.dc-monitor-status')).toContainText('无有效数据');
  await expect(page.locator('[data-id="ai_soil_temp_soil_temperature"] .dc-monitor-value')).toHaveText('--');
  expect(result).toEqual({unchanged:true,receiptPreserved:true,rawValue:'--'});
 }
});

test('soil humidity SSE and rerender distinguish real zero from missing-data zero',async({page})=>{
 await page.evaluate(()=>{
  const a=(window as any).AppState;
  a._controlData={sensor:[{actionType:19,sensors:[{key:'ai_soil_humidity_soil_humidity',value:null,quality:'low',unit:'%'}]}]};
  document.getElementById('dc-content')!.innerHTML=a._renderMonitorDataCard(a._controlData);
 });
 const card=page.locator('[data-id="ai_soil_humidity_soil_humidity"]');
 for(const value of [6.9,0,null]) {
  await page.evaluate(value=>{
   const a=(window as any).AppState;
   a._handleSSESensorData({key:'ai_soil_humidity_soil_humidity',value,quality:value===null?'disconnected':'valid',valueValid:value!==null});
  },value);
  await expect(card.locator('.dc-monitor-value')).toHaveText(String(value??0));
  await expect(card).toHaveAttribute('data-value-placeholder',String(value===null));
  await expect(card).toHaveAttribute('data-measurement-valid',String(value!==null));
  if(value===null) await expect(card.locator('.dc-monitor-status')).toBeVisible();
  else await expect(card.locator('.dc-monitor-status')).toBeHidden();
  await page.evaluate(()=>{const a=(window as any).AppState;document.getElementById('dc-content')!.innerHTML=a._renderMonitorDataCard(a._controlData);});
  await expect(card.locator('.dc-monitor-value')).toHaveText(String(value??0));
  await expect(card).toHaveAttribute('data-measurement-valid',String(value!==null));
 }
});

test('temporarily stopped Modbus keeps configured cards unknown until fresh SSE samples arrive',async({page})=>{
 const fixture=JSON.parse(source('test/browser/fixtures/com15-monitor-cache.json'));
 await page.evaluate(({status,controls})=>{
  const a=(window as any).AppState;
  a._modbusStatus={status:'stopped',tasks:[]};
  a._dcApplyProtocolDevices({success:true,data:{modbusRtu:{enabled:true,master:{tasks:status.tasks,devices:[]}}}});
  document.getElementById('dc-content')!.innerHTML=a._renderMonitorDataCard(controls);
 },fixture);
 await expect(page.locator('.dc-monitor-card')).toHaveCount(9);
 for(const task of fixture.status.tasks)for(const mapping of task.mappings)
  await expect(page.locator('[data-id="'+mapping.sensorId+'"] .dc-monitor-value')).toHaveText('--');
 await page.evaluate(()=>{
  const a=(window as any).AppState;
  a._handleSSEModbusData([{sensorId:'modbus_temp',value:'24.3',quality:'valid'}]);
  document.getElementById('dc-content')!.innerHTML=a._renderMonitorDataCard(a._controlData||{});
 });
 await expect(page.locator('[data-id="modbus_temp"] .dc-monitor-value')).toHaveText('24.3');
});

test('degraded protocol response cannot erase configured devices or claim metadata loaded',async({page})=>{
 const result=await page.evaluate(()=>{
  const a=(window as any).AppState;
  a._dcApplyProtocolDevices({success:true,data:{modbusRtu:{enabled:true,master:{devices:[{enabled:true,name:'relay'}]}}}});
  const accepted=a._dcApplyProtocolDevices({success:true,degraded:true,data:{modbusRtu:{enabled:false,degraded:true,master:{tasks:[],devices:[]}}}});
  return {accepted,devices:a._modbusDevices.map((d:any)=>d.name)};
 });
 expect(result).toEqual({accepted:false,devices:['relay']});
});

test('invalid or failed SSE samples cannot restore relay online or invent numeric readings',async({page})=>{
 const values=await page.evaluate(()=>{const a=(window as any).AppState;return [true,false,'   ',[],{},null].map(value=>a._dcMonitorValue({value}));});
 expect(values).toEqual(['--','--','--','--','--','--']);
 await page.evaluate(()=>{
  const a=(window as any).AppState;
  a._modbusDevices=[{sensorId:'relay',deviceType:'relay',channelCount:1,ncMode:true}];
  a._dcDeviceOnline={0:false};a._renderSidTag=()=>'';
  a._dcUpdatePanelOnlineState=(idx:number,online:boolean)=>{a._dcDeviceOnline[idx]=online;};
  document.getElementById('dc-content')!.innerHTML=a._renderDcCoilGrid(0,1,[],true);
 });
 for(const item of [{value:0,quality:'error'},{value:null},{value:' '},{value:true}]) {
  await page.evaluate(item=>(window as any).AppState._handleSSEModbusData([{sensorId:'relay_ch0',...item}]),item);
  await expect(page.locator('.dc-coil-st')).toHaveText('--');
  expect(await page.evaluate(()=>(window as any).AppState._dcDeviceOnline[0])).toBe(false);
 }
 await page.evaluate(()=>(window as any).AppState._handleSSEModbusData([{sensorId:'relay_ch0',value:'0',quality:'good'}]));
 await expect(page.locator('.dc-coil-st')).toHaveText('ON');
 expect(await page.evaluate(()=>(window as any).AppState._dcDeviceOnline[0])).toBe(true);
});
