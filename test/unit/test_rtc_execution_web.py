"""Exercise the actual RTC form methods in Node/VM, with no live device."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[2]
SCRIPT=r'''
const assert=require('assert');
const fs=require('fs');
const vm=require('vm');
const app={},elements={};
let blocks=[];
const escapeHtml=value=>String(value).replace(/[&<>"']/g,c=>
    ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
app.registerModule=(name,api)=>Object.assign(app,api);
const context={AppState:app,
    escapeHtml,t:(key,fallback)=>fallback,window:{},
    document:{getElementById:id=>elements[id]||null,querySelectorAll:()=>blocks}};
vm.createContext(context);
for(const name of ['periph-exec','periph-exec-modbus','periph-exec-form','periph-exec-templates'])
    vm.runInContext(fs.readFileSync('web-src/modules/runtime/'+name+'.js','utf8'),context);
app._pePeripherals=[{id:'rtc_01',type:50,name:'Clock',enabled:true},
    {id:'rtc_off',type:50,name:'<unsafe>',enabled:false},{id:'relay',type:12,name:'Relay'}];
function makeBlock(target='rtc_01',command='start',type=35) {
    const controls={'.pe-action-type':{value:String(type)},'.pe-target-periph':{value:target},
        '.pe-rtc-command':{value:command},'.pe-rtc-set-source':{value:command},'.pe-action-value':{value:'DANGEROUS_OLD_VALUE'},
        '.pe-use-received-value':{checked:true},'.pe-sync-delay':{value:'200'},
        '.pe-exec-mode':{value:'1'}};
    const block={querySelector:s=>controls[s]||null,querySelectorAll:()=>[],controls};
    return block;
}
function container() {
    elements['periph-exec-actions']={querySelectorAll:()=>blocks,innerHTML:''};
    app._isPollTriggerActive=()=>false;
}
const cases={
    choices(){
        for(const invalid of ['',undefined,'$value','START','on']) {
            const html=app._renderRtcCommandOptions(invalid);
            assert(html.includes('value="" selected'));
            assert(!html.includes('value="start" selected'));
        }
        for(const command of ['start','stop'])
            assert(app._renderRtcCommandOptions(command).includes('value="'+command+'" selected'));
    },
    targets(){
        const select={};app._populateRtcTargetSelect(select,'rtc_01');
        assert.equal(select.value,'rtc_01');assert(!select.innerHTML.includes('Relay'));
        assert(select.innerHTML.includes('&lt;unsafe&gt;'));
        assert(select.innerHTML.includes('[未启用]'));
        app._populateRtcTargetSelect(select,'modbus:0');
        assert(select.innerHTML.includes('value="modbus:0" disabled'));
        assert.equal(select.value,'modbus:0'); // Preserve invalid identity for validation.
    },
    collect(){
        container();
        for(const target of ['rtc_01','rtc_off','modbus:0','']) for(const command of ['start','stop',''])
        for(const poll of [false,true]) {
            blocks=[makeBlock(target,command)];app._isPollTriggerActive=()=>poll;
            const result=app._collectPeriphExecActions();assert.equal(result.length,1);
            assert.equal(result[0].actionType,35);assert.equal(result[0].targetPeriphId,target);
            assert.equal(result[0].actionValue,command);assert.equal(result[0].useReceivedValue,false);
            assert.equal(result[0].syncDelayMs,200);assert.equal(result[0].execMode,1);
        }
    },
    rebuild(){
        container();blocks=[makeBlock('rtc_01','stop')];let saved;
        app._createPeriphExecActionElement=data=>{saved=data;};
        app._rebuildActionBlocksForTriggerChange();
        assert.equal(saved.actionType,35);assert.equal(saved.actionValue,'stop');
        assert.equal(saved.useReceivedValue,false);assert.equal(saved.targetPeriphId,'rtc_01');
    },
    readCollect(){
        container();
        for(const target of ['rtc_01','rtc_off','modbus:0','']) for(const poll of [false,true]) {
            blocks=[makeBlock(target,'start',36)];app._isPollTriggerActive=()=>poll;
            const result=app._collectPeriphExecActions();assert.equal(result.length,1);
            assert.equal(result[0].actionType,36);assert.equal(result[0].targetPeriphId,target);
            assert.equal(result[0].actionValue,'');assert.equal(result[0].useReceivedValue,false);
        }
    },
    readRebuild(){
        container();blocks=[makeBlock('rtc_01','start',36)];let saved;
        app._createPeriphExecActionElement=data=>{saved=data;};
        app._rebuildActionBlocksForTriggerChange();
        assert.equal(saved.actionType,36);assert.equal(saved.actionValue,'');
        assert.equal(saved.useReceivedValue,false);assert.equal(saved.targetPeriphId,'rtc_01');
    },
    readModbusIsolation(){
        const block=makeBlock('modbus:0','',36);
        const option={value:'36',dataset:{},disabled:false,hidden:false};
        block.querySelectorAll=()=>[option];
        app._syncModbusActionOptions(block);assert.equal(option.disabled,false);
        app._getPeriphExecBlock=()=>block;app._onTargetPeriphChange(block.controls['.pe-target-periph'],0);
        assert.equal(block.controls['.pe-action-type'].value,'36');
    },
    setChoices(){
        for(const value of ['',undefined,'start','ntp','$value','SYSTEM-LOCAL'])
            assert(app._renderRtcSetOptions(value).includes('value="" selected'));
        assert(app._renderRtcSetOptions('system-local').includes('value="system-local" selected'));
    },
    setCollect(){
        container();
        for(const target of ['rtc_01','rtc_off','modbus:0','']) for(const value of ['', 'system-local']) {
            blocks=[makeBlock(target,value,37)];
            const result=app._collectPeriphExecActions();
            assert.equal(result[0].actionType,37);assert.equal(result[0].targetPeriphId,target);
            assert.equal(result[0].actionValue,value);assert.equal(result[0].useReceivedValue,false);
        }
    },
    setRebuild(){
        container();blocks=[makeBlock('rtc_01','system-local',37)];let saved;
        app._createPeriphExecActionElement=data=>{saved=data;};
        app._rebuildActionBlocksForTriggerChange();
        assert.equal(saved.actionType,37);assert.equal(saved.actionValue,'system-local');
        assert.equal(saved.useReceivedValue,false);assert.equal(saved.targetPeriphId,'rtc_01');
    },
    setModbusIsolation(){
        const block=makeBlock('modbus:0','system-local',37);
        const option={value:'37',dataset:{},disabled:false,hidden:false};
        block.querySelectorAll=()=>[option];
        app._syncModbusActionOptions(block);assert.equal(option.disabled,false);
        app._getPeriphExecBlock=()=>block;app._onTargetPeriphChange(block.controls['.pe-target-periph'],0);
        assert.equal(block.controls['.pe-action-type'].value,'37');
    },
    modbusIsolation(){
        const block=makeBlock('modbus:0','stop');
        const option={value:'35',dataset:{},disabled:false,hidden:false};
        block.querySelectorAll=()=>[option];
        app._syncModbusActionOptions(block);assert.equal(option.disabled,false);
        app._getPeriphExecBlock=()=>block;app._onTargetPeriphChange(block.controls['.pe-target-periph'],0);
        assert.equal(block.controls['.pe-action-type'].value,'35');
        block.controls['.pe-action-type'].value='16';app._syncModbusActionOptions(block);
        assert.equal(option.disabled,true);
    },
    noNumericHistory(){
        blocks=[makeBlock()];
        const history={checked:true,closest:()=>null};elements['periph-exec-record-history']=history;
        app._updatePeriphExecPanelOptions();assert.equal(history.checked,false);assert.equal(history.disabled,true);
        blocks[0].controls['.pe-action-type'].value='19';app._updatePeriphExecPanelOptions();
        assert.equal(history.disabled,false);
        blocks[0].controls['.pe-action-type'].value='36';app._updatePeriphExecPanelOptions();
        assert.equal(history.disabled,true);assert.equal(history.checked,false);
        blocks[0].controls['.pe-action-type'].value='37';app._updatePeriphExecPanelOptions();
        assert.equal(history.disabled,true);assert.equal(history.checked,false);
    }
};
cases[process.argv[1]]();
'''

class RtcExecutionWebTests(unittest.TestCase):
    def run_case(self,name):
        node=shutil.which('node')
        if not node:self.skipTest('Node.js required')
        result=subprocess.run([node,'-e',SCRIPT,name],cwd=ROOT,capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_command_requires_explicit_selection(self):self.run_case('choices')
    def test_targets_are_typed_and_html_escaped(self):self.run_case('targets')
    def test_collection_preserves_rtc_without_received_override(self):self.run_case('collect')
    def test_trigger_change_preserves_rtc_command(self):self.run_case('rebuild')
    def test_invalid_target_never_converts_rtc_to_modbus_control(self):self.run_case('modbusIsolation')
    def test_rtc_control_is_not_numeric_history(self):self.run_case('noNumericHistory')
    def test_read_collection_discards_hidden_old_command(self):self.run_case('readCollect')
    def test_read_survives_trigger_rebuild(self):self.run_case('readRebuild')
    def test_read_never_converts_invalid_target_to_modbus(self):self.run_case('readModbusIsolation')
    def test_set_has_no_implicit_source_selection(self):self.run_case('setChoices')
    def test_set_collection_uses_only_typed_source(self):self.run_case('setCollect')
    def test_set_survives_trigger_rebuild(self):self.run_case('setRebuild')
    def test_set_never_converts_invalid_target_to_modbus(self):self.run_case('setModbusIsolation')

if __name__=='__main__':unittest.main()
