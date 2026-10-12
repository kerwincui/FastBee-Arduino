"""Prepared actual queue-status UI and late API response lifecycle regressions."""
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
    AppState:app,window:{},document:{getElementById:id=>id==='periph-exec-report-status'?hint:id==='periph-exec-table-body'?body:null},
    apiGet:()=>new Promise((resolve,reject)=>requests.push({resolve,reject}))});
Object.assign(app,{applyDeveloperModeState(){},renderEmptyTableRow(el,n,text){el.innerHTML=text;},
    _setPeriphExecCapacity(){},_renderPeriphExecPagination(){}});
const valid=()=>({available:true,deliveryVerified:false,bindingScope:'mqtt_instance',depth:1,capacity:5,maxPayloadBytes:512,
    retryIntervalMs:5000,inFlight:false,oldestAgeMs:17000,admitted:3,evicted:1,rejectedInvalid:2,
    rejectedMemory:4,rejectedFull:5,rejectedBusy:6,contention:9,retryAccepted:7,retryRejected:8,
    oldestAttempts:2,retryDelayMs:0,retryState:'ready',lastRetryResult:'rejected',rejectedUnknownInstance:11,discardedInstanceChanged:12});
const unknown=()=>assert(hint.textContent.includes('未确认')&&!hint.textContent.includes('缓存 0/5'));
const response=status=>({success:true,data:[],total:0,pendingReports:status});
const cases={
    changed_before_retry(){const s=valid();Object.assign(s,{depth:0,oldestAttempts:0,retryDelayMs:0,retryState:'empty',lastRetryResult:'none',retryAccepted:0,retryRejected:0,admitted:0,evicted:0,discardedInstanceChanged:1});delete s.oldestAgeMs;app._renderPendingReportStatus(s);for(const text of ['缓存 0/5','实例更换丢弃 1','最近重试：尚未完成'])assert(hint.textContent.includes(text));assert(!hint.textContent.includes('最近重试：实例已更换'));},
    instance_changed(){const s=valid();s.lastRetryResult='instance_changed';app._renderPendingReportStatus(s);for(const text of ['未发送并丢弃','归属未确认 11','实例更换丢弃 12','同配置重建也不跨实例重放'])assert(hint.textContent.includes(text));},
    unknown_binding(){for(const [key,value] of [['bindingScope',undefined],['bindingScope','platform'],['rejectedUnknownInstance',undefined],['discardedInstanceChanged',-1]]){const s=valid();s[key]=value;app._renderPendingReportStatus(s);unknown();}},
    counters(){app._renderPendingReportStatus(valid());for(const s of ['缓存 1/5','17 秒','接受 7','保留 8','淘汰旧报告 1','格式/长度 2','内存 4','在途满额 5','锁忙 6','锁竞争 9','重启丢失','不代表平台收到'])assert(hint.textContent.includes(s),s);},
    empty(){const s=valid();s.depth=0;s.oldestAttempts=0;s.retryState='empty';delete s.oldestAgeMs;app._renderPendingReportStatus(s);assert(hint.textContent.includes('缓存 0/5'));},
    flight(){const s=valid();s.inFlight=true;s.retryState='in_flight';app._renderPendingReportStatus(s);assert(hint.textContent.includes('正在尝试'));},
    retry_details(){const s=valid();s.retryDelayMs=4001;s.retryState='cooldown';app._renderPendingReportStatus(s);for(const v of ['队首已尝试 2 次','冷却至少 5 秒','最近重试：拒绝，已保留'])assert(hint.textContent.includes(v));},
    bad_retry_state(){for(const [key,value] of [['retryState','in_flight'],['retryDelayMs',5001],['lastRetryResult','delivered'],['oldestAttempts',-1],['retryState',undefined]]){const s=valid();s[key]=value;app._renderPendingReportStatus(s);unknown();}},
    legacy(){app._renderPendingReportStatus(undefined);unknown();},
    unavailable(){app._renderPendingReportStatus({available:false,deliveryVerified:false});unknown();},
    malformed(){for(const [field,value] of [['depth',6],['depth',-1],['depth','1'],['inFlight','false'],['deliveryVerified',true],['capacity',9],['oldestAgeMs',-1],['oldestAgeMs',4294967296],['retryAccepted',NaN],['rejectedBusy',undefined]]){const s=valid();s[field]=value;app._renderPendingReportStatus(s);unknown();}},
    inconsistent(){const s=valid();s.depth=0;s.inFlight=true;app._renderPendingReportStatus(s);unknown();},
    async loading(){const p=app.loadPeriphExecPage();assert(hint.textContent.includes('加载中'));requests[0].resolve(response(valid()));await p;assert(hint.textContent.includes('缓存 1/5'));},
    async failed(){const p=app.loadPeriphExecPage();requests[0].reject(Error('offline'));await p;unknown();},
    async api_failed(){const p=app.loadPeriphExecPage();requests[0].resolve({success:false,pendingReports:valid()});await p;unknown();},
    async stale_success(){const a=app.loadPeriphExecPage(),b=app.loadPeriphExecPage();requests[1].resolve(response(valid()));await b;const text=hint.textContent;requests[0].resolve(response(null));await a;assert.equal(hint.textContent,text);},
    async stale_failure(){const a=app.loadPeriphExecPage(),b=app.loadPeriphExecPage();requests[1].resolve(response(valid()));await b;const text=hint.textContent;requests[0].reject(Error('late'));await a;assert.equal(hint.textContent,text);},
    async page_departure(){const p=app.loadPeriphExecPage();hooks['periph-exec'].call(app);hint.textContent='new page';requests[0].resolve(response(valid()));await p;assert.equal(hint.textContent,'new page');}
};
Promise.resolve().then(()=>cases[process.argv[1]]()).catch(e=>{console.error(e);process.exitCode=1;});
'''


class PendingReportWebTests(unittest.TestCase):
    def check(self, case):
        node = shutil.which('node')
        if not node:
            raise RuntimeError('Node required')
        result = subprocess.run([node, '-e', SCRIPT, case], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('changed_before_retry', 'instance_changed', 'unknown_binding', 'counters', 'empty', 'flight', 'retry_details', 'bad_retry_state', 'legacy', 'unavailable', 'malformed',
              'inconsistent', 'loading', 'failed', 'api_failed', 'stale_success',
              'stale_failure', 'page_departure'):
    setattr(PendingReportWebTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
