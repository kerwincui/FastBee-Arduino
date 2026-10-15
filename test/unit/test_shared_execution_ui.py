"""Production shared prompt behavior across both lazy loading orders; no device I/O."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const nodes=new Map();let created=0,hidden=0,warnings=[];
class Element {
 constructor(){this.value='';this.events={};this.textContent='';this.focuses=0;}
 set innerHTML(html){this.html=html;for(const match of html.matchAll(/\bid="([^"]+)"/g))nodes.set(match[1],new Element());}
 addEventListener(event,listener){(this.events[event]??=[]).push(listener);}
 emit(event,values={}){for(const listener of this.events[event]||[])listener({target:this,preventDefault(){},...values});}
 focus(){this.focuses++;}select(){}
}
const document={getElementById:id=>nodes.get(id)||null,createElement(){created++;return new Element();},
 body:{appendChild(el){nodes.set(el.id,el);}}};
const app={registerModule(name,methods){Object.assign(this,methods);},showElement(){},hideElement(){},
 showModal(modal){modal.visible=true;},hideModal(id){hidden++;const modal=nodes.get(id);if(modal)modal.visible=false;}};
const context=vm.createContext({AppState:app,document,Notification:{warning:(...args)=>warnings.push(args)},
 setTimeout:fn=>fn(),prompt:()=>null});context.window=context;
const load=file=>vm.runInContext(fs.readFileSync('web-src/'+file,'utf8'),context);
load('js/utils.js');
// B4 起执行值回退输入走异步 FBConfirm（Promise<string|null>）；桩沿用原 prompt 回退
// 语义（message、默认值两个入参），由场景自行替换 context.prompt 驱动确认/取消/空值。
context.FBConfirm=(opts)=>Promise.resolve(opts&&opts.input?context.prompt(String(opts.message),String(opts.input.value??'')):false);
const lazy=kind=>load(kind==='core'?'modules/runtime/device-control/core.js':'modules/runtime/periph-exec.js');
const input=()=>nodes.get('periph-exec-run-value-input'),button=id=>nodes.get(id);
const tests={
 async coreFirst(){lazy('core');const result=app.promptPeriphExecRunValue({defaultValue:0});assert.equal(input().value,'0');input().emit('keydown',{key:'Enter'});assert.equal(await result,'0');assert.equal(created,1);assert.equal(hidden,1);},
 async rulesFirst(){lazy('rules');const result=app.promptPeriphExecRunValue({defaultValue:'  25.5  ',ruleName:'<img>'});button('confirm-periph-exec-run-value-btn').emit('click');assert.equal(await result,'25.5');assert(nodes.get('periph-exec-run-value-help').textContent.startsWith('<img>'));},
 async crossCore(){lazy('rules');const result=app.promptPeriphExecRunValue({defaultValue:'3'}),state=app._periphExecRunPromptState;lazy('core');assert.equal(app._periphExecRunPromptState,state);button('confirm-periph-exec-run-value-btn').emit('click');assert.equal(await result,'3');assert.equal(created,1);assert.equal(hidden,1);},
 async crossRules(){lazy('core');const result=app.promptPeriphExecRunValue({defaultValue:'7'}),state=app._periphExecRunPromptState;lazy('rules');assert.equal(app._periphExecRunPromptState,state);button('confirm-periph-exec-run-value-btn').emit('click');assert.equal(await result,'7');assert.equal(created,1);},
 async replace(){lazy('core');const first=app.promptPeriphExecRunValue({defaultValue:'old'});lazy('rules');const second=app.promptPeriphExecRunValue({defaultValue:'new'});assert.equal(await first,null);button('confirm-periph-exec-run-value-btn').emit('click');assert.equal(await second,'new');assert.equal(created,1);assert.equal(hidden,1);},
 async cancel(){lazy('rules');for(const id of ['close-periph-exec-run-value-modal','cancel-periph-exec-run-value-btn','periph-exec-run-value-modal']){const result=app.promptPeriphExecRunValue({defaultValue:'8'});button(id).emit('click');assert.equal(await result,null);assert.equal(app._periphExecRunPromptState,null);}assert.equal(created,1);assert.equal(hidden,3);},
 async blank(){lazy('core');const result=app.promptPeriphExecRunValue({defaultValue:'  '}),state=app._periphExecRunPromptState;button('confirm-periph-exec-run-value-btn').emit('click');assert.equal(app._periphExecRunPromptState,state);assert(nodes.get('periph-exec-run-value-error').textContent.includes('请输入'));assert.equal(hidden,0);input().value='0';input().emit('keydown',{key:'Enter'});assert.equal(await result,'0');},
 async defaults(){lazy('core');let result=app.promptPeriphExecRunValue();assert.equal(input().placeholder,'例如: 80、25.5、1,0,0.5');app._resolvePeriphExecRunValuePrompt(null);await result;lazy('rules');result=app.promptPeriphExecRunValue();assert.equal(input().placeholder,'');app._resolvePeriphExecRunValuePrompt(null);await result;assert.equal(created,1);},
 async fallback(){document.body=null;for(const kind of ['core','rules']){lazy(kind);let options;context.prompt=(...args)=>{options=args;return '  0  ';};assert.equal(await app.promptPeriphExecRunValue({defaultValue:0}),'0');assert.equal(options[1],'0');context.prompt=()=>null;assert.equal(await app.promptPeriphExecRunValue(),null);context.prompt=()=>'';assert.equal(await app.promptPeriphExecRunValue(),null);assert.equal(warnings.at(-1)[1],kind==='core'?'执行失败':'外设执行');}assert.equal(created,0);}
};
Promise.resolve().then(()=>tests[process.argv[1]]()).then(()=>{
 console.log('SHARED_EXECUTION_UI_PASS:' + process.argv[1]);
}).catch(error=>{console.error(error);process.exitCode=1;});
'''


class SharedExecutionUITests(unittest.TestCase):
    def check(self, mode):
        result = subprocess.run(['node', '-e', SCRIPT, mode], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('SHARED_EXECUTION_UI_PASS:' + mode, result.stdout.splitlines(),
                      'Scenario did not settle: ' + result.stdout + result.stderr)


for _mode in ('coreFirst', 'rulesFirst', 'crossCore', 'crossRules', 'replace', 'cancel', 'blank', 'defaults', 'fallback'):
    setattr(SharedExecutionUITests, 'test_' + _mode, lambda self, mode=_mode: self.check(mode))
