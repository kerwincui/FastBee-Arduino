"""Prepared MQTT queue visibility behavior tests; no server or device requests."""
import subprocess
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROBE = r'''
const fs=require('fs'), vm=require('vm'), assert=require('assert');
const mode=process.argv[2], element={textContent:'',title:''}, pending=[];
const box={AppState:{},window:{isPageAbortError:()=>false},console,
 document:{getElementById:id=>id==='mqtt-status-report-queue'?element:null,querySelector:()=>null},
 setTimeout:()=>1,clearTimeout:()=>{},clearInterval:()=>{},
 apiGetSilent:()=>new Promise((resolve,reject)=>pending.push({resolve,reject}))};
vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),box);
const app=box.AppState;
const q={available:true,deliveryVerified:false,capacity:9,ringCapacity:8,depth:1,ringDepth:0,
 pendingReport:true,consumerActive:false,pendingAttempts:2,retryDelayMs:500,accepted:1,
 publishedLocally:0,evictedOldest:3,rejectedInvalid:4,rejectedThrottle:5,publishRejected:6,allocationDeferred:7,rejectedTestMode:8};
const render=(queue=q,connected=true)=>app._formatMqttReportQueueStatus(queue,connected);
const history={scope:'boot',persisted:false,payloadRetained:false,instances:3,instancesWithLoss:1,
 discardedReports:2,unconfirmedInstances:0,saturated:false,lastLossReason:'network_switch',
 lastLossRingDepth:1,lastLossPendingReport:true,
 byReason:{unspecified:0,configuration_disabled:0,rebuild:0,network_switch:2,shutdown:0,initialization_failed:0}};
(async()=>{
 if(mode==='history_empty_current') {
  const text=render({...q,depth:0,ringDepth:0,pendingReport:false,pendingAttempts:0,retryDelayMs:0,retirement:history});
  for(const part of ['0/9 · 空','本次启动销毁丢弃2','网络切换2','含失败保留报告','重启清零'])assert(text.includes(part));
 } else if(mode==='history_unavailable') {
  const text=render({available:false,retirement:history});assert(text.startsWith('未确认'));
  assert(text.includes('本次启动销毁丢弃2'));assert(!text.includes('0/9'));
 } else if(mode==='history_invalid') {
  for(const delta of [{scope:'persisted'},{persisted:true},{payloadRetained:true},{discardedReports:'2'},
   {discardedReports:3},{lastLossReason:'__proto__'},{lastLossRingDepth:9},{instancesWithLoss:5},
   {lastLossRingDepth:0,lastLossPendingReport:false},{byReason:{network_switch:2}}]) {
   const text=render({...q,retirement:{...history,...delta}});
   assert(text.includes('销毁历史未确认'));assert(!text.includes('本次启动销毁丢弃'));
  }
 } else if(mode==='history_saturation') {
  const text=render({...q,retirement:{...history,saturated:true,discardedReports:4294967295,
   byReason:{...history.byReason,network_switch:4294967295}}});
  assert(text.includes('本次启动销毁丢弃至少4294967295'));
 } else if(mode==='history_uncertain') {
  const text=render({...q,retirement:{...history,unconfirmedInstances:1}});
  assert(text.includes('另有1次销毁数量未确认'));assert(text.includes('销毁丢弃2'));
 } else if(mode==='retained') { const text=render();assert(text.includes('保留报告尝试2次'));assert(text.includes('等待至少1秒'));assert(text.includes('平台收件未验证')); }
 else if(mode==='counters') { const text=render();for(const part of ['淘汰3','格式拒绝4','限流拒绝5','发布失败6','内存推迟7','测试隔离拒绝8'])assert(text.includes(part)); }
 else if(mode==='configured_topics') {
  assert(render({...q,configuredTopicAccepted:1}).includes('指定主题累计入队1'));
  for(const value of [undefined,null,'1',-1,1.5,2,Infinity]) {
   const text=render({...q,configuredTopicAccepted:value});
   assert(text.includes('指定主题入队数未确认') && !text.includes('指定主题累计入队'));
  }
 }
 else if(mode==='offline') assert(render(q,false).includes('等待连接'));
 else if(mode==='active') { q.consumerActive=true;q.retryDelayMs=0;assert(render().includes('处理中')); }
 else if(mode==='empty') { q.depth=0;q.pendingReport=false;q.pendingAttempts=0;q.retryDelayMs=0;assert(render().includes('0/9 · 空')); }
 else if(mode==='unknown') { assert(render(null).startsWith('未确认'));assert(render({available:false}).startsWith('未确认'));delete q.accepted;assert(render().startsWith('未确认')); }
 else if(mode==='contradiction') { for(const change of [{depth:2},{ringDepth:9},{pendingReport:false},{deliveryVerified:true},{retryDelayMs:1001},{consumerActive:true},{pendingAttempts:-1},{rejectedInvalid:'4'},{capacity:8}])assert(render({...q,...change}).startsWith('未确认')); }
 else if(mode==='dom') { app._updateMqttReportQueueUI({reportQueue:q,connected:true});assert(element.textContent===render());assert(element.title===element.textContent);app._updateMqttReportQueueUI(null);assert(element.textContent.startsWith('未确认')); }
 else if(mode==='request_failure') { app._updateMqttReportQueueUI({reportQueue:q,connected:true});app._loadMqttStatus();pending[0].resolve({success:false});await Promise.resolve();await Promise.resolve();assert(element.textContent.startsWith('未确认')); }
 else if(mode==='handled_failure') { app._updateMqttReportQueueUI({reportQueue:q,connected:true});app._loadMqttStatus();pending[0].reject({_handled:true,status:403});await Promise.resolve();await Promise.resolve();assert(element.textContent.startsWith('未确认')); }
 else if(mode==='stale') { app._loadMqttStatus();app._loadMqttStatus();pending[1].resolve({success:true,data:{reportQueue:q,connected:true}});await Promise.resolve();await Promise.resolve();const before=element.textContent;pending[0].reject(new Error('stale'));await Promise.resolve();await Promise.resolve();assert(element.textContent===before && !before.startsWith('未确认')); }
 else throw new Error(mode);
})().catch(err=>{console.error(err);process.exitCode=1;});
'''


class MqttReportQueueWebTests(unittest.TestCase):
    def check(self, case):
        result = subprocess.run(['node', '-e', PROBE,
            str(ROOT/'web-src/modules/runtime/protocol/mqtt-config.js'), case],
            capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


for _case in ('retained', 'counters', 'configured_topics', 'offline', 'active', 'empty', 'unknown', 'contradiction',
              'dom', 'request_failure', 'handled_failure', 'stale', 'history_empty_current',
              'history_unavailable', 'history_invalid', 'history_saturation', 'history_uncertain'):
    setattr(MqttReportQueueWebTests, 'test_'+_case, lambda self, case=_case:self.check(case))

if __name__ == '__main__':
    unittest.main()
