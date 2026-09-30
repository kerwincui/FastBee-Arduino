"""Exercise real editor saves, concurrent settings, and layout size guards."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE = r"""
const assert = require('node:assert/strict'), fs = require('node:fs'), vm = require('node:vm');
const {minifyJS} = require('./scripts/minify-js');
function fixture(minified) {
 const fields = {};
 const sandbox = {AppState:{}, console, structuredClone, setTimeout, clearTimeout,
  document:{getElementById:id=>fields[id] ||= {value:'',textContent:'',classList:{toggle(){}}}}};
 sandbox.window = sandbox;
 const context = vm.createContext(sandbox);
 for (const file of ['screen-panel-model','screen-panel']) {
  const src = fs.readFileSync('web-src/modules/runtime/'+file+'.js','utf8');
  vm.runInContext(minified ? minifyJS(src) : src, context);
 }
 const app=sandbox.AppState, h=app._screenPanelModelHelpers, m=app._spEnsureModel();
 Object.assign(app,{_spRenderAll(){},_spRenderDirtyState(){},_spPersistDraftSoon(){}});
 m.config=h.normalizeConfig(h.defaultConfig()); m.savedConfig=structuredClone(m.config);
 m.revision=33; m.dirty=true; m.loaded=true; m.persisted=true;
 return {app,h,m,sandbox,fields};
}
async function scenario(name,minified) {
 const {app,h,m,sandbox,fields}=fixture(minified);
 if(name==='slider') {
  const p=m.config.pages[0];p.layoutId='data-detail';
  p.cards=['brightness','sleep-time','touch-beep','confirm-idle','sleep'].map((kind,i)=>Object.assign({id:'c'+i,kind},h.CARD_DEFAULTS[kind]));
  app._spLayoutTemplatePage(p);
  assert.ok(p.cards[0].rect.w>=180 && p.cards[0].rect.h>=80, JSON.stringify(p.cards[0].rect));
  assert.equal(app._spValidateConfig(m.config).errors.length,0);
  assert.equal(app._spMinimumSize(p.cards[0]).w,180);
  assert.equal(app._spMinimumSize(p.cards[0]).h,80);
  p.layoutMode='canvas';p.cards=p.cards.slice(0,1);p.cards[0].rect={x:0,y:0,w:180,h:79};
  let posts=0;sandbox.apiPostJson=async()=>{posts++;return {success:true};};
  assert.equal(await app._spSave(),false);assert.equal(posts,0);
  assert.match(fields['sp-page-message'].textContent,/180.*80/);
  return;
 }
 if(name==='sizes') {
  const p=m.config.pages[0];p.layoutMode='canvas';
  p.cards=[Object.assign({id:'brightness',kind:'brightness'},h.CARD_DEFAULTS.brightness)];
  assert.equal(p.cards[0].size,'wide');
  p.cards[0].rect=app._spPresetRect(p.cards[0],p);
  const roundtrip=h.normalizeConfig(h.sanitizeConfig(m.config));
  assert.equal(roundtrip.pages[0].cards[0].size,'custom');
  assert.deepEqual(roundtrip.pages[0].cards[0].rect,p.cards[0].rect);
  return;
 }
 const base=h.sanitizeConfig(m.savedConfig),remote=structuredClone(base),posts=[];
 let gets=0;
 m.config.pages[0].title='Local layout';
 remote.settings.brightnessPercent=52;
 if(name==='setting-conflict') m.config.settings.brightnessPercent=85;
 if(name==='conflict') remote.pages[0].title='Other layout';
 if(name==='same') remote.pages[0].title='Local layout';
 sandbox.apiGetFresh=async()=>{gets++;if(name==='offline') throw new Error('offline');return {success:true,data:{revision:34,config:remote}};};
 sandbox.apiGet=sandbox.apiGetFresh;
 sandbox.apiPostJson=async(path,body)=>{
  posts.push(structuredClone(body));
  if(name==='duplicate' || name==='pending-edit') {
   if(name==='duplicate') assert.equal(await app._spSave(),false);
   if(name==='pending-edit') m.config.pages[0].title='Typed during save';
   return {success:true,data:{revision:34,config:body.config}};
  }
  if(posts.length===1 || name==='race') throw {status:409,data:{currentRevision:34}};
  if(name==='failure') throw {status:400,data:{error:'specific validation reason'}};
  return {success:true,data:{revision:35,config:body.config}};
 };
 const result=await app._spSave();
 if(name==='merge') {
  assert.equal(result,true);assert.equal(gets,1);assert.equal(posts.length,2);
  assert.equal(posts[0].expectedRevision,33);assert.equal(posts[1].expectedRevision,34);
  assert.equal(posts[1].config.settings.brightnessPercent,52);
  assert.equal(posts[1].config.pages[0].title,'Local layout');
  assert.equal(m.revision,35);assert.equal(m.dirty,false);
 } else if(name==='same') {
  assert.equal(result,true);assert.equal(posts.length,1);assert.equal(m.revision,34);
  assert.equal(m.config.settings.brightnessPercent,52);assert.equal(m.dirty,false);
 } else if(name==='conflict') {
  assert.equal(result,false);assert.equal(posts.length,1);assert.equal(m.revision,33);
  assert.equal(m.config.pages[0].title,'Local layout');assert.equal(m.dirty,true);
  assert.match(fields['sp-page-message'].textContent,/34/);
  assert.doesNotMatch(fields['sp-page-message'].textContent,/另一窗口/);
 } else if(name==='race') {
  assert.equal(result,false);assert.equal(gets,1);assert.equal(posts.length,2);
  assert.equal(m.dirty,true);assert.equal(m.config.pages[0].title,'Local layout');
 } else if(name==='failure') {
  assert.equal(result,false);assert.equal(m.dirty,true);
  assert.match(fields['sp-page-message'].textContent,/specific validation reason/);
 } else if(name==='setting-conflict' || name==='offline') {
  assert.equal(result,false);assert.equal(posts.length,1);assert.equal(m.revision,33);
  assert.equal(m.dirty,true);assert.equal(m.config.pages[0].title,'Local layout');
  if(name==='setting-conflict') assert.equal(m.config.settings.brightnessPercent,85);
 } else if(name==='duplicate') {
  assert.equal(result,true);assert.equal(posts.length,1);assert.equal(m.revision,34);
 } else if(name==='pending-edit') {
  assert.equal(result,true);assert.equal(posts.length,1);assert.equal(m.revision,34);
  assert.equal(m.dirty,true);assert.equal(m.config.pages[0].title,'Typed during save');
  assert.equal(m.savedConfig.pages[0].title,'Local layout');
 }
 assert.equal(m.layoutSaving,false);
}
(async()=>{for(const minified of [false,true])await scenario(process.argv[1],minified);})().catch(e=>{console.error(e);process.exit(1);});
"""


class ScreenSaveConflictTests(unittest.TestCase):
    def scenario(self, name):
        result = subprocess.run(['node', '-e', PROBE, name], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_slider_template_and_save_guard(self):
        self.scenario('slider')

    def test_saved_rect_is_not_mislabeled_small(self):
        self.scenario('sizes')

    def test_device_setting_update_merges_without_losing_local_layout(self):
        self.scenario('merge')

    def test_already_saved_layout_is_acknowledged_without_another_write(self):
        self.scenario('same')

    def test_real_layout_conflict_preserves_draft(self):
        self.scenario('conflict')

    def test_retry_is_bounded_under_continuous_device_changes(self):
        self.scenario('race')

    def test_device_validation_error_survives_retry(self):
        self.scenario('failure')

    def test_same_setting_conflict_does_not_overwrite_either_side(self):
        self.scenario('setting-conflict')

    def test_offline_conflict_read_preserves_original_baseline(self):
        self.scenario('offline')

    def test_duplicate_save_is_not_submitted_twice(self):
        self.scenario('duplicate')

    def test_edit_during_pending_request_is_retained(self):
        self.scenario('pending-edit')
