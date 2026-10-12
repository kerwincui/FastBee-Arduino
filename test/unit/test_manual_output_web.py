"""Actual shared manual-output helper and both page consumers, without a device."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert=require('assert/strict'),fs=require('fs'),vm=require('vm');
const messages=[],reads=[],posts=[],timers=[];
let responses=[],postResult={success:true,operationId:42,state:'queued'},postError=null;
let pageGeneration=1,onRead=null;
const app={currentPage:'periph-exec',registerModule(name,methods){Object.assign(this,methods);},onPageHide(){}};
const sandbox={AppState:app,window:{apiGetGovernorState:()=>({pageGeneration})},
 document:{getElementById:id=>id==='device-control-page'?{classList:{contains:()=>true}}:null,querySelectorAll:()=>[]},console:{error(){}},
 Notification:{success:text=>messages.push(['success',text]),error:text=>messages.push(['error',text])},
 setTimeout(fn,delay){timers.push(delay);queueMicrotask(fn);return timers.length;},clearTimeout(){},
 apiPost:async(url,payload)=>{posts.push({url,payload});if(postError)throw postError;return postResult;},
 apiGetFresh:async(url,params)=>{reads.push({url,params});if(onRead)onRead();const response=responses.shift();
  if(response instanceof Error)throw response;return response;}};
vm.createContext(sandbox);
for(const path of ['web-src/js/utils.js','web-src/modules/runtime/periph-exec.js','web-src/modules/runtime/device-control/core.js'])
 vm.runInContext(fs.readFileSync(path,'utf8'),sandbox,{filename:path});
const helper=sandbox.window.PeriphExecRunValuePrompt;
const result=state=>({success:true,data:[{operationId:42,state}]});
const conflict={targetId:'relay',evaluated:true,reason:'CONTROL_CONFLICT',source:'rule',holdRemainingMs:1001,generation:'18446744073709551615'};
const pending=()=>helper._observeManualOutput({success:true,operationId:42,state:'queued'},()=>true);
const cases={
 async third_success(){responses=[result('queued'),result('running'),result('success')];await pending();
  assert.equal(reads.length,3);assert(messages.at(-1)[1].includes('执行完成'));assert(messages.at(-1)[1].includes('物理负载'));},
 async three_pending(){responses=[result('queued'),result('running'),result('running')];await pending();
  assert.equal(reads.length,3);assert(messages.at(-1)[1].includes('尚未确认完成'));assert.equal(posts.length,0);
  assert.deepEqual(timers,[400,800,1200]);},
 async third_failure(){responses=[result('queued'),result('running'),{success:true,data:[{operationId:42,state:'failed',control:conflict}]}];
  await pending();assert.equal(reads.length,3);for(const word of ['占用','relay','规则','2秒','不会自动重试'])assert(messages.at(-1)[1].includes(word));},
 async network_unknown(){responses=[new Error('private-network-details')];await pending();assert.equal(reads.length,1);
  assert(messages.at(-1)[1].includes('未确认'));assert(!messages.at(-1)[1].includes('private-network'));assert.equal(posts.length,0);},
 async mismatched_operation(){responses=[{success:true,data:[{operationId:43,state:'success'}]}];await pending();
  assert.equal(reads.length,1);assert(messages.at(-1)[1].includes('未确认'));},
 async conflict(){await helper._observeManualOutput({success:false,control:conflict},()=>true);
  assert.equal(reads.length,0);assert.equal(posts.length,0);assert(messages.at(-1)[1].includes('不会自动重试'));},
 async inactive(){await helper._observeManualOutput({success:false,control:conflict},()=>false);assert.equal(messages.length,0);},
 async departed(){let active=true;responses=[result('success')];onRead=()=>{active=false;};
  await helper._observeManualOutput(postResult,()=>active);assert.equal(messages.length,1);assert.equal(reads.length,1);},
 async rule_duplicate(){responses=[result('success')];await Promise.all([app.runPeriphExecOnce('r',false),app.runPeriphExecOnce('r',false)]);
  assert.equal(posts.length,1);assert.equal(reads.length,1);assert.equal(app._peManualRuns.size,0);
  assert.equal(posts[0].payload.confirmOverride,undefined);assert.equal(posts[0].payload.actor,undefined);},
 async rule_conflict(){postResult={success:false,state:'failed',control:conflict};await app.runPeriphExecOnce('r',false);
  assert.equal(posts.length,1);assert.equal(reads.length,0);assert(messages.at(-1)[1].includes('占用'));},
 async rule_unknown(){postError=new TypeError('Failed to fetch');await app.runPeriphExecOnce('r',false);
  assert.equal(posts.length,1);assert.equal(reads.length,0);assert(messages.at(-1)[1].includes('不会自动重试'));},
 async rule_departure(){responses=[result('success')];onRead=()=>{++pageGeneration;};await app.runPeriphExecOnce('r',false);
  assert.equal(posts.length,1);assert.equal(messages.length,1);},
 async device_duplicate(){app.currentPage='device-control';app._dcLifecycleGeneration=7;app._featureActive={deviceControl:true};
  const button={disabled:false,getAttribute:key=>key==='data-id'?'r':'false'};
  responses=[result('queued'),result('running'),result('success')];
  await Promise.all([app._executeRule('r',button),app._executeRule('r',button)]);
  assert.equal(posts.length,1);assert.equal(reads.length,3);assert.equal(app._dcRuleRuns.size,0);
  assert(messages.at(-1)[1].includes('执行完成'));},
 async motor_unknown(){
  await helper._observeManualOutput({success:false,control:{attemptKnown:false,attempted:false,reason:'CONTROL_NOT_ATTEMPTED',
    backendOperationId:42,acknowledgedSteps:2}},()=>true);
  assert.equal(posts.length,0);assert.equal(reads.length,0);
  for(const word of ['待确认','是否已执行尚未知','电机操作 42','已应答 2 步','不会自动重试'])assert(messages.at(-1)[1].includes(word));
  assert(!messages.at(-1)[1].includes('未尝试'));
 },
 reason_fallback(){assert(helper._manualOutputMessage({errorCode:'cancelled',control:{reason:'CONTROL_NOT_ATTEMPTED'}}).includes('cancelled'));
  assert(helper._manualOutputMessage({data:{control:conflict}}).includes('relay'));}
};
Promise.resolve().then(()=>cases[process.argv[1]]()).catch(error=>{console.error(error);process.exitCode=1;});
'''


class ManualOutputWebTests(unittest.TestCase):
    def check(self, case):
        node = shutil.which('node')
        if not node:
            raise RuntimeError('Node required')
        result = subprocess.run([node, '-e', SCRIPT, case], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('third_success', 'three_pending', 'third_failure', 'network_unknown',
              'mismatched_operation', 'conflict', 'inactive', 'departed', 'rule_duplicate',
              'rule_conflict', 'rule_unknown', 'rule_departure', 'device_duplicate', 'reason_fallback', 'motor_unknown'):
    setattr(ManualOutputWebTests, 'test_' + _case, lambda self, case=_case: self.check(case))
