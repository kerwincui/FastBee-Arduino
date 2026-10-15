"""Execute the production SD diagnostics UI with deterministic API boundaries."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert=require('assert/strict'),fs=require('fs'),vm=require('vm');
const text={},content={value:'keep draft'},path={value:'/fastbee/test.txt'},calls=[],notifications=[];
let get=async()=>({success:true,data:sample()}),post=async()=>({success:true}),confirmed=true;
const app={registerModule(name,methods){Object.assign(this,methods);}};
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/peripheral-io.js','utf8'),{
  AppState:app,document:{getElementById:id=>id==='peripheral-sd-content'?content:id==='peripheral-sd-path'?path:null},
  apiGet:async(...args)=>{calls.push(['get',...args]);return get(...args);},
  apiPost:async(...args)=>{calls.push(['post',...args]);return post(...args);},
  Notification:{success:(...args)=>notifications.push(args)},
  // B4 起共享确认弹窗为异步 FBConfirm（Promise<boolean|string|null>）；与同步
  // confirm 桩共用同一 confirmed 变量，取消语义保持一致。
  FBConfirm:async(opts)=>opts&&opts.input?(opts.input.value??''):confirmed,
  confirm:()=>confirmed,console,TextEncoder
});
app._setPeripheralIoText=(id,value)=>text[id]=value;
app._setPeripheralIoOutput=(id,value)=>text[id]=value;
app._setPeripheralIoError=value=>text.error=value;
app._peripheralIoErrorMessage=(error,fallback)=>error.message||fallback;
app._setPeripheralIoBusy=value=>app._periphIoBusy=value;
app.guardDeveloperModeAction=()=>true;app.isDeveloperModeEnabled=()=>true;
app._periphIoSession={};app._periphIoId='sd-a';app._periphIoType=37;app._periphIoKind='sd';
const sample=()=>({statusAvailable:true,mounted:true,capacityQuality:'last_operation_snapshot',capacityAgeMs:42,cardType:'SDHC',totalBytes:4096,usedBytes:128});
const unknown=()=>{assert.equal(text['peripheral-sd-total'],'--');assert.equal(text['peripheral-sd-used'],'--');assert.equal(text['peripheral-sd-free'],'--');};
const cases={
  long_age_keeps_exact_monotonic_age(){const age=3*2**32+37;app._renderPeripheralSdData({...sample(),capacityAgeMs:age});assert.equal(text['peripheral-sd-filesystem'],'上次操作 · '+age+' ms 前');assert.notEqual(text['peripheral-sd-total'],'--');assert.equal(calls.length,0);},
  unsafe_age_is_unknown(){for(const age of [Number.MAX_SAFE_INTEGER+1,Infinity,0.5,'4294967296']){app._renderPeripheralSdData({...sample(),capacityAgeMs:age});unknown();}},
  snapshot_has_age_not_live_claim(){app._renderPeripheralSdData(sample());assert.equal(text['peripheral-sd-mounted'],'已挂载');assert.notEqual(text['peripheral-sd-total'],'--');assert.equal(text['peripheral-sd-filesystem'],'上次操作 · 42 ms 前');assert.equal(calls.length,0);},
  busy_is_unknown_not_unmounted(){app._renderPeripheralSdData(sample());app._renderPeripheralSdData({statusAvailable:false});assert.equal(text['peripheral-sd-mounted'],'状态未确认');unknown();},
  legacy_metadata_is_not_promoted(){app._renderPeripheralSdData({mounted:true,totalBytes:4096,usedBytes:0});assert.equal(text['peripheral-sd-mounted'],'状态未确认');unknown();},
  invalid_capacity_never_becomes_zero(){for(const override of [{usedBytes:null},{totalBytes:'4096'},{usedBytes:4097},{usedBytes:-1},{capacityAgeMs:null},{capacityAgeMs:-1},{mounted:false},{statusAvailable:false},{capacityQuality:'fresh'},{totalBytes:Infinity}]){app._renderPeripheralSdData({...sample(),...override});unknown();}},
  null_mount_is_unknown(){app._renderPeripheralSdData({...sample(),mounted:null});assert.equal(text['peripheral-sd-mounted'],'状态未确认');unknown();},
  pending_refresh_discards_prior_snapshot:async()=>{app._renderPeripheralSdData(sample());let resolve;get=()=>new Promise(r=>resolve=r);const work=app.refreshPeripheralIo(false);unknown();resolve({success:true,data:sample()});await work;assert.equal(text['peripheral-sd-mounted'],'已挂载');},
  refresh_failure_keeps_draft_not_sample:async()=>{app._renderPeripheralSdData(sample());get=async()=>{throw new Error('offline');};await app.refreshPeripheralIo(false);unknown();assert.equal(content.value,'keep draft');assert.equal(text.error,'offline');},
  late_read_never_overwrites_new_draft:async()=>{let resolve;get=()=>new Promise(r=>resolve=r);const work=app.runPeripheralSdOperation('read-file');app._periphIoSession={};content.value='new draft';app._periphIoBusy=true;resolve({success:true,data:{content:'old file'}});await work;assert.equal(content.value,'new draft');assert.equal(app._periphIoBusy,true);assert.equal(calls.length,1);},
  late_mutation_does_not_refresh_new_device:async()=>{let resolve;post=()=>new Promise(r=>resolve=r);const work=app.runPeripheralSdOperation('mount');app._periphIoSession={};app._periphIoId='sd-b';app._periphIoBusy=true;text.error='new';resolve({success:true});await work;assert.equal(calls.length,1);assert.equal(calls[0][2].id,'sd-a');assert.equal(app._periphIoBusy,true);assert.equal(text.error,'new');assert.equal(notifications.length,0);},
  late_failure_does_not_touch_new_session:async()=>{let reject;post=()=>new Promise((r,j)=>reject=j);const work=app.runPeripheralSdOperation('write-file');app._periphIoSession={};text.error='new';app._periphIoBusy=true;reject(new Error('old'));await work;assert.equal(text.error,'new');assert.equal(app._periphIoBusy,true);},
  missing_success_is_unconfirmed:async()=>{post=async()=>({});await app.runPeripheralSdOperation('write-file');assert(text.error.includes('结果未确认'));assert.equal(calls.length,1);assert.equal(notifications.length,0);assert.equal(content.value,'keep draft');unknown();},
  partial_failure_invalidates_capacity:async()=>{app._renderPeripheralSdData(sample());post=async()=>({success:false,error:'SD_WRITE_TRUNCATED'});await app.runPeripheralSdOperation('write-file');unknown();assert.equal(text.error,'SD_WRITE_TRUNCATED');assert.equal(content.value,'keep draft');assert.equal(calls.length,1);},
  successful_write_refreshes_once:async()=>{await app.runPeripheralSdOperation('write-file');assert.deepEqual(calls.map(c=>c[0]),['post','get']);assert.equal(calls[0][2].content,'keep draft');assert.equal(calls[1][2].id,'sd-a');assert.equal(notifications.length,1);assert.equal(app._periphIoBusy,false);},
  successful_read_keeps_capacity_metadata:async()=>{app._renderPeripheralSdData(sample());const total=text['peripheral-sd-total'];get=async()=>({success:true,data:{content:'read text',length:9}});await app.runPeripheralSdOperation('read-file');assert.equal(content.value,'read text');assert.equal(text['peripheral-sd-total'],total);assert.equal(calls.length,1);},
  malformed_read_keeps_draft:async()=>{for(const data of [{},{content:'abc'},{content:'abc',length:2},{content:3,length:1},{content:'abc',length:-1},{content:'x'.repeat(4097),length:4097}]){get=async()=>({success:true,data});await app.runPeripheralSdOperation('read-file');assert.equal(content.value,'keep draft');assert(text.error.includes('SD_READ_UNCONFIRMED'));}},
  utf8_read_counts_bytes:async()=>{get=async()=>({success:true,data:{content:'中文',length:6}});await app.runPeripheralSdOperation('read-file');assert.equal(content.value,'中文');assert.equal(text.error,'');},
  empty_read_is_complete:async()=>{get=async()=>({success:true,data:{content:'',length:0}});await app.runPeripheralSdOperation('read-file');assert.equal(content.value,'');assert.equal(text.error,'');},
  directory_exact_boundary_is_not_truncated(){const entries=Array.from({length:32},(_,i)=>({path:'/fastbee/file_'+i,directory:false,size:i}));app._renderPeripheralSdData({entries,count:32,truncated:false});assert.equal(text['peripheral-sd-output'].split('\n').length,32);assert(!text['peripheral-sd-output'].includes('更多'));app._renderPeripheralSdData({entries,count:32,truncated:true});assert(text['peripheral-sd-output'].includes('还有更多条目'));},
  directory_empty_is_explicit(){app._renderPeripheralSdData({entries:[],count:0,truncated:false});assert.equal(text['peripheral-sd-output'],'[目录为空]');assert.equal(content.value,'keep draft');},
  directory_malformed_keeps_prior_list:async()=>{const row={path:'/fastbee/a',directory:false,size:3};for(const data of [{},{entries:[]},{entries:[],count:0,truncated:true},{entries:[row],count:2,truncated:false},{entries:[row],count:1,truncated:'false'},{entries:[{...row,size:-1}],count:1,truncated:false},{entries:[{...row,path:'/other/a'}],count:1,truncated:false},{entries:[{...row,path:'/fastbee/../a'}],count:1,truncated:false},{entries:Array(33).fill(row),count:33,truncated:true}]){text['peripheral-sd-output']='prior list';get=async()=>({success:true,data});await app.runPeripheralSdOperation('list');assert(text.error.includes('SD_LIST_UNCONFIRMED'));assert.equal(text['peripheral-sd-output'],'prior list');assert.equal(content.value,'keep draft');}},
  directory_request_does_not_refresh_capacity:async()=>{app._renderPeripheralSdData(sample());const label=text['peripheral-sd-filesystem'];get=async()=>({success:true,data:{entries:[],count:0,truncated:false}});await app.runPeripheralSdOperation('list');assert.equal(text['peripheral-sd-filesystem'],label);assert.equal(calls.length,1);},
  late_directory_keeps_new_session:async()=>{let resolve;get=()=>new Promise(r=>resolve=r);const pending=app.runPeripheralSdOperation('list');app._periphIoSession={};text['peripheral-sd-output']='new session';app._periphIoBusy=true;resolve({success:true,data:{entries:[],count:0,truncated:false}});await pending;assert.equal(text['peripheral-sd-output'],'new session');assert.equal(app._periphIoBusy,true);assert.equal(calls.length,1);},
  recovery_diagnostics(){app._renderPeripheralSdData({...sample(),recoverySuggested:true,ioFailures:3,recoveryAttempts:2,recoveries:1,lastFailureCode:'SD_WRITE_TRUNCATED'});for(const value of ['I/O故障 3','手动恢复 1/2','SD_WRITE_TRUNCATED','不因恢复清零'])assert(text['peripheral-sd-recovery'].includes(value));},
  recovery_unknown(){for(const details of [{},{recoverySuggested:true,ioFailures:3,recoveryAttempts:0,recoveries:1,lastFailureCode:''}]){app._renderPeripheralSdData({...sample(),...details});assert(text['peripheral-sd-recovery'].includes('未确认'));}},
  lost_bus_keeps_diagnostics_not_capacity(){app._renderPeripheralSdData({statusAvailable:false,configurationAvailable:true,configured:true,recoverySuggested:true,ioFailures:1,recoveryAttempts:0,recoveries:0,lastFailureCode:'SD_SPI_LEASE_LOST'});assert.equal(text['peripheral-sd-mounted'],'状态未确认');unknown();assert(text['peripheral-sd-recovery'].includes('SD_SPI_LEASE_LOST'));assert.equal(calls.length,0);},
  recovery_is_one_request_without_draft:async()=>{await app.runPeripheralSdOperation('recover');assert.deepEqual(calls.map(c=>c[0]),['post','get']);assert.equal(calls[0][2].action,'recover');assert.equal(calls[0][2].id,'sd-a');assert(!('content' in calls[0][2])&&!('path' in calls[0][2]));assert.equal(content.value,'keep draft');},
  recovery_failure_is_not_retried:async()=>{post=async()=>({success:false,error:'SD_MOUNT_FAILED'});await app.runPeripheralSdOperation('recover');assert.equal(calls.length,1);assert.equal(text.error,'SD_MOUNT_FAILED');assert.equal(content.value,'keep draft');assert.equal(notifications.length,0);},
  recovery_cancel_and_permission:async()=>{confirmed=false;await app.runPeripheralSdOperation('recover');confirmed=true;app.guardDeveloperModeAction=()=>false;await app.runPeripheralSdOperation('recover');assert.equal(calls.length,0);},
  unknown_action_does_not_post:async()=>{await app.runPeripheralSdOperation('format');assert.equal(calls.length,0);},
  late_recovery_does_not_refresh_new_device:async()=>{let done;post=()=>new Promise(r=>done=r);const pending=app.runPeripheralSdOperation('recover');while(!done)await Promise.resolve();app._periphIoSession={};app._periphIoId='sd-new';done({success:true});await pending;assert.equal(calls.length,1);assert.equal(notifications.length,0);},
  cancellation_and_guards_make_no_calls:async()=>{confirmed=false;await app.runPeripheralSdOperation('delete-file');app.guardDeveloperModeAction=()=>false;await app.runPeripheralSdOperation('mount');app._periphIoBlocked='blocked';await app.runPeripheralSdOperation('read-file');assert.equal(calls.length,0);},
  new_modal_clears_old_summary(){const source=fs.readFileSync('web-src/modules/runtime/peripheral-io.js','utf8');const open=source.slice(source.indexOf('async openPeripheralIoModal('),source.indexOf('closePeripheralIoModal() {'));assert(open.includes('this._renderPeripheralSdData({statusAvailable: false})'));}
};
Promise.resolve(cases[process.argv[1]]()).catch(error=>{console.error(error);process.exitCode=1;});
'''


class SdQualityWebTests(unittest.TestCase):
    def run_case(self, name):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run([node, '-e', SCRIPT, name], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _name in ('long_age_keeps_exact_monotonic_age', 'unsafe_age_is_unknown', 'snapshot_has_age_not_live_claim', 'busy_is_unknown_not_unmounted', 'legacy_metadata_is_not_promoted',
              'invalid_capacity_never_becomes_zero', 'null_mount_is_unknown', 'pending_refresh_discards_prior_snapshot',
              'refresh_failure_keeps_draft_not_sample', 'late_read_never_overwrites_new_draft',
              'late_mutation_does_not_refresh_new_device', 'late_failure_does_not_touch_new_session',
              'missing_success_is_unconfirmed', 'partial_failure_invalidates_capacity',
              'successful_write_refreshes_once', 'successful_read_keeps_capacity_metadata',
              'malformed_read_keeps_draft', 'utf8_read_counts_bytes', 'empty_read_is_complete',
              'directory_exact_boundary_is_not_truncated', 'directory_empty_is_explicit',
              'directory_malformed_keeps_prior_list', 'directory_request_does_not_refresh_capacity', 'late_directory_keeps_new_session',
              'recovery_diagnostics', 'recovery_unknown', 'lost_bus_keeps_diagnostics_not_capacity', 'recovery_is_one_request_without_draft',
              'recovery_failure_is_not_retried', 'recovery_cancel_and_permission',
              'unknown_action_does_not_post', 'late_recovery_does_not_refresh_new_device',
              'cancellation_and_guards_make_no_calls', 'new_modal_clears_old_summary'):
    setattr(SdQualityWebTests, 'test_' + _name, lambda self, name=_name: self.run_case(name))

if __name__ == '__main__':
    unittest.main()
