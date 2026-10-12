"""Prepared actual form collection checks; no browser/device/network calls."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert=require('assert/strict'),fs=require('fs'),vm=require('vm');
const fields=new Map();
const item={querySelector:q=>fields.get(q)||null};
const container={querySelectorAll:()=>[item]};
const app={_isPollTriggerActive:()=>false,_pePeripherals:[{id:'uart-a',type:1},{id:'gpio',type:12}]};
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/periph-exec-form.js','utf8'),{
    AppState:app,window:{},document:{getElementById:()=>container}});
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/periph-exec-templates.js','utf8'),{
    AppState:app,document:{getElementById:()=>container}});
fields.set('.pe-action-value',{value:'  hello  '});
fields.set('.pe-action-type',{value:'29'});fields.set('.pe-target-periph',{value:'uart-a'});
const collect=()=>app._collectPeriphExecActions()[0];
const cases={
    dedicated(){assert.equal(collect().actionValue,'  hello  ');},
    generic(){fields.get('.pe-action-type').value='10';assert.equal(collect().actionValue,'  hello  ');},
    other(){fields.get('.pe-action-type').value='10';fields.get('.pe-target-periph').value='gpio';assert.equal(collect().actionValue,'hello');},
    empty_received(){fields.get('.pe-action-value').value='';fields.set('.pe-use-received-value',{checked:true});const a=collect();assert.equal(a.actionValue,'');assert.equal(a.useReceivedValue,true);},
    envelope(){fields.get('.pe-action-value').value='{"action":"send","value":"$value"}';assert.equal(collect().actionValue,fields.get('.pe-action-value').value);},
    help(){const text=app._uartSendHelp();for(const part of ['256','不自动换行','对端收到','勿盲目重试'])assert(text.includes(part));}
};
cases[process.argv[1]]();
'''


class UartActionWebTests(unittest.TestCase):
    def check(self, case):
        node = shutil.which('node')
        if not node:
            raise RuntimeError('Node required')
        result = subprocess.run([node, '-e', SCRIPT, case], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('dedicated', 'generic', 'other', 'empty_received', 'envelope', 'help'):
    setattr(UartActionWebTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
