"""RTC diagnostic UI executes production JS against deterministic API boundaries."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert=require('assert/strict'),fs=require('fs'),vm=require('vm');
const text={},input={value:'2026-10-01T10:20:30'},calls=[];
const app={registerModule(name,methods){Object.assign(this,methods);}};
let get=async()=>({success:true,data:sample()}),post=async()=>({success:true}),confirmed=true;
const context={AppState:app,document:{getElementById:id=>id==='peripheral-ds1302-datetime'?input:null},
  apiGet:async(...args)=>{calls.push(['get',...args]);return get(...args);},
  apiPost:async(...args)=>{calls.push(['post',...args]);return post(...args);},
  // B4 起共享确认弹窗为异步 FBConfirm（Promise<boolean|string|null>）；与同步
  // confirm 桩共用同一 confirmed 变量，取消语义保持一致。
  FBConfirm:async(opts)=>opts&&opts.input?(opts.input.value??''):confirmed,
  confirm:()=>confirmed,console,Date,TextEncoder};
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/peripheral-io.js','utf8'),context);
app._setPeripheralIoText=(id,value)=>text[id]=value;
app._setPeripheralIoOutput=(id,value)=>text[id]=value;
app._setPeripheralIoError=value=>text.error=value;
app._peripheralIoErrorMessage=(error,fallback)=>error.message||fallback;
app._setPeripheralIoBusy=value=>app._periphIoBusy=value;
app.guardDeveloperModeAction=()=>true;
app._periphIoSession={};app._periphIoId='rtc-a';app._periphIoType=50;app._periphIoKind='ds1302';
const sample=()=>({initialized:true,running:true,quality:'calendar_unverified',timeBasis:'device_calendar_unzoned',formatted:'2026-10-01 10:20:30'});
const output=()=>text['peripheral-ds1302-output'];
const cases={
  running_is_unverified(){app._renderPeripheralDs1302Data(sample());assert(text['peripheral-ds1302-running'].includes('时间源未核验'));assert.equal(output().timeBasis,'device_calendar_unzoned');assert.equal(output().calendar,'2026-10-01 10:20:30');assert.equal(calls.length,0);},
  halted_is_not_current(){app._renderPeripheralDs1302Data({...sample(),running:false,quality:'halted'});assert(text['peripheral-ds1302-running'].includes('非运行时钟'));},
  legacy_and_inconsistent_quality_unknown(){for(const value of [{formatted:'old',running:true},{...sample(),quality:'trusted'},{...sample(),timeBasis:'UTC'},{...sample(),running:false}]){app._renderPeripheralDs1302Data(value);assert.equal(text['peripheral-ds1302-running'],'质量未知');}},
  refresh_failure_discards_old_calendar:async()=>{app._renderPeripheralDs1302Data(sample());get=async()=>{throw new Error('offline');};await app.refreshPeripheralIo(true);assert.equal(text['peripheral-ds1302-current'],'--');assert(output().includes('旧日历不可'));assert.equal(text.error,'offline');assert.equal(app._periphIoBusy,false);assert.equal(calls.length,1);},
  pending_read_does_not_display_old_sample:async()=>{app._renderPeripheralDs1302Data(sample());let resolve;get=()=>new Promise(r=>resolve=r);const work=app.refreshPeripheralIo(true);assert.equal(text['peripheral-ds1302-current'],'--');assert.equal(output(),'读取中');resolve({success:true,data:sample()});await work;assert.equal(text['peripheral-ds1302-current'],sample().formatted);},
  late_read_does_not_update_new_session:async()=>{let resolve;get=()=>new Promise(r=>resolve=r);const work=app.refreshPeripheralIo(true);app._periphIoSession={};text['peripheral-ds1302-current']='new-session';app._periphIoBusy=true;resolve({success:true,data:sample()});await work;assert.equal(text['peripheral-ds1302-current'],'new-session');assert.equal(app._periphIoBusy,true);},
  late_write_success_never_reads_new_device:async()=>{let resolve;post=()=>new Promise(r=>resolve=r);const work=app.runPeripheralDs1302Operation('start');app._periphIoSession={};app._periphIoId='rtc-b';text['peripheral-ds1302-output']='new-session';app._periphIoBusy=true;resolve({success:true});await work;assert.equal(calls.length,1);assert.equal(calls[0][2].id,'rtc-a');assert.equal(output(),'new-session');assert.equal(app._periphIoBusy,true);},
  late_write_failure_does_not_poison_new_session:async()=>{let reject;post=()=>new Promise((r,j)=>reject=j);const work=app.runPeripheralDs1302Operation('start');app._periphIoSession={};text.error='new-error';text['peripheral-ds1302-output']='new-session';reject(new Error('old-failure'));await work;assert.equal(text.error,'new-error');assert.equal(output(),'new-session');assert.equal(calls.length,1);},
  missing_success_is_unconfirmed:async()=>{post=async()=>({message:'ambiguous'});await app.runPeripheralDs1302Operation('start');assert.equal(calls.length,1);assert(output().includes('写入结果未确认'));assert.equal(text['peripheral-ds1302-current'],'--');assert.equal(app._periphIoBusy,false);},
  same_session_success_reads_once:async()=>{await app.runPeripheralDs1302Operation('start');assert.deepEqual(calls.map(c=>c[0]),['post','get']);assert.equal(calls[1][2].id,'rtc-a');assert.equal(output().quality,'运行中；时间源未核验');assert.equal(app._periphIoBusy,false);},
  input_draft_is_not_overwritten:async()=>{await app.refreshPeripheralIo(true);assert.equal(input.value,'2026-10-01T10:20:30');},
  cancelled_stop_does_not_clear_or_write:async()=>{confirmed=false;app._renderPeripheralDs1302Data(sample());await app.runPeripheralDs1302Operation('stop');assert.equal(calls.length,0);assert.equal(text['peripheral-ds1302-current'],sample().formatted);},
  blocked_and_unauthorized_do_not_write:async()=>{app._periphIoBlocked='blocked';await app.runPeripheralDs1302Operation('start');app._periphIoBlocked='';app.guardDeveloperModeAction=()=>false;await app.runPeripheralDs1302Operation('start');assert.equal(calls.length,0);},
  old_success_read_without_quality_stays_unknown:async()=>{get=async()=>({success:true,data:{formatted:'old-calendar'}});await app.refreshPeripheralIo(true);assert.equal(text['peripheral-ds1302-running'],'质量未知');assert.equal(output().timeBasis,'unknown');},
  static_snapshot_label_and_open_reset(){const html=fs.readFileSync('web-src/pages/fragments/peripheral-io.html','utf8');assert(html.includes('上次读取（日历，无时区）'));const js=fs.readFileSync('web-src/modules/runtime/peripheral-io.js','utf8');assert(js.includes("this._clearPeripheralDs1302Reading('尚未读取')"));}
};
Promise.resolve(cases[process.argv[1]]()).catch(error=>{console.error(error);process.exitCode=1;});
'''


class RtcQualityWebTests(unittest.TestCase):
    def run_case(self, name):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run([node, '-e', SCRIPT, name], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _name in (
    'running_is_unverified', 'halted_is_not_current', 'legacy_and_inconsistent_quality_unknown',
    'refresh_failure_discards_old_calendar', 'pending_read_does_not_display_old_sample',
    'late_read_does_not_update_new_session', 'late_write_success_never_reads_new_device',
    'late_write_failure_does_not_poison_new_session', 'missing_success_is_unconfirmed',
    'same_session_success_reads_once', 'input_draft_is_not_overwritten',
    'cancelled_stop_does_not_clear_or_write', 'blocked_and_unauthorized_do_not_write',
    'old_success_read_without_quality_stays_unknown', 'static_snapshot_label_and_open_reset',
):
    setattr(RtcQualityWebTests, 'test_' + _name, lambda self, name=_name: self.run_case(name))


if __name__ == '__main__':
    unittest.main()
