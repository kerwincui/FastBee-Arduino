import {test,expect} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
const root=resolve(__dirname,'../../..');
const source=(p:string)=>readFileSync(resolve(p.startsWith('web-src/')?(process.env.COM15_RECOVERY_SOURCE_ROOT||root):root,p),'utf8');
for(const width of [330,640,695,1060,1420])for(const zoom of [0.7,1,1.3]){
 test(`device-control default/reset layout has no collisions at ${width}px / zoom ${zoom}`,async({page})=>{
  await page.route('http://layout.test/',route=>route.fulfill({contentType:'text/html',body:'<meta charset="utf-8"><button class="dc-layout-reset">重置布局</button><div class="dc-zoom-wrapper"><div class="dc-control-flow"></div></div>'}));
  await page.goto('http://layout.test/');
  await page.addStyleTag({content:source('web-src/css/main.css')});
  await page.addStyleTag({content:`body{margin:0}.dc-control-flow{width:${width}px}.dc-zoom-wrapper{transform:scale(${zoom});transform-origin:0 0} *{transition:none!important}`});
  await page.evaluate(()=>{const w=window as any;w.AppState={};w.escapeHtml=(v:any)=>String(v??'');});
  for(const file of ['render','layout'])await page.addScriptTag({content:source(`web-src/modules/runtime/device-control/${file}.js`)});
  await page.evaluate(()=>{
   const w=window as any,a=w.AppState,flow=document.querySelector('.dc-control-flow')!;
   // Production inventory markup/CSS exposes the original 695px-vs-one-column defect.
   flow.innerHTML=a._renderModbusDeviceInventory(Array.from({length:6},(_,i)=>({name:'Device '+i,deviceType:i===5?'relay':'sensor',slaveAddress:i+1})))+
    '<div class="dc-control-group" data-dc-sort-key="gpio"><div style="height:140px">GPIO</div></div>'+
    '<div class="dc-control-group" data-dc-sort-key="system"><div style="height:240px">System</div></div>'+
    '<div class="dc-monitor-grid" data-dc-sort-key="monitor-data"><div style="height:100px">Monitor</div></div>'+
    '<div class="dc-control-group" data-dc-sort-key="future-wide" style="width:695px"><div style="height:110px">Wide plugin</div></div>';
   document.querySelector('.dc-layout-reset')!.addEventListener('click',()=>a._dcResetLayout());
   a._dcApplyLayout();
  });
  async function assertBoxes(){
   const state=await page.evaluate(()=>{
    const flow=document.querySelector('.dc-control-flow') as HTMLElement;
    const bounds=flow.getBoundingClientRect();
    const cards=Array.from(flow.querySelectorAll('[data-dc-sort-key]')).map(e=>({key:e.getAttribute('data-dc-sort-key'),r:e.getBoundingClientRect().toJSON()}));
    const collisions=[];
    for(let i=0;i<cards.length;i++)for(let j=i+1;j<cards.length;j++){
     const a=cards[i].r,b=cards[j].r;
     if(Math.min(a.right,b.right)-Math.max(a.left,b.left)>0.5&&Math.min(a.bottom,b.bottom)-Math.max(a.top,b.top)>0.5)collisions.push([cards[i].key,cards[j].key]);
    }
    return {collisions,outside:cards.filter(c=>c.r.left<bounds.left-0.5||c.r.right>bounds.right+0.5||c.r.bottom>bounds.bottom+0.5).map(c=>c.key)};
   });
   expect(state.collisions).toEqual([]);expect(state.outside).toEqual([]);
  }
  await assertBoxes();
  await page.evaluate(()=>{for(const e of document.querySelectorAll('[data-dc-sort-key]')){const el=e as HTMLElement;el.style.left='0px';el.style.top='0px';el.style.width='200px';el.style.height='100px';}});
  await page.locator('.dc-layout-reset').click();
  await assertBoxes();
  await page.evaluate(()=>(window as any).AppState._dcApplyLayout());
  await assertBoxes();
 });
}

for(const viewport of [1440,1920])test(`device-control ${viewport}px viewport reproduces legacy geometry and fixes default/reset`,async({page})=>{
 await page.setViewportSize({width:viewport,height:1000});
 await page.route('http://layout.test/',route=>route.fulfill({contentType:'text/html',body:'<meta charset="utf-8"><button id="reset">重置布局</button><div class="dc-control-flow"></div>'}));
 await page.goto('http://layout.test/');
 await page.addStyleTag({content:source('web-src/css/main.css')});
 await page.addStyleTag({content:'body{margin:0}.dc-control-flow{margin-left:244px;width:calc(100vw - 268px)}*{transition:none!important}'});
 await page.evaluate(()=>{(window as any).AppState={};});
 const layout=source('web-src/modules/runtime/device-control/layout.js');
 // Use actual pre-fix production source kept in the release snapshot; no workspace mutation.
 const legacy=source('test/browser/fixtures/device-control-layout-before.js');
 await page.addScriptTag({content:legacy});
 await page.evaluate(()=>{
  const specs=[['health','dc-risk-banner',197],['monitor-data','dc-monitor-grid',187],['modbus-devices','dc-modbus-device-inventory',115],['sensor','dc-control-group',93],['other','dc-control-group',93],['system','dc-control-group',98],['modbus-relay-0','dc-modbus-device-panel',270]];
  document.querySelector('.dc-control-flow')!.innerHTML=specs.map(([key,cls,h])=>`<div class="${cls}" data-dc-sort-key="${key}" style="min-height:${h}px">${key}</div>`).join('');
  (window as any).AppState._dcApplyLayout();
  document.getElementById('reset')!.onclick=()=>(window as any).AppState._dcResetLayout();
 });
 async function geometry(){return page.evaluate(()=>{
  const flow=document.querySelector('.dc-control-flow')!,b=flow.getBoundingClientRect();
  const cards=Array.from(flow.children).map(e=>({key:e.getAttribute('data-dc-sort-key'),r:e.getBoundingClientRect()}));
  const collisions:string[][]=[];for(let i=0;i<cards.length;i++)for(let j=i+1;j<cards.length;j++){const a=cards[i].r,c=cards[j].r;if(Math.min(a.right,c.right)-Math.max(a.left,c.left)>1&&Math.min(a.bottom,c.bottom)-Math.max(a.top,c.top)>1)collisions.push([cards[i].key!,cards[j].key!]);}
  return {collisions,outside:cards.filter(c=>c.r.right>b.right+1).map(c=>c.key)};
 });}
 const before=await geometry();
 if(viewport===1440)expect(before.outside).toContain('modbus-devices');
 else expect(before.collisions.some(pair=>pair.includes('modbus-devices'))).toBe(true);
 await page.addScriptTag({content:layout});
 await page.evaluate(()=>(window as any).AppState._dcApplyLayout());
 expect(await geometry()).toEqual({collisions:[],outside:[]});
 await page.locator('#reset').click();
 expect(await geometry()).toEqual({collisions:[],outside:[]});
});
