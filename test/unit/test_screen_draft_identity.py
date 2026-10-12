"""Execute the production draft methods; DHCP reuse must not cross devices."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ScreenDraftIdentityTests(unittest.TestCase):
    def run_script(self, program):
        loader = "vm.runInContext(fs.readFileSync('web-src/modules/runtime/'+name+'.js','utf8'),context)"
        for variant in ('source', 'terser'):
            with self.subTest(variant=variant):
                script = program if variant == 'source' else program.replace(loader,
                    "vm.runInContext(require('terser').minify_sync(fs.readFileSync('web-src/modules/runtime/'+name+'.js','utf8'),{compress:{passes:3},mangle:true}).code,context)")
                result = subprocess.run(['node', '-e', script], cwd=ROOT, capture_output=True,
                                        text=True, encoding='utf-8', timeout=20)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_pagehide_flushes_latest_edit_and_history_without_crossing_device_identity(self):
        program = r'''
const assert=require('node:assert/strict'), fs=require('node:fs'), vm=require('node:vm');
const store=new Map(),timers=new Map(),events=new Map();let next=0;
const context=vm.createContext({AppState:{},location:{host:'offline'},console,
 setTimeout(fn){timers.set(++next,fn);return next;},clearTimeout(id){timers.delete(id);},
 addEventListener(name,fn){events.set(name,fn);},
 localStorage:{getItem:k=>store.get(k)||null,setItem:(k,v)=>store.set(k,v),removeItem:k=>store.delete(k)}});
context.window=context;
for(const name of ['screen-panel-model','screen-panel'])vm.runInContext(fs.readFileSync('web-src/modules/runtime/'+name+'.js','utf8'),context);
const app=context.AppState,m=app._spEnsureModel();
m.deviceMac='A';m.config=app._screenPanelModelHelpers.defaultConfig();m.savedConfig=structuredClone(m.config);
m.revision=3;m.dirty=true;
m.config.pages[0].title='earlier';app._spPersistDraftSoon();
for(const fn of timers.values())fn();timers.clear();
m.config.pages[0].title='latest';app._spPersistDraftSoon();
assert.equal(timers.size,1);assert.ok(events.has('pagehide'),'browser departure must flush pending edits');
events.get('pagehide')();
const key=[...store.keys()][0],draft=JSON.parse(store.get(key));
assert.equal(draft.config.pages[0].title,'latest');assert.equal(draft.baseRevision,3);
assert.equal(draft.baseConfig.pages[0].title,m.savedConfig.pages[0].title);assert.equal(timers.size,0);
m.dirty=false;m.historyDirty=true;m.historyDraft={enabled:true};app._spPersistDraftSoon();events.get('pagehide')();
assert.equal(JSON.parse(store.get(key)).historyDraft.enabled,true);
const saved=store.get(key);m.deviceMac='';events.get('pagehide')();assert.equal(store.get(key),saved);
m.deviceMac='A';app._spPersistDraftSoon();m.deviceMac='B';events.get('pagehide')();
assert.equal(store.size,1,'a stale A timer must not create a B draft');assert.equal(store.get(key),saved);
context.localStorage.setItem=()=>{throw Error('storage disabled');};
m.deviceMac='A';app._spPersistDraftSoon();events.get('pagehide')();
assert.equal(timers.size,0,'storage failure cannot leave a dangling timer');
console.log('departure draft scenarios passed');
'''
        self.run_script(program)

    def test_mac_revision_isolation_and_missing_identity_preserve_storage(self):
        program = r'''
const assert=require('node:assert/strict'), fs=require('node:fs'), vm=require('node:vm');
const store=new Map();let next=0;const timers=new Map();
const context=vm.createContext({AppState:{},location:{host:'192.168.5.26'},console,
 setTimeout(fn){timers.set(++next,fn);return next;},clearTimeout(id){timers.delete(id);},
 localStorage:{getItem:k=>store.get(k)||null,setItem:(k,v)=>store.set(k,v),removeItem:k=>store.delete(k)}});
context.window=context;
for(const name of ['screen-panel-model','screen-panel'])vm.runInContext(fs.readFileSync('web-src/modules/runtime/'+name+'.js','utf8'),context);
const app=context.AppState,m=app._spEnsureModel(), config=app._screenPanelModelHelpers.defaultConfig();
m.config=config;m.revision=7;m.dirty=true;m.deviceMac='E8:3D:C1:F7:65:44';
function flush(){for(const fn of timers.values())fn();timers.clear();}
app._spPersistDraftSoon();flush();assert.equal(store.size,1);
const key=[...store.keys()][0];assert.ok(key.endsWith(m.deviceMac));
assert.ok(app._spRestoreDraft(config,7));assert.equal(app._spRestoreDraft(config,8).baseRevision,8);
const stored=store.get(key),legacy=JSON.parse(stored);delete legacy.baseConfig;store.set(key,JSON.stringify(legacy));
assert.equal(app._spRestoreDraft(config,8),null,'legacy drafts without baseline cannot merge across revisions');store.set(key,stored);
m.deviceMac='14:C1:9F:C1:52:D8';assert.equal(app._spRestoreDraft(config,7),null);
app._spClearDraft();assert.equal(store.size,1,'clearing B must retain A');
m.deviceMac='';assert.equal(app._spRestoreDraft(config,7),null);
app._spPersistDraftSoon();flush();app._spClearDraft();assert.equal(store.size,1,'unknown identity never mutates storage');
m.deviceMac='E8:3D:C1:F7:65:44';app._spPersistDraftSoon();m.deviceMac='14:C1:9F:C1:52:D8';flush();
assert.equal(store.size,1,'late timer cannot write A draft as B');
store.set(key.replace('E8:3D:C1:F7:65:44',m.deviceMac),store.get(key));
assert.equal(app._spRestoreDraft(config,7),null,'payload MAC must match key and live identity');
console.log('draft identity scenarios passed');
'''
        self.run_script(program)


if __name__ == '__main__':
    unittest.main()
