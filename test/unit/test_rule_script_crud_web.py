"""Prepared actual script editor write-outcome behavior; no browser/device access."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROBE = r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert/strict');
const mode=process.argv[1],notices=[],errors=[],requests=[];
let invalidations=0,reads=0,hidden=0,complete;
const nodes=new Map();
function node(id){if(!nodes.has(id))nodes.set(id,{value:'',checked:false,disabled:false});return nodes.get(id);}
const app={registerModule(name,methods){Object.assign(this,methods);this.setupRuleScriptEvents=()=>{};}};
async function send(url,body){
 requests.push({url,body});
 if(mode==='save_pending'||mode==='save_stale'||mode==='action_pending')return new Promise(resolve=>{complete=resolve;});
 if(mode==='save_lost'||mode==='action_lost')throw Error('network lost');
 if(mode==='save_rejected')return {success:false,error:'running rules retained'};
 if(mode==='save_numeric')return {success:1};
 return {success:true};
}
vm.runInNewContext(fs.readFileSync('web-src/modules/admin/rule-script.js','utf8'),{
 AppState:app,document:{getElementById:node},console,confirm:()=>true,
 window:{apiInvalidateCache(){++invalidations;if(mode==='cache_failure')throw Error('cache');}},
 Notification:{success(text){notices.push(text);},error(text){errors.push(text);}},apiPost:send,apiDelete:send
});
Object.assign(app,{currentPage:'rule-script',_rsEditorSeq:1,_rsEditorReady:true,
 guardDeveloperModeAction:()=>true,clearInlineError(){},showInlineError(id,text){errors.push(text);},hideModal(){++hidden;},
 loadRuleScriptPage(options){assert.equal(options.noCache,true);++reads;return Promise.resolve();}
});
node('rule-script-name').value='sample';node('rule-script-content').value='${x}';
node('rule-script-trigger-type').value='1';node('rule-script-protocol-type').value='0';
(async()=>{
 if(mode.startsWith('action_')){
  const pending=app.toggleRuleScript('a',true);await Promise.resolve();await Promise.resolve();
  if(mode==='action_pending'){
   await app.toggleRuleScript('a',true);assert.equal(requests.length,1);complete({success:true});
  }
  await pending;await Promise.resolve();await Promise.resolve();
  assert.equal(requests.length,1);assert.equal(reads,1);assert.equal(invalidations,1);assert.equal(app._rsActions.size,0);
  if(mode==='action_lost')assert(errors.some(text=>text.includes('核验')));return;
 }
 const pending=app.saveRuleScript();
 if(mode==='save_pending') {app.saveRuleScript();assert.equal(requests.length,1);complete({success:true});}
 if(mode==='save_stale') {app._rsEditorSeq=2;app._rsEditorReady=true;app.currentPage='dashboard';complete({success:false});}
 await pending;await Promise.resolve();await Promise.resolve();
 assert.equal(requests.length,1);assert.equal(invalidations,1);assert.equal(app._rsSavePending,false);
 if(mode==='save_stale'){
  assert.equal(reads,0);assert.equal(errors.length,0);assert.equal(hidden,0);assert.equal(app._rsEditorReady,true);return;
 }
 assert.equal(reads,1);
 if(['save_lost','save_rejected','save_numeric'].includes(mode)){
  assert.equal(hidden,0);assert.equal(notices.length,0);assert.equal(app._rsEditorReady,false);
  assert.equal(node('save-rule-script-btn').disabled,true);assert(errors.some(text=>text.includes('核验')));
  app.saveRuleScript();assert.equal(requests.length,1);
 }else{assert.equal(hidden,1);assert.equal(notices.length,1);assert.equal(errors.length,0);}
})().catch(error=>{console.error(error);process.exitCode=1;});
'''


class RuleScriptCrudWebTests(unittest.TestCase):
    def check(self, case):
        result = subprocess.run(['node', '-e', PROBE, case], cwd=ROOT, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('save_success', 'save_pending', 'save_stale', 'save_lost', 'save_rejected',
              'save_numeric', 'cache_failure', 'action_success', 'action_pending', 'action_lost'):
    setattr(RuleScriptCrudWebTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
