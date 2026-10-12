"""Prepared UART diagnostics renderer and existing request lifecycle regressions."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert=require('assert/strict'),fs=require('fs'),vm=require('vm');
const hint={textContent:''},body={innerHTML:'',addEventListener(){}},hooks={},requests=[];
const app={registerModule(name,methods){Object.assign(this,methods);},
    onPageHide(name,fn){hooks[name]=fn;}};
vm.runInNewContext(fs.readFileSync('web-src/js/utils.js','utf8')+'\n'+fs.readFileSync('web-src/modules/runtime/periph-exec.js','utf8'),{
    AppState:app,window:{},document:{getElementById:id=>id==='periph-exec-uart-text-status'?hint:id==='periph-exec-table-body'?body:null},
    apiGet:()=>new Promise((resolve,reject)=>requests.push({resolve,reject}))});
Object.assign(app,{applyDeveloperModeState(){},renderEmptyTableRow(el,n,text){el.innerHTML=text;},
    _setPeriphExecCapacity(){},_renderPeriphExecPagination(){}});
const fields=['dispatchAttempts','oversizedFrames','invalidByteFrames','readFailures','sourceChanges','staleFrames','serializationFailures','hardwareFifoFaults','hardwareBufferFaults','hardwareFrameFaults','hardwareParityFaults','hardwareBreakFaults'];
const valid=()=>({available:true,scope:'boot',unit:'events',peerVerified:false,maxFrameBytes:160,
    idleBoundaryMs:500,countsAreLowerBounds:false,hardwareErrorScope:'first_observed_generic_stream_fault',
    hardwareErrorRecovery:'explicit_restart',hardwareErrorDeliveryConfirmed:false,...Object.fromEntries(fields.map((key,i)=>[key,i+1]))});
const unknown=()=>assert(hint.textContent.includes('未确认')&&hint.textContent.includes('不能判断没有丢帧'));
const response=status=>({success:true,data:[],total:0,uartText:status});
const cases={
    counters(){app._renderUartTextStatus(valid());for(const s of ['本次启动累计','提交规则匹配尝试 1','超长帧 2','无效字节帧 3','读取失败 4','来源变化观察 5','发布时来源未确认 6','序列化失败 7','硬件首故障 FIFO 8','缓冲满 9','帧错误 10','奇偶错误 11','BREAK 12','接收锁存','不会重放旧数据','SDK错误通知可能延迟或缺失','不可相加','不代表规则已执行或对端验证'])assert(hint.textContent.includes(s),s);},
    zero(){const s=valid();fields.forEach(k=>s[k]=0);app._renderUartTextStatus(s);assert(hint.textContent.includes('零计数不证明通信正常'));assert(hint.textContent.includes('仅统计规则文本接收'));},
    legacy(){app._renderUartTextStatus(undefined);unknown();app._renderUartTextStatus({available:false});unknown();},
    malformed(){for(const key of fields)for(const value of [undefined,-1,0.5,'1',NaN,Infinity,4294967296,'<img src=x onerror=alert(1)>']){const s=valid();s[key]=value;app._renderUartTextStatus(s);unknown();assert(!hint.textContent.includes('<img'));}},
    metadata(){for(const [key,value] of [['available',1],['scope','lifetime'],['unit','frames'],['peerVerified',true],['maxFrameBytes',159],['idleBoundaryMs',499],['countsAreLowerBounds','false'],['hardwareErrorScope',undefined],['hardwareErrorRecovery','auto'],['hardwareErrorDeliveryConfirmed',true]]){const s=valid();s[key]=value;app._renderUartTextStatus(s);unknown();}},
    saturated(){for(const key of fields){const s=valid();s[key]=4294967295;app._renderUartTextStatus(s);unknown();s.countsAreLowerBounds=true;app._renderUartTextStatus(s);assert(hint.textContent.includes('计数为下限'));assert(hint.textContent.includes('4294967295'));}},
    async loading(){const p=app.loadPeriphExecPage();assert(hint.textContent.includes('加载中'));requests[0].resolve(response(valid()));await p;assert(hint.textContent.includes('超长帧 2'));assert.equal(requests.length,1);},
    async failed(){const p=app.loadPeriphExecPage();requests[0].reject(Error('private-wire-data'));await p;unknown();assert(!hint.textContent.includes('private-wire-data'));},
    async api_failed(){const p=app.loadPeriphExecPage();requests[0].resolve({success:false,uartText:valid()});await p;unknown();},
    async stale_success(){const a=app.loadPeriphExecPage(),b=app.loadPeriphExecPage();requests[1].resolve(response(valid()));await b;const text=hint.textContent;requests[0].resolve(response(null));await a;assert.equal(hint.textContent,text);},
    async stale_failure(){const a=app.loadPeriphExecPage(),b=app.loadPeriphExecPage();requests[1].resolve(response(valid()));await b;const text=hint.textContent;requests[0].reject(Error('late'));await a;assert.equal(hint.textContent,text);},
    async page_departure(){const p=app.loadPeriphExecPage();hooks['periph-exec'].call(app);hint.textContent='new page';requests[0].resolve(response(valid()));await p;assert.equal(hint.textContent,'new page');}
};
Promise.resolve().then(()=>cases[process.argv[1]]()).catch(e=>{console.error(e);process.exitCode=1;});
'''


class UartTextStatusWebTests(unittest.TestCase):
    def check(self, case):
        node = shutil.which('node')
        if not node:
            raise RuntimeError('Node required')
        result = subprocess.run([node, '-e', SCRIPT, case], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('counters', 'zero', 'legacy', 'malformed', 'metadata', 'saturated', 'loading',
              'failed', 'api_failed', 'stale_success', 'stale_failure', 'page_departure'):
    setattr(UartTextStatusWebTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
