"""Prepared offline selected-bundle dependency checks; no device or network I/O."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert=require('assert/strict'),fs=require('fs'),vm=require('vm');
const app={registerModule(name,methods){Object.assign(this,methods);this.setupDeviceConfigEvents=()=>{};}};
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/device-config-transfer.js','utf8'),{AppState:app,window:{},console});
const entry=(name,value)=>({name,content:JSON.stringify(value)});
const per=(items)=>entry('peripherals.json',{peripherals:items});
const rules=(actions,extra={})=>entry('periph_exec.json',{rules:[{enabled:false,actions,...extra}]});
const action=(id,type=38)=>({actionType:type,targetPeriphId:id});
const check=files=>app._buildConfigTransferDependencyPreview(files);
const contains=(list,fragment)=>list.some(x=>x.includes(fragment));
const tests={
    typedTargets(){for(const [a,t] of [[38,50],[36,37],[29,12],[11,47],[19,12],[5,16],[39,17]]) {
        const p=check([rules([action('target',a)]),per([{id:'target',type:t,enabled:true}])]);
        assert(contains(p.errors,'类型不兼容：1'));assert(!JSON.stringify(p).includes('target'));
    }},
    typedAllowed(){for(const [a,t] of [[38,37],[36,50],[29,1],[11,45],[19,18],[5,27],[4,41]]) {
        assert.equal(check([rules([action('target',a)]),per([{id:'target',type:t,enabled:true}])]).errors.length,0);
    }},
    passivePwm(){for(const passive of [false,0,true,1]) {
        const p=check([rules([action('buzzer',4)]),per([{id:'buzzer',type:12,params:{passive}}])]);
        assert.equal(contains(p.errors,'类型不兼容'),passive===false||passive===0);
    }},
    expanderPwm(){for(const a of [3,4,5]) {
        const p=check([rules([action('logical',a)]),per([{id:'logical',type:17,params:{ioSource:1,providerId:'p'}},{id:'p',type:55}])]);
        assert(contains(p.errors,'类型不兼容：1'));
    }},
    metadataUnknown(){for(const target of [{id:'x'},{id:'x',type:'37'},{id:'x',type:12,params:{passive:'true'}},{id:'x',type:17,params:{ioSource:'1'}}]) {
        const p=check([rules([action('x',4)]),per([target])]);assert.equal(p.errors.length,0);
        assert(contains(p.warnings,'引用/来源：1'));
    }},
    pairedInputs(){for(const t of [11,13,14,18,19,20]) {
        const p=check([per([{id:'a',type:11,params:{pairedInputId:'b'}},{id:'b',type:t,enabled:true,params:{pairedInputId:'a'}}])]);
        assert.equal(p.errors.length,0);assert(contains(p.lines,'已定位引用：2'));
    }},
    invalidPairs(){for(const id of [null,7,'none','x\0y']) {
        assert(contains(check([per([{id:'a',type:11,params:{pairedInputId:id}}])]).errors,'缺失：1'));
    }
        assert(contains(check([per([{id:'a',type:11,params:{pairedInputId:'a'}}])]).errors,'引用自身：1'));
        assert(contains(check([per([{id:'a',type:11,params:{pairedInputId:'b'}},{id:'b',type:12}])]).errors,'类型不符：1'));
    },
    typedRedacted(){const secret='<img onerror=PRIVATE_TYPE>',files=[per([{id:secret,type:12,enabled:false}]),rules([action(secret,38)])],before=JSON.stringify(files);
        const p=check(files);assert(contains(p.errors,'类型不兼容'));assert(!JSON.stringify(p).includes('PRIVATE_TYPE'));
        assert.equal(JSON.stringify(files),before);
    },
    complete(){const p=check([rules([action('sd')]),per([{id:'sd',type:37,enabled:true}])]);
        assert.equal(p.errors.length,0);assert(contains(p.lines,'已定位引用：1'));},
    missing(){const p=check([rules([action('missing')]),per([])]);
        assert(contains(p.errors,'缺失：1'));},
    deselected(){const p=check([rules([action('sd')])]);assert.equal(p.errors.length,0);
        assert(contains(p.warnings,'尚未解析的引用/来源：1'));assert(contains(p.lines,'已定位引用：0'));},
    invalidTargets(){for(const id of ['',null,23,'x\0y']) {
        assert(contains(check([rules([action(id)])]).errors,'引用为空'));}},
    duplicateIds(){const p=check([per([{id:'x'},{id:'x'},{id:''},null])]);assert(contains(p.errors,'无效：3'));},
    exact(){const p=check([per([{id:'SD',enabled:true}]),rules([action('sd'),action(' SD')])]);assert(contains(p.errors,'缺失：2'));},
    disabled(){const p=check([per([{id:'sd',enabled:false}]),rules([action('sd')])]);
        assert.equal(p.errors.length,0);assert(contains(p.warnings,'未显式启用：1'));},
    numericEnabled(){const p=check([per([{id:'sd',enabled:1}]),rules([action('sd')])]);assert(!contains(p.warnings,'未显式启用'));},
    namespaces(){const actions=[action('modbus:0',16),action('rule',22),action('event',21),action('weather',34),action('rule',23)];
        const p=check([per([]),rules(actions,{triggers:[{triggerType:0,triggerPeriphId:'property'},{triggerType:4,triggerPeriphId:'event-source'},{triggerType:1}]})]);
        assert.equal(p.errors.length,0);assert(contains(p.warnings,'引用/来源：7'));},
    parents(){const p=check([per([{id:'child',type:12,params:{ioSource:1,providerId:'provider'}},
        {id:'provider',type:55,enabled:true,params:{busPeripheralId:'bus'}},{id:'bus',type:2,enabled:true},
        {id:'sensor',type:38,params:{busPeripheralId:'bus'}}])]);assert.equal(p.errors.length,0);assert(contains(p.lines,'已定位引用：3'));},
    wrongParents(){const p=check([per([{id:'child',type:12,params:{ioSource:1,providerId:'wrong'}},
        {id:'provider',type:55,params:{busPeripheralId:'wrong'}},{id:'wrong',type:37}])]);assert(contains(p.errors,'类型不符：2'));},
    directBus(){const p=check([per([{id:'provider',type:55,params:{busPeripheralId:''}},{id:'sensor',type:38,params:{}}])]);assert.equal(p.errors.length,0);},
    unknownShape(){const p=check([entry('peripherals.json',null),entry('periph_exec.json',{rules:{}})]);
        assert.equal(p.errors.length,0);assert(contains(p.warnings,'未建立引用索引'));assert(contains(p.warnings,'未解析动作引用'));},
    unknownActions(){const p=check([rules([{actionType:999},{actionType:'38'},null])]);assert.equal(p.errors.length,0);assert(contains(p.warnings,'引用/来源：3'));},
    duplicateFiles(){const p=check([per([]),per([])]);assert(contains(p.errors,'重复文件名'));},
    redacted(){const secret='<img onerror=SECRET>',files=[per([{id:secret,enabled:true}]),rules([action(secret)]),entry('network.json',{password:secret})];
        const before=JSON.stringify(files),p=check(files);assert.equal(JSON.stringify(files),before);assert(!JSON.stringify(p).includes(secret));assert(!JSON.stringify(p).includes('SECRET'));},
    prototypeIds(){const p=check([per([{id:'__proto__',enabled:true},{id:'constructor',enabled:true}]),rules([action('__proto__'),action('constructor')])]);
        assert.equal(p.errors.length,0);assert(contains(p.lines,'已定位引用：2'));},
    contract(){const header=fs.readFileSync('include/core/PeripheralExecution.h','utf8');
        const values=new Map([...header.matchAll(/\b(ACTION_[A-Z0-9_]+)\s*=\s*(\d+)/g)].map(m=>[m[1],Number(m[2])]));
        const cpp=fs.readFileSync('include/core/PeripheralActionContract.h','utf8').split('inline bool isSensorTarget')[0];
        const required=new Set([...cpp.matchAll(/case ExecActionType::(ACTION_[A-Z0-9_]+):/g)].map(m=>values.get(m[1])));
        for(const id of values.values())assert.equal(check([per([]),rules([action('',id)])]).errors.length>0,required.has(id),'action '+id);
    }
};
tests[process.argv[1]]();
'''


class ConfigTransferDependencyTests(unittest.TestCase):
    def run_case(self, name):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run([node, '-e', SCRIPT, name], cwd=ROOT,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _name in ('typedTargets', 'typedAllowed', 'passivePwm', 'expanderPwm', 'metadataUnknown', 'pairedInputs', 'invalidPairs', 'typedRedacted',
              'complete', 'missing', 'deselected', 'invalidTargets', 'duplicateIds',
              'exact', 'disabled', 'numericEnabled', 'namespaces', 'parents',
              'wrongParents', 'directBus', 'unknownShape', 'unknownActions',
              'duplicateFiles', 'redacted', 'prototypeIds', 'contract'):
    setattr(ConfigTransferDependencyTests, 'test_' + _name,
            lambda self, name=_name: self.run_case(name))


if __name__ == '__main__':
    unittest.main()
