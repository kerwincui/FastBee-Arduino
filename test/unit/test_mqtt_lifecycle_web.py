"""Prepared lifecycle feedback tests against the real Web module; never contacts a device."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE = r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const mode=process.argv[2], requests=[];
const nodes={};
for(const id of ['badge','server','clientid','reconnects','reason']) {
 const e={textContent:'',title:'',className:'',removeAttribute:()=>{}};
 e.classList={remove:()=>{},contains:name=>e.className.split(/\s+/).includes(name)};
 nodes['mqtt-status-'+id]=e;
}
const box={AppState:{},window:{isPageAbortError:()=>false},console,
 document:{getElementById:id=>nodes[id]||null,querySelector:()=>null},
 setTimeout:()=>1,clearTimeout:()=>{},clearInterval:()=>{},
 apiPostSilent:()=>new Promise((resolve,reject)=>requests.push({resolve,reject}))};
vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),box);
const app=box.AppState,badge=nodes['mqtt-status-badge'],reason=nodes['mqtt-status-reason'];
app._mqttStatusRequestSeq=1;app._mqttLitePollStopped=false;
const base={enabled:true,initialized:true,connected:false,autoReconnect:true,networkConnected:true,
 lastErrorCode:0,lastErrorClass:'none',reconnectCount:0};
const unknown=()=>{assert.strictEqual(badge.textContent,'状态未确认');assert(!badge.className.includes('online'));};
const flush=async()=>{await Promise.resolve();await Promise.resolve();};
(async()=>{
 if(mode==='worker_pending') {
  app._updateMqttStatusUI({...base,reconnectStopPending:true,reconnectCancellationRequested:true,connected:true});
  unknown();assert.strictEqual(requests.length,0);assert(reason.textContent.includes('等待后台任务退出'));
 } else if(mode==='worker_cancelled') {
  app._updateMqttStatusUI({...base,reconnectCancellationRequested:true,stopped:true});
  assert.strictEqual(requests.length,0);assert(!badge.className.includes('online'));
  assert(reason.textContent.includes('手动重新连接'));
 } else if(mode==='busy') {
  app._updateMqttStatusUI({...base,lifecycleBusy:true,statusReason:'切换中'});
  unknown();assert.strictEqual(requests.length,0);assert.strictEqual(reason.textContent,'切换中');
 } else if(mode==='unavailable_connected') {
  app._updateMqttStatusUI({...base,stateAvailable:false,connected:true,testConnected:true});
  unknown();assert.strictEqual(requests.length,0);assert.strictEqual(nodes['mqtt-status-reconnects'].textContent,'--');
 } else if(mode==='pending_timeout') {
  app._mqttConnectingStartTime=1;
  app._updateMqttStatusUI({...base,lifecyclePending:true,connecting:true});
  unknown();assert.strictEqual(app._mqttConnectingStartTime,0);assert.strictEqual(requests.length,0);
 } else if(mode==='memory') {
  app._updateMqttStatusUI({...base,lifecycleBusy:true,stoppedByMemory:true,lastErrorCode:-14});
  assert.strictEqual(badge.textContent,'内存保护');assert.strictEqual(requests.length,0);
 } else if(mode==='test_pending'||mode==='test_restore_blocked'||mode==='test_probe') {
  app._mqttTestConnectedDetected=true;
  app._updateMqttStatusUI({...base,connected:true,testConnected:true,
   testRestorePending:mode==='test_pending',testRestoreBlocked:mode==='test_restore_blocked',
   connectionTestOnly:mode==='test_probe'});
  assert.strictEqual(requests.length,0);assert(!badge.className.includes('online'));
  assert.strictEqual(app._mqttTestConnectedDetected,false);
  assert.strictEqual(badge.textContent,mode==='test_restore_blocked'?'配置恢复待处理':'测试已连接，待恢复');
 } else {
  if(mode==='pending_reply') {
   app._mqttLitePollStopped=true;app._startMqttStatusPolling();
   assert.strictEqual(app._mqttLitePollStopped,false);
  }
  app._updateMqttStatusUI(base);assert.strictEqual(requests.length,1);
  if(mode==='pending_reply'||mode==='busy_reply') {
   requests[0].resolve({success:mode==='pending_reply',data:{reconnecting:false,
    lifecyclePending:mode==='pending_reply',lifecycleBusy:mode==='busy_reply',statusReason:'等待在途操作'}});
   await flush();unknown();assert.strictEqual(reason.textContent,'等待在途操作');
  } else if(mode==='rejected_reply') {
   requests[0].resolve({success:false,data:{statusReason:'启动失败'}});
   await flush();assert.strictEqual(badge.textContent,'连接失败');assert.strictEqual(reason.textContent,'启动失败');
  } else if(mode==='stale_reply'||mode==='stale_failure'||mode==='page_left') {
   if(mode==='page_left')app._stopMqttStatusPolling();
   else app._mqttStatusRequestSeq++;
   app._mqttAutoReconnectTriggered=true;
   app._updateMqttStatusUI({...base,connected:true});
   const before=badge.textContent;
   if(mode==='stale_failure')requests[0].reject(new Error('old request'));
   else requests[0].resolve({success:false,data:{lifecycleBusy:true,statusReason:'old request'}});
   await flush();assert.strictEqual(badge.textContent,before);assert.strictEqual(before,'已连接');
  } else throw new Error(mode);
 }
})().catch(err=>{console.error(err);process.exitCode=1;});
'''


class MqttLifecycleWebTests(unittest.TestCase):
    def check(self, case):
        result = subprocess.run(['node', '-e', PROBE,
            str(ROOT/'web-src/modules/runtime/protocol/mqtt-config.js'), case],
            capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


for _case in ('test_pending', 'test_restore_blocked', 'test_probe',
              'busy', 'unavailable_connected', 'pending_timeout', 'memory', 'pending_reply',
              'busy_reply', 'rejected_reply', 'stale_reply', 'stale_failure', 'page_left',
              'worker_pending', 'worker_cancelled'):
    setattr(MqttLifecycleWebTests, 'test_'+_case, lambda self, case=_case:self.check(case))

if __name__ == '__main__':
    unittest.main()
