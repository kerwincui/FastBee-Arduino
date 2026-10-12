"""Local risk summary must not leak imported secrets or imply compatibility."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert=require('assert/strict'), fs=require('fs'), vm=require('vm');
const app={registerModule(name,methods){Object.assign(this,methods);this.setupDeviceConfigEvents=()=>{};}};
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/device-config-transfer.js','utf8'), {AppState:app,window:{},TextEncoder,Blob,console});
const preview=(name,config)=>app._buildConfigTransferPreview({name,content:JSON.stringify(config)});
const tests={
    private(){for(const name of ['device.json','network.json','protocol.json','users.json','auth.json']) {
        const p=preview(name,{password:'TOP_SECRET_123',token:'TOKEN_SECRET_456',deviceNum:'IDENTITY_PRIVATE',apSSID:'PRIVATE_SSID'}),text=JSON.stringify(p);
        for(const secret of ['TOP_SECRET_123','TOKEN_SECRET_456','IDENTITY_PRIVATE','PRIVATE_SSID'])assert(!text.includes(secret));
        assert(p.warnings.some(s=>s.includes(name==='device.json'?'保留目标值':'取消选择')));
    }},
    peripherals(){const p=preview('peripherals.json',{peripherals:[{id:'p1',enabled:true},{id:'p1',enabled:false},{enabled:false}]});
        assert(p.lines[0].includes('3'));assert(p.lines[0].includes('显式启用：1'));assert(p.warnings.some(s=>s.includes('重复')));assert(!JSON.stringify(p).includes('p1'));},
    rules(){const p=preview('periph_exec.json',{rules:[{id:'PRIVATE_ID',enabled:true,actions:[{actionType:36,targetPeriphId:'PRIVATE_TARGET'}]}]});
        assert(p.warnings.some(s=>s.includes('自动执行')));assert(p.warnings.some(s=>s.includes('旧固件')));
        assert(!JSON.stringify(p).includes('PRIVATE'));assert(p.lines[0].includes('显式启用：1'));},
    defaultEnabled(){const p=preview('periph_exec.json',{rules:[{actions:[]}]});assert(p.warnings.some(s=>s.includes('不推断为禁用')));},
    numericEnabled(){const p=preview('periph_exec.json',{rules:[{enabled:1,actions:[]},{enabled:0,actions:[]}]});assert(p.lines[0].includes('显式启用：1'));assert(p.warnings.some(s=>s.includes('自动执行')));assert(!p.warnings.some(s=>s.includes('不推断为禁用')));},
    features(){const p=preview('features.json',{features:{mqtt:true,screenPanel:false}});assert(p.lines[0].includes('2'));assert(p.warnings.some(s=>s.includes('预算')));},
    root(){for(const value of [null,[],42,'secret']) {const p=preview('device.json',value);assert(p.warnings.some(s=>s.includes('结构未验证')));assert(!JSON.stringify(p).includes('secret'));}},
    malformed(){assert.throws(()=>app._buildConfigTransferPreview({name:'device.json',content:'{'}));assert.throws(()=>app._buildConfigTransferPreview({name:'../secret.json',content:'{}'}));},
    oversized(){assert.throws(()=>preview('device.json',{a:'x'.repeat(128*1024)}));},
    xss(){const p=preview('peripherals.json',{peripherals:[{id:'<img onerror=alert(1)>',name:'<script>bad</script>',pins:[1]}]});assert(!JSON.stringify(p).includes('<'));},
    unknown(){const p=preview('custom.json',{secret:'NEVER_RENDER'});assert(!JSON.stringify(p).includes('NEVER_RENDER'));assert(p.warnings.some(s=>s.includes('服务端')));},
    nonMutation(){const c={peripherals:[{id:'p',enabled:true,pins:[1,2]}]},before=JSON.stringify(c);preview('peripherals.json',c);assert.equal(JSON.stringify(c),before);},
    shape(){for(const name of ['peripherals.json','periph_exec.json','features.json'])assert(JSON.stringify(preview(name,{})).includes('验证'));}
};
tests[process.argv[1]]();
'''

class ConfigMigrationPreviewTests(unittest.TestCase):
    def run_case(self, name):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run([node, '-e', SCRIPT, name], cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

for _name in ('private','peripherals','rules','defaultEnabled','numericEnabled','features','root','malformed','oversized','xss','unknown','nonMutation','shape'):
    setattr(ConfigMigrationPreviewTests, 'test_' + _name, lambda self, name=_name: self.run_case(name))

if __name__ == '__main__':
    unittest.main()
