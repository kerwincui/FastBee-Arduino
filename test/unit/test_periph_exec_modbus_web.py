"""Run the real Modbus action editor in Chromium without device/API writes."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

PROBE = r"""
const fs = require('fs');
const assert = require('assert');
const {chromium} = require('./test/browser/node_modules/playwright');
(async () => {
  const browser = await chromium.launch({headless:true});
  try {
    const page = await browser.newPage();
    await page.setContent('<div id="periph-exec-actions"></div>');
    await page.evaluate(() => {
      window.t = (_,fallback) => fallback;
      window.escapeHtml = value => String(value ?? '').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
      window.AppState = {};
    });
    for(const file of ['periph-exec-modbus.js','periph-exec-form.js'])
      await page.addScriptTag({content:fs.readFileSync('web-src/modules/runtime/'+file,'utf8')});
    await page.evaluate(() => Object.assign(AppState, {
      pollMode:false,
      _isPollTriggerActive(){return this.pollMode;},
      _hasSetModeTrigger(){return true;},
      _hiddenClass(show){return show?'':' is-hidden';},
      _setSectionVisible(el,show){if(el)el.classList.toggle('is-hidden',!show);},
      _getPeriphExecBlock(_,index){return document.querySelectorAll('.periph-exec-config-item')[index];},
      _mountPeriphExecLazyActionPanel(){},
      _updateExpanderActionAvailability(){},
      _syncRemoteActionValueState(){},
      _updatePeriphExecPanelOptions(){},
      _isNeoPixelTarget(){return false;},_isSetPwmTarget(){return false;},_isRfModuleTarget(){return false;},
      _pePeripherals:[{id:'gpio',name:'GPIO',type:4}],
      _modbusDevices:[{name:'Generic controller',deviceType:'motor',enabled:true}],
      _masterTasks:[{name:'Sample',enabled:true,slaveAddress:8,functionCode:3,startAddress:5,quantity:1}]
    }));
    for(const poll of [false,true])for(const actionType of [16,17]) {
      const value=actionType===17?'5:255':'0:1';
      const result=await page.evaluate(({poll,actionType,value})=>{
        const app=AppState;app.pollMode=poll;
        document.getElementById('periph-exec-actions').innerHTML='';
        app._createPeriphExecActionElement({targetPeriphId:'modbus:0',actionType,actionValue:value,useReceivedValue:false},0);
        const block=document.querySelector('.periph-exec-config-item');
        const visible=s=>!block.querySelector(s).classList.contains('is-hidden');
        const initial=app._collectPeriphExecActions()[0];
        app._onTargetPeriphChange(block.querySelector('.pe-target-periph'),0);
        const changed=app._collectPeriphExecActions()[0];
        return {initial,changed,type:block.querySelector('.pe-action-type').value,
          scalar:visible('.pe-action-value-group'),selector:visible('.pe-action-type-group'),
          legacy:visible('.pe-modbus-ctrl-panel'),readonly:block.querySelector('.pe-action-value').readOnly};
      },{poll,actionType,value});
      for(const wire of [result.initial,result.changed]) {
        assert.equal(wire.actionType,actionType);assert.equal(wire.actionValue,value);
        assert.equal(wire.targetPeriphId,'modbus:0');assert.equal(wire.useReceivedValue,false);
      }
      assert.equal(result.type,String(actionType));assert(result.scalar&&result.selector);
      assert(!result.legacy&&!result.readonly);
    }
    for(const poll of [false,true]) {
      const result=await page.evaluate(poll=>{
        const app=AppState;app.pollMode=poll;
        document.getElementById('periph-exec-actions').innerHTML='';
        app._createPeriphExecActionElement({targetPeriphId:'modbus:0',actionType:18,
          actionValue:JSON.stringify({ctrl:[{d:0,a:'reverse'}]}),useReceivedValue:false},0);
        const block=document.querySelector('.periph-exec-config-item');
        const legacy=app._collectPeriphExecActions()[0];
        const select=block.querySelector('.pe-action-type');select.value='17';
        app.onPeriphExecActionTypeChangeInBlock('17',0);
        block.querySelector('.pe-action-value').value='7:123';
        const direct=app._collectPeriphExecActions()[0];
        select.value='18';app.onPeriphExecActionTypeChangeInBlock('18',0);
        const restored=app._collectPeriphExecActions()[0];
        return {legacy,direct,restored};
      },poll);
      assert.equal(JSON.parse(result.legacy.actionValue).ctrl[0].a,'reverse');
      assert.equal(result.legacy.actionType,18);
      assert.equal(result.direct.actionType,17);assert.equal(result.direct.actionValue,'7:123');
      assert.equal(result.restored.actionType,18);assert(JSON.parse(result.restored.actionValue).ctrl);
    }
    console.log('Modbus direct/legacy round-trip: 6 scenarios passed');
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
"""


class ModbusWebEditorTests(unittest.TestCase):
    def test_generic_writes_and_legacy_controls_survive_real_dom_round_trip(self):
        result = subprocess.run(["node", "-e", PROBE], cwd=ROOT,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
