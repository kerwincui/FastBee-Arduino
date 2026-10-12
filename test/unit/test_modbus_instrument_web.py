"""Actual Web codec, versioned instrument parser and disabled-draft lifecycle."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const els=new Map(),el=id=>{if(!els.has(id))els.set(id,{value:'',files:[],checked:false,disabled:false,textContent:''});return els.get(id);};
let warnings=[];
const app={_masterTasks:[],_modbusDevices:[],_protocolConfig:{},guardDeveloperModeAction:()=>true,_renderAllDevices(){}};
const context={AppState:app,document:{getElementById:el},TextEncoder,URL,Notification:{warning:m=>warnings.push(m)},console};
context.window=context;vm.createContext(context);
for(const file of ['web-src/js/utils.js','web-src/modules/runtime/protocol/modbus-config.js','web-src/modules/runtime/protocol/modbus-instruments.js'])vm.runInContext(fs.readFileSync(file,'utf8'),context);
app._renderAllDevices=()=>{};
const plan=()=>({type:'fastbee-modbus-instrument',version:1,templateId:'test-v1',model:'Fixture',revision:'v1',hardwareVerified:false,
 source:'https://example.invalid/manual.pdf',sourcePages:'1',name:'Meter',read:{functionCode:4,startAddress:0,quantity:2,
 mappings:[{regOffset:0,dataType:4,wordOrder:1,scaleFactor:1,decimalPlaces:1,sensorId:'voltage',unit:'V'}]}});
const build=(p=plan(),address=1)=>app._buildModbusInstrumentDraft(p,address);
const reset=()=>{el('mb-template-kind').value='sdm120';el('mb-template-address').value='3';el('rtu-peripheral-id').value='uart1';el('rtu-transfer-type').value='0';};
reset();
const tests={
 codec(){const d=context.decodeModbusMapping;assert.equal(d([65535,65535],{dataType:2}),4294967295);assert.equal(d([0,0xc148],{dataType:4,wordOrder:1}),-12.5);assert.equal(d([1,2],{dataType:2,wordOrder:1}),131073);assert.equal(d([65535,65534],{dataType:3}),-2);assert.equal(d([12],{scaleFactor:0}),0);assert.equal(context.formatModbusMapping([12],{decimalPlaces:0}),'12');},
 invalidCodec(){for(const m of [null,{regOffset:null},{dataType:null},{wordOrder:null},{dataType:5},{wordOrder:2},{wordOrder:1},{dataType:4},{regOffset:2},{scaleFactor:NaN},{scaleFactor:null},{scaleFactor:1e50},{scaleFactor:1e-50}])assert.equal(context.decodeModbusMapping([1],m),null);assert.equal(context.decodeModbusMapping([0,0x7fc0],{dataType:4,wordOrder:1}),null);assert.equal(context.decodeModbusMapping([true],{}),null);assert.equal(context.formatModbusMapping([1],{decimalPlaces:null}),'--');},
 schema(){const d=build();assert.equal(d.enabled,false);assert.equal(d.functionCode,4);assert.equal(d.mappings[0].wordOrder,1);assert.equal(d.slaveAddress,1);assert.equal(Object.hasOwn(d,'devices'),false);},
 versions(){for(const patch of [{version:2},{version:'1'},{type:'other'},{hardwareVerified:true},{model:''},{revision:''},{source:'javascript:alert(1)'},{source:'https://u:p@example.invalid/'},{sourcePages:''},{devices:[]}])assert.throws(()=>build({...plan(),...patch}));},
 reads(){for(const patch of [{functionCode:6},{functionCode:16},{quantity:33},{quantity:0},{startAddress:65535},{mappings:[]},{ctrl:[]}]){const p=plan();Object.assign(p.read,patch);assert.throws(()=>build(p));}for(const address of [0,248,'1',1.5,NaN])assert.throws(()=>build(plan(),address));},
 fields(){for(const patch of [{regOffset:1},{dataType:0},{wordOrder:2},{unit:'12345678'},{sensorId:'x"x'},{sensorId:'1234567890123456'},{scaleFactor:null},{decimalPlaces:7},{command:'write'}]){const p=plan();Object.assign(p.read.mappings[0],patch);assert.throws(()=>build(p));}const p=plan();p.read.mappings.push({...p.read.mappings[0]});assert.throws(()=>build(p));},
 async previewApply(){await app.previewModbusInstrument();assert.equal(app._masterTasks.length,0);assert(el('mb-template-preview').textContent.includes('V2.4'));assert.equal(app.applyModbusInstrument(),false);el('mb-template-confirm').checked=true;assert.equal(app.applyModbusInstrument(),true);assert.equal(app._masterTasks.length,1);assert.equal(app._masterTasks[0].enabled,false);assert.equal(app._masterTasks[0].slaveAddress,3);assert.equal(app.applyModbusInstrument(),false);},
 async stale(){await app.previewModbusInstrument();el('mb-template-confirm').checked=true;app._modbusDevices.push({enabled:false});assert.equal(app.applyModbusInstrument(),false);assert.equal(app._masterTasks.length,0);},
 async duplicate(){app._masterTasks=[{mappings:[{sensorId:'voltage'}]}];await app.previewModbusInstrument();assert.equal(app._mbInstrumentReview,null);assert(el('mb-template-preview').textContent.includes('字段'));},
 async mode(){el('rtu-transfer-type').value='1';await app.previewModbusInstrument();assert.equal(app._mbInstrumentReview,null);reset();el('rtu-peripheral-id').value='';await app.previewModbusInstrument();assert.equal(app._mbInstrumentReview,null);},
 async file(){el('mb-template-kind').value='file';el('mb-template-file').files=[{size:1024,text:async()=>JSON.stringify(plan())}];await app.previewModbusInstrument();assert(el('mb-template-preview').textContent.includes('外部模板'));el('mb-template-confirm').checked=true;assert.equal(app.applyModbusInstrument(),true);assert.equal(app._masterTasks[0].mappings[0].wordOrder,1);},
 async late(){let done;el('mb-template-kind').value='file';el('mb-template-file').files=[{size:1024,text:()=>new Promise(resolve=>{done=resolve;})}];const pending=app.previewModbusInstrument();el('mb-template-address').value='4';done(JSON.stringify(plan()));await pending;assert.equal(app._mbInstrumentReview,null);assert.equal(app._masterTasks.length,0);},
 zeroForm(){app._masterTasks=[{quantity:2}];app._currentMappingTaskIdx=0;app._currentMappings=[{}];const inputs=['0','4','0','0','voltage','V','1'].map(value=>({value}));el('mapping-table-body').querySelectorAll=()=>[{querySelectorAll:()=>inputs}];assert.equal(app._collectMappingValues(),true);assert.equal(app._currentMappings[0].scaleFactor,0);assert.equal(app._currentMappings[0].decimalPlaces,0);const before=JSON.stringify(app._currentMappings);inputs[2].value='';assert.equal(app._collectMappingValues(),false);assert.equal(JSON.stringify(app._currentMappings),before);},
 attribute(){app.applyDeveloperModeState=()=>{};app._currentMappings=[{sensorId:'"><img src=x onerror=1>',unit:'" autofocus="',regOffset:'" bad="',dataType:0}];app._renderMappingTable();const html=el('mapping-table-body').innerHTML;assert(!html.includes('<img'));assert(html.includes('&quot;'));assert(!html.includes('value="" autofocus'));}
};
Promise.resolve().then(()=>tests[process.argv[1]]()).catch(error=>{console.error(error);process.exitCode=1;});
'''


class ModbusInstrumentWebTests(unittest.TestCase):
    def check(self, mode):
        result = subprocess.run([shutil.which('node') or 'node', '-e', SCRIPT, mode], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


for _mode in ('codec','invalidCodec','schema','versions','reads','fields','previewApply','stale','duplicate','mode','file','late','zeroForm','attribute'):
    setattr(ModbusInstrumentWebTests, 'test_' + _mode, lambda self, mode=_mode: self.check(mode))
