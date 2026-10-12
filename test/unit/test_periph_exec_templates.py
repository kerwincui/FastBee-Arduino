"""Offline behavior of production read-only collection drafts; no devices or writes."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert = require('assert/strict'), fs = require('fs'), vm = require('vm');
const els = new Map();
const el = id => { if (!els.has(id)) els.set(id, {value:'',checked:false,disabled:false,textContent:'',replaceChildren(){this.replaced=true;}}); return els.get(id); };
const app = {_peEditorDataReady:true,guardDeveloperModeAction:()=>true};
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/periph-exec-templates.js','utf8'),
    {AppState:app,document:{getElementById:el}});
app._pePeripherals = [{id:'rtc',name:'RTC',type:50,enabled:true},{id:'sensor',type:38,enabled:true,params:{driverName:'SHT31'}},{id:'relay',type:12}];
app._peTypeCatalog = [{value:50,implemented:true,featureActive:true},{value:38,implemented:true,featureActive:true,
    driverDescriptors:[{name:'SHT31',compiled:true,featureActive:true,minIntervalMs:10000,channels:[{name:'temperature',unit:'C',mode:0},{name:'mode2',unit:'V',mode:2}]}]}];
const rtc = {kind:'rtc-read',target:'rtc',interval:'60',channel:'',property:''};
const sensor = {kind:'configured-sensor',target:'sensor',interval:'60',channel:'temperature',property:'roomTemp'};
const modbus = {kind:'configured-modbus',target:'0',interval:'60',channel:'',property:'',reviewed:true};
app._masterTasks = [{name:'Meter',enabled:true,slaveAddress:1,functionCode:3,startAddress:0,quantity:2,mappings:[{sensorId:'meterVoltage',regOffset:0,unit:'V'}]}];
app._peTemplateModbusConfig = {enabled:true,transferType:0,master:{tasks:app._masterTasks}};
const build = input => app._buildPeriphExecTemplateDraft(input);
function fill(input) { for (const [key,value] of Object.entries(input)) { if(key==='reviewed') el('pe-template-modbus-reviewed').checked=value; else el('pe-template-'+key).value=value; } }
const tests = {
    modbus(){const d=build(modbus),a=d.actions[0];assert.equal(d.enabled,false);assert.equal(a.actionType,18);assert.equal(a.targetPeriphId,'');assert.equal(a.useReceivedValue,false);assert.deepEqual(JSON.parse(a.actionValue),{poll:[0]});},
    modbusConsent(){for(const reviewed of [false,undefined,1,'true'])assert.throws(()=>build({...modbus,reviewed}));},
    modbusIndex(){for(const target of ['', '-1','01','1','0.1','modbus:0','0e0','9'.repeat(30)])assert.throws(()=>build({...modbus,target}));},
    modbusProtocol(){for(const value of [false,0,undefined]){app._peTemplateModbusConfig.enabled=value;assert.throws(()=>build(modbus));}app._peTemplateModbusConfig.enabled=true;for(const value of [1,'0',undefined]){app._peTemplateModbusConfig.transferType=value;assert.throws(()=>build(modbus));}app._peTemplateModbusConfig.transferType=0;app._peTemplateModbusConfig.degraded=true;assert.throws(()=>build(modbus));},
    modbusTask(){const t=app._masterTasks[0];for(const fc of [0,1,2,5,6,15,16,23,'3',true]){t.functionCode=fc;assert.throws(()=>build(modbus));}t.functionCode=4;assert.equal(build(modbus).actions[0].actionType,18);for(const value of [false,0,undefined]){t.enabled=value;assert.throws(()=>build(modbus));}},
    modbusRange(){const t=app._masterTasks[0];for(const [key,bad] of Object.entries({slaveAddress:[0,248,1.5,'1',true],startAddress:[-1,65536,0.5,'0'],quantity:[0,65,1.5,'1']})){const original=t[key];for(const value of bad){t[key]=value;assert.throws(()=>build(modbus));}t[key]=original;}t.startAddress=65535;assert.throws(()=>build(modbus));t.quantity=1;assert.equal(build(modbus).actions[0].actionType,18);},
    modbusMappings(){const t=app._masterTasks[0];for(const mappings of [null,[],[{}],[{sensorId:'v'}],[{sensorId:'1bad',regOffset:0}],[{sensorId:'v',regOffset:-1}],[{sensorId:'v',regOffset:2}],[{sensorId:'v',regOffset:'0'}]]){t.mappings=mappings;assert.throws(()=>build(modbus));}},
    modbusStale(){fill(modbus);app.previewPeriphExecTemplate();assert(app._peTemplatePreview);el('pe-template-confirm').checked=true;app._masterTasks[0].slaveAddress=2;app.applyPeriphExecTemplate();assert.equal(app._peTemplatePreview,null);assert.equal(el('periph-exec-actions').replaced,undefined);},
    modbusApply(){fill(modbus);app.previewPeriphExecTemplate();el('pe-template-confirm').checked=true;let actions=[];app._createPeriphExecTriggerElement=()=>{};app._createPeriphExecActionElement=d=>actions.push(d);app.applyPeriphExecTemplate();assert.equal(actions.length,1);assert.deepEqual(JSON.parse(actions[0].actionValue),{poll:[0]});assert.equal(el('periph-exec-enabled').checked,false);},
    rtc(){const d=build(rtc);assert.equal(d.enabled,false);assert.equal(d.actions[0].actionType,36);assert.equal(d.actions[0].actionValue,'');assert.equal(d.actions[0].useReceivedValue,false);assert.equal(d.recordHistory,false);assert.equal(d.allowPanelControl,false);assert.equal(d.reportAfterExec,true);},
    sensor(){const d=build(sensor),v=JSON.parse(d.actions[0].actionValue);assert.equal(d.actions[0].actionType,19);assert.equal(v.channelField,'temperature');assert.equal(v.dataField,'roomTemp');assert.equal(v.periphId,'sensor');assert.equal(v.scaleFactor,1);assert.equal(v.offset,0);assert.equal(v.unit,'C');assert.equal(d.enabled,false);},
    interval(){for(const n of ['',0,-1,'x',1.5,4,86401,Infinity])assert.throws(()=>build({...rtc,interval:n}));for(const n of [5,86400])assert.equal(build({...rtc,interval:n}).triggers[0].intervalSec,n);assert.throws(()=>build({...sensor,interval:9}));},
    fields(){for(const s of ['', '1temp','a.b','<img>','a'.repeat(64)])assert.throws(()=>build({...sensor,property:s}));for(const s of ['_x','a'.repeat(63)])assert.equal(JSON.parse(build({...sensor,property:s}).actions[0].actionValue).dataField,s);},
    targets(){for(const target of ['', 'gone','relay','modbus:0','sensor'])assert.throws(()=>build({...rtc,target}));assert.throws(()=>build({...sensor,target:'rtc'}));},
    channels(){assert.throws(()=>build({...sensor,channel:''}));assert.throws(()=>build({...sensor,channel:'mode2'}));app._pePeripherals[1].params.mode=1;assert.equal(JSON.parse(build({...sensor,channel:'mode2'}).actions[0].actionValue).channelField,'mode2');},
    disabled(){app._pePeripherals[0].enabled=false;assert.throws(()=>build(rtc));app._pePeripherals[0].enabled=true;app._peTypeCatalog[0].featureActive=false;assert.throws(()=>build(rtc));},
    capability(){app._peTypeCatalog=[];assert.throws(()=>build(rtc));assert.throws(()=>build(sensor));},
    driver(){app._peTypeCatalog[1].driverDescriptors[0].compiled=false;assert.throws(()=>build(sensor));},
    loading(){app._peEditorDataReady=false;assert.throws(()=>build(rtc));},
    existing(){el('periph-exec-original-id').value='saved-rule';assert.throws(()=>build(rtc));},
    unknown(){for(const kind of ['','rtc-set','output','__proto__'])assert.throws(()=>build({...rtc,kind}));},
    preview(){fill(rtc);app.previewPeriphExecTemplate();assert(app._peTemplatePreview);assert.equal(el('periph-exec-name').value,'');assert.equal(el('pe-template-apply').disabled,true);assert(el('pe-template-preview').textContent.includes('MQTT'));},
    apply(){fill(sensor);app.previewPeriphExecTemplate();el('pe-template-confirm').checked=true;let actions=[],triggers=[];app._createPeriphExecTriggerElement=d=>triggers.push(d);app._createPeriphExecActionElement=d=>actions.push(d);app.applyPeriphExecTemplate();assert.equal(actions.length,1);assert.equal(actions[0].actionType,19);assert.equal(triggers[0].intervalSec,60);assert.equal(el('periph-exec-enabled').checked,false);assert.equal(app._peTemplatePreview,null);assert.equal(el('periph-exec-report').checked,true);},
    consent(){fill(rtc);app.previewPeriphExecTemplate();app.applyPeriphExecTemplate();assert.equal(el('periph-exec-triggers').replaced,undefined);},
    stale(){fill(rtc);app.previewPeriphExecTemplate();el('pe-template-confirm').checked=true;el('pe-template-interval').value='100';app.applyPeriphExecTemplate();assert.equal(el('periph-exec-triggers').replaced,undefined);assert.equal(app._peTemplatePreview,null);},
    staleCapability(){fill(rtc);app.previewPeriphExecTemplate();el('pe-template-confirm').checked=true;app._pePeripherals=[];app.applyPeriphExecTemplate();assert.equal(el('periph-exec-triggers').replaced,undefined);assert.equal(app._peTemplatePreview,null);},
    permission(){fill(rtc);app.previewPeriphExecTemplate();el('pe-template-confirm').checked=true;app.guardDeveloperModeAction=()=>false;app.applyPeriphExecTemplate();assert.equal(el('periph-exec-triggers').replaced,undefined);},
    saving(){fill(rtc);app.previewPeriphExecTemplate();el('pe-template-confirm').checked=true;app._peSavePending=true;app.applyPeriphExecTemplate();assert.equal(el('periph-exec-triggers').replaced,undefined);},
    utf8(){for(const name of ['很长的多字节名称'.repeat(8),'a'.repeat(100),'🌡️'.repeat(30)]){app._pePeripherals[0].name=name;assert(Buffer.byteLength(build(rtc).name,'utf8')<=48);}app._pePeripherals[0].id='温'.repeat(14);assert.throws(()=>build({...rtc,target:app._pePeripherals[0].id}));}
};
tests[process.argv[1]]();
'''

class PeriphExecTemplateTests(unittest.TestCase):
    def run_case(self, case):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run([node, '-e', SCRIPT, case], cwd=ROOT, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

for _case in ('modbus','modbusConsent','modbusIndex','modbusProtocol','modbusTask','modbusRange','modbusMappings','modbusStale','modbusApply',
              'rtc','sensor','interval','fields','targets','channels','disabled','capability','driver','loading','existing',
              'unknown','preview','apply','consent','stale','staleCapability','permission','saving','utf8'):
    setattr(PeriphExecTemplateTests, 'test_' + _case, lambda self, case=_case: self.run_case(case))

if __name__ == '__main__':
    unittest.main()
