"""Run real editor ordering/sleep methods in source and minified form, offline."""
import json
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE = r"""
const fs = require('fs'), vm = require('vm');
const { minifyJS } = require('./scripts/minify-js');
function run(minified) {
 const fields = {};
 const box = () => ({ value:'', checked:false, disabled:false, title:'', textContent:'',
  options:[{value:'0'},{value:'30'},{value:'120'}], appendChild(option){this.options.push(option);},
  classList:{toggle(){},remove(){}} });
 for (const id of ['sp-idle-sleep','sp-auto-sleep-enabled','sp-sleep-capability']) fields[id]=box();
 const sandbox = {AppState:{},console,escapeHtml:String,setTimeout,clearTimeout,
  document:{getElementById:id=>fields[id]||null,createElement:()=>box()}};
 sandbox.window=sandbox;
 const context=vm.createContext(sandbox);
 for(const name of ['screen-panel-model','screen-panel']) {
  const source=fs.readFileSync('web-src/modules/runtime/'+name+'.js','utf8');
  vm.runInContext(minified?minifyJS(source):source,context);
 }
 const app=sandbox.AppState, h=app._screenPanelModelHelpers, m=app._spEnsureModel();
 app._spRenderAll=()=>{}; app._spRenderDirtyState=()=>{}; app._spPersistDraftSoon=()=>{};
 let error=''; app._spSetLayoutError=value=>{error=value;};
 m.loaded=true; m.capabilities={sleep:true};
 m.config=h.normalizeConfig({settings:{homePageId:'b',bottomNavPageIds:['a','b','c'],idleSleepSeconds:45},
  pages:['a','b','c'].map(id=>({id,title:id,layoutMode:'template',cards:[
   {id:id+'1',kind:'data',sourceRef:'sensor.temperature',size:'small'},
   {id:id+'2',kind:'control',controlRef:'relay.6.1',display:'switch',size:'small'},
   {id:id+'3',kind:'clock',size:'small'}]}))});
 for(const page of m.config.pages)app._spLayoutTemplatePage(page);
 m.savedConfig=h.clone(m.config); m.selectedPageId='b';
 const initial=JSON.stringify(h.sanitizeConfig(m.config));
 app._spReorderItem('page','c','a');
 const pageOrder={ids:m.config.pages.map(p=>p.id),nav:m.config.settings.bottomNavPageIds,
  home:m.config.settings.homePageId,selected:m.selectedPageId,undo:m.undo.length};
 app._spUndo(); const undoExact=JSON.stringify(h.sanitizeConfig(m.config))===initial;
 app._spRedo(); const redoOrder=m.config.pages.map(p=>p.id);
 let history=m.undo.length;
 app._spReorderItem('page','missing','a');app._spReorderItem('page','a','a');
 const noopHistory=m.undo.length===history;
 const originalPage=JSON.stringify(m.config.pages.find(p=>p.id==='b'));
 const moved=app._spReorderItem('card','b3','b1');
 const page=m.config.pages.find(p=>p.id==='b');
 const cardOrder={moved,ids:page.cards.map(c=>c.id),valid:!h.templatePageNeedsLayout(page,m.config,app),
  source:page.cards.find(c=>c.id==='b1').sourceRef,control:page.cards.find(c=>c.id==='b2').controlRef};
 app._spUndo();const cardUndoExact=JSON.stringify(m.config.pages.find(p=>p.id==='b'))===originalPage;
 m.config.pages.find(p=>p.id==='b').layoutMode='canvas';history=m.undo.length;
 const canvasRejected=app._spReorderItem('card','b3','b1')===false && m.undo.length===history;
 const tiny=m.config.pages.find(p=>p.id==='b');tiny.layoutMode='template';tiny.canvas={width:80,height:80};
 const tinyBefore=JSON.stringify(tiny);history=m.undo.length;
 const overflowRejected=app._spReorderItem('card','b3','b1')===false && m.undo.length===history && JSON.stringify(tiny)===tinyBefore;
 app._spRenderAutoSleep(); app._spSetAutoSleepEnabled(false);app._spRenderAutoSleep();
 const disabled={value:m.config.settings.idleSleepSeconds,checked:fields['sp-auto-sleep-enabled'].checked,delayDisabled:fields['sp-idle-sleep'].disabled};
 app._spSetAutoSleepEnabled(true);app._spRenderAutoSleep();
 const restored={value:m.config.settings.idleSleepSeconds,checked:fields['sp-auto-sleep-enabled'].checked,custom:fields['sp-idle-sleep'].options.some(o=>Number(o.value)===45)};
 m.capabilities.sleep=false;app._spRenderAutoSleep();
 const canDisableUnsupported=!fields['sp-auto-sleep-enabled'].disabled;
 app._spSetAutoSleepEnabled(false);app._spRenderAutoSleep();history=m.undo.length;
 app._spSetAutoSleepEnabled(true);app._spRenderAutoSleep();
 const unsupported={value:m.config.settings.idleSleepSeconds,toggleDisabled:fields['sp-auto-sleep-enabled'].disabled,unchangedHistory:m.undo.length===history};
 const wire=h.sanitizeConfig(m.config);
 const invalidTimeouts=[-1,0.5,86401,'30'].map(value=>{
  const copy=h.defaultConfig();copy.settings.idleSleepSeconds=value;
  return app._spValidateConfig(copy).errors.some(e=>e.includes('86400'));
 });
 return {pageOrder,undoExact,redoOrder,noopHistory,cardOrder,cardUndoExact,canvasRejected,overflowRejected,error,
  disabled,restored,canDisableUnsupported,unsupported,wireTimeout:wire.settings.idleSleepSeconds,
  extraWireFields:Object.keys(wire.settings).filter(k=>/autoSleep|orderPointer/.test(k)),invalidTimeouts};
}
console.log(JSON.stringify([run(false),run(true)]));
"""


class ScreenEditorOrderSleepTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run(['node', '-e', PROBE], cwd=ROOT, text=True,
                                encoding='utf-8', capture_output=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.results = json.loads(result.stdout)

    def test_page_order_preserves_home_selection_and_navigation_membership(self):
        for result in self.results:
            self.assertEqual({'ids': ['c', 'a', 'b'], 'nav': ['c', 'a', 'b'],
                              'home': 'b', 'selected': 'b', 'undo': 1}, result['pageOrder'])

    def test_order_changes_are_undoable_and_noops_do_not_add_history(self):
        for result in self.results:
            self.assertTrue(result['undoExact'])
            self.assertEqual(['c', 'a', 'b'], result['redoOrder'])
            self.assertTrue(result['noopHistory'])

    def test_template_reorder_preserves_bindings_and_valid_geometry(self):
        for result in self.results:
            self.assertEqual({'moved': True, 'ids': ['b3', 'b1', 'b2'], 'valid': True,
                              'source': 'sensor.temperature', 'control': 'relay.6.1'}, result['cardOrder'])
            self.assertTrue(result['cardUndoExact'])

    def test_canvas_and_overflow_reordering_do_not_mutate_layout(self):
        for result in self.results:
            self.assertTrue(result['canvasRejected'])
            self.assertTrue(result['overflowRejected'])

    def test_auto_sleep_switch_maps_to_existing_zero_timeout_contract(self):
        for result in self.results:
            self.assertEqual({'value': 0, 'checked': False, 'delayDisabled': True}, result['disabled'])
            self.assertEqual(0, result['wireTimeout'])
            self.assertEqual([], result['extraWireFields'])

    def test_auto_sleep_restores_custom_delay_within_the_edit_session(self):
        for result in self.results:
            self.assertEqual({'value': 45, 'checked': True, 'custom': True}, result['restored'])

    def test_missing_wake_capability_allows_disable_but_prevents_enable(self):
        for result in self.results:
            self.assertTrue(result['canDisableUnsupported'])
            self.assertEqual({'value': 0, 'toggleDisabled': True, 'unchangedHistory': True}, result['unsupported'])

    def test_invalid_timeouts_are_rejected_before_saving(self):
        for result in self.results:
            self.assertTrue(all(result['invalidTimeouts']))


if __name__ == '__main__':
    unittest.main()
