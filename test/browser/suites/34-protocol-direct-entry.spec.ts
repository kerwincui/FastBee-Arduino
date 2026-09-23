import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
const root=resolve(__dirname,'../../..');
const source=(file:string)=>readFileSync(resolve(root,file),'utf8');

test('direct protocol entry renders Modbus without loading peripherals or i18n', async ({page})=>{
 const errors:string[]=[];page.on('pageerror',error=>errors.push(error.message));
 await page.setContent(source('web-src/pages/protocol.html')+source('web-src/pages/fragments/protocol-modbus-rtu.html'));
 await page.evaluate(()=>{
  const w=window as any;
  w.escapeHtml=(s:any)=>String(s??'');
  w.Notification={error:(s:string)=>{w.__notification=s;}};
  w.AppState={applyDeveloperModeState(){},isDeveloperModeEnabled:()=>true,getDeveloperModeDisabledText:()=>'',
   showElement(){},hideElement(){},
   _setCheckbox(id:string,v:boolean){const el=document.getElementById(id) as HTMLInputElement;if(el)el.checked=v;},
   _setValue(id:string,v:any){const el=document.getElementById(id) as HTMLInputElement;if(el)el.value=String(v);}};
  w.apiGet=w.apiGetFresh=async(path:string,params:any={})=>{
   if(path==='/api/peripherals')return {success:true,data:[{id:'rs485',name:'RS485 UART1',type:1,enabled:true}]};
   if(path==='/api/peripherals/')return {success:true,data:{pins:[18,17],params:{baudRate:9600}}};
   return {success:true,data:{}};
  };
 });
 for(const file of ['common','protocol-config','modbus-config'])await page.addScriptTag({content:source(`web-src/modules/runtime/protocol/${file}.js`)});
 await page.evaluate(()=>{
  const w=window as any,a=w.AppState;
  if(typeof w.t!=='undefined')throw new Error('Regression must start without global translator');
  a.refreshMasterStatus=()=>{};a._startMasterStatusRefresh=()=>{};
  a._fillProtocolForm('modbus-rtu',{modbusRtu:{enabled:true,peripheralId:'rs485',master:{
   tasks:Array.from({length:5},(_,i)=>({name:'Sensor '+i,enabled:true,slaveAddress:i+1,mappings:[{}]})),
   devices:[{name:'Relay',deviceType:'relay',enabled:true,slaveAddress:6}]}}});
 });
 await expect(page.locator('#all-devices-body tr')).toHaveCount(6);
 await expect(page.locator('#rtu-peripheral-id')).toHaveValue('rs485');
 await expect(page.locator('#all-devices-body')).toContainText('映射');
 await expect(page.locator('#all-devices-body tr').last()).toContainText('Relay');
 // The common module's lazy-control failure notification must be self-contained too.
 await page.evaluate(()=>{
  const w=window as any;
  w.ModuleLoader={loadModule(_name:string,done:()=>void){done();}};
  w.AppState._openModbusControlModal(0,null);
 });
 expect(await page.evaluate(()=>(window as any).__notification)).toBe('加载失败，请刷新重试');
 expect(errors).toEqual([]);
});

