"""Exercise every production component's wire roundtrip and draft recovery."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE = r'''
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const {minifyJS}=require('./scripts/minify-js');
for(const minified of [false,true]) {
 const storage=new Map(),timers=new Map();let timer=0;
 const box={AppState:{},structuredClone,console,location:{host:'device.test'},
  setTimeout:fn=>{timers.set(++timer,fn);return timer;},clearTimeout:id=>timers.delete(id),
  localStorage:{getItem:k=>storage.get(k)||null,setItem:(k,v)=>storage.set(k,v),removeItem:k=>storage.delete(k)}};
 box.window=box;vm.createContext(box);
 for(const n of ['screen-panel-model','screen-panel']) {
  const s=fs.readFileSync('web-src/modules/runtime/'+n+'.js','utf8');vm.runInContext(minified?minifyJS(s):s,box);
 }
 const app=box.AppState,h=app._screenPanelModelHelpers,m=app._spEnsureModel();
 const plain=x=>JSON.parse(JSON.stringify(x));
 m.choices=[{kind:'source',type:'number',ref:'sensor.a',unit:'C'},
  {kind:'source',type:'number',ref:'sensor.b',unit:'C'},
  ...['switch','number','action','motor'].map(type=>({kind:'control',type,ref:'rule.'+type,available:true,allowPanelControl:true}))];
 if(process.argv[1]==='roundtrip') {
  for(const kind of Object.keys(h.CARD_DEFAULTS)) for(const display of kind==='control'?['number','switch','action']:['']) {
   const c=h.defaultConfig();c.settings.allowSystemRestart=true;
   const card=Object.assign({id:'component',kind,precision:1,rect:{x:8,y:8,w:440,h:340}},h.CARD_DEFAULTS[kind]);
   if(display) {card.display=display;card.controlRef='rule.'+display;card.confirm=false;}
   if(kind==='motor')card.controlRef='rule.motor';
   if(kind==='page-link')card.targetPageId='home';
   if(h.SOURCE_KINDS.includes(kind) && kind!=='weather')card.sourceRef='sensor.a';
   if(kind==='bar')card.sourceRefs=['sensor.a','sensor.b'];
   if(['bar','gauge'].includes(kind)||display==='number') {card.min=-12.5;card.max=82.5;card.step=0.5;card.initialValue=1.5;}
   c.pages[0].layoutMode='canvas';c.pages[0].cards=[card];
   const wire=h.sanitizeConfig(c,m.choices),loaded=h.normalizeConfig(wire);
   assert.deepEqual(plain(h.sanitizeConfig(loaded,m.choices)),plain(wire),kind+':'+display+' must roundtrip');
   if(display)assert.equal(loaded.pages[0].cards[0].confirm,false);
   m.config=loaded;assert.deepEqual(plain(app._spValidateConfig(loaded).errors),[],kind+':'+display);
  }
  const keepAwake=h.defaultConfig();
  keepAwake.settings.idleSleepSeconds=0;
  keepAwake.pages[0].layoutMode='canvas';
  keepAwake.pages[0].cards=[{id:'keep-awake',kind:'sleep-time',display:'switch',title:'屏幕常亮',precision:1,rect:{x:8,y:8,w:220,h:144}}];
  const keepWire=h.sanitizeConfig(keepAwake,m.choices);
  assert.equal(keepWire.pages[0].cards[0].visualType,'switch');
  const keepLoaded=h.normalizeConfig(keepWire);
  assert.equal(keepLoaded.pages[0].cards[0].display,'switch');
  assert.deepEqual(plain(h.sanitizeConfig(keepLoaded,m.choices)),plain(keepWire));
  assert.deepEqual(plain(app._spValidateConfig(keepLoaded).errors),[]);
  const keepPreview=app._spSystemPreview(keepLoaded.pages[0].cards[0],keepLoaded.settings);
  assert.ok(keepPreview.includes('role="switch"'));
  assert.ok(keepPreview.includes('aria-checked="true"'));
  keepLoaded.settings.idleSleepSeconds=30;
  assert.ok(app._spSystemPreview(keepLoaded.pages[0].cards[0],keepLoaded.settings).includes('aria-checked="false"'));
  const sleepSelect=h.normalizeConfig({pages:[{id:'system',layoutMode:'canvas',cards:[
   {id:'sleep-select',kind:'sleep-time',display:'select',rect:{x:8,y:8,w:220,h:144}}]}]});
  assert.equal(h.sanitizeConfig(sleepSelect,m.choices).pages[0].cards[0].visualType,'select');
 } else if(process.argv[1]==='unbound') {
  m.config=h.normalizeConfig({pages:[{id:'home',title:'Controls',layoutMode:'canvas',cards:[
   {id:'control',kind:'control',display:'action',rect:{x:8,y:8,w:220,h:144}}]}]});
  const errors=app._spValidateConfig(m.config).errors;assert.equal(errors.length,1);
  assert.ok(errors[0].includes('未选择控制项'));
 } else {
  m.deviceMac='E8:3D:C1:F7:65:44';m.revision=33;m.savedConfig=h.defaultConfig();m.config=h.clone(m.savedConfig);
  m.config.pages[0].title='Local draft';m.dirty=true;
  app._spPersistDraftSoon();for(const fn of timers.values())fn();timers.clear();
  const remote=h.clone(m.savedConfig);remote.settings.brightnessPercent=52;
  if(process.argv[1]==='draft-conflict')remote.pages[0].title='Remote edit';
  const restored=app._spRestoreDraft(remote,34);assert.ok(restored);
  assert.equal(restored.config.pages[0].title,'Local draft');
  assert.equal(restored.config.settings.brightnessPercent,52);
  if(process.argv[1]==='draft-conflict') {
   assert.equal(restored.baseRevision,33);assert.equal(restored.baseConfig.pages[0].title,m.savedConfig.pages[0].title);
   assert.deepEqual(plain(restored.conflict.paths),['pages']);
  } else {assert.equal(restored.baseRevision,34);assert.equal(restored.conflict,null);}
  m.recoverableDraft=restored;app._spMarkDirty=()=>{};let message='';app._spSetMessage=s=>message=s;
  app._spRecoverDraft();assert.equal(m.revision,restored.baseRevision);assert.equal(m.savedConfig,restored.baseConfig);
  assert.equal(!!message,process.argv[1]==='draft-conflict');
  m.deviceMac='14:C1:9F:C1:52:D8';assert.equal(app._spRestoreDraft(remote,34),null);
 }
}
'''


class ScreenComponentRoundtripTests(unittest.TestCase):
    def scenario(self, name):
        result = subprocess.run(['node', '-e', PROBE, name], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8', timeout=25)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_every_component_preserves_wire_fields_and_validates(self):
        self.scenario('roundtrip')

    def test_unbound_control_explains_required_configuration(self):
        self.scenario('unbound')

    def test_saved_draft_merges_new_device_settings_after_reload(self):
        self.scenario('draft-merge')

    def test_saved_draft_retains_original_baseline_on_layout_conflict(self):
        self.scenario('draft-conflict')
