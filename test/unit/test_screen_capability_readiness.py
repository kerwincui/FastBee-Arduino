"""Run real editor methods against changing physical-touch capabilities."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE = r'''
const assert=require('node:assert/strict'),fs=require('fs'),vm=require('vm');
const {minifyJS}=require('./scripts/minify-js');
async function run(minified) {
 const fields={};
 const get=id=>fields[id]||(fields[id]={textContent:'',value:'',disabled:false,classList:{toggle(name,on){this[name]=on;}}});
 const kinds=['brightness','sleep','sleep-time','buzzer','touch-beep','restart','confirm-idle'];
 const buttons=kinds.map(kind=>({dataset:{spAdd:kind}}));
 const sandbox={AppState:{},console,setTimeout,clearTimeout,document:{getElementById:get,querySelectorAll:()=>buttons}};
 sandbox.window=sandbox;const context=vm.createContext(sandbox);
 for(const name of ['screen-panel-model','screen-panel']) {
  const source=fs.readFileSync('web-src/modules/runtime/'+name+'.js','utf8');
  vm.runInContext(minified?minifyJS(source):source,context);
 }
 const app=sandbox.AppState,m=app._spEnsureModel();
 m.config.settings.brightnessPercent=52;app.currentPage='screen-panel';app._spRenderPreview=()=>{};
 const draft=JSON.stringify(m.config);
 const reason='touch controller unavailable; retrying';
 const unavailable={touchConfigured:true,touchReady:false,brightnessContinuous:false,buzzer:false,sleep:false,systemRestart:false,controlConfirmIdle:true,disabledReasons:{brightnessContinuous:reason,buzzer:reason,sleep:reason,systemRestart:reason}};
 let caps=unavailable;
 sandbox.apiGet=async()=>({success:true,data:{application:{applied:true,revision:28,status:'applied',runtime:{running:true}},capabilities:caps}});
 await app._spRefreshState(false);
 for(const button of buttons.filter(x=>x.dataset.spAdd!=='confirm-idle')) {assert.equal(button.disabled,true);assert.equal(button.draggable,false);assert.equal(button.title,reason);}
 assert.equal(buttons.at(-1).disabled,false);
 assert.equal(get('sp-system-capabilities').textContent,reason);
 assert.equal(get('sp-apply-summary').textContent,'');
 assert.equal(get('sp-brightness').disabled,true);
 assert.equal(get('sp-allow-restart').disabled,false); // Revocation remains editable.
 caps={touchConfigured:true,touchReady:true,brightnessContinuous:true,buzzer:true,sleep:true,systemRestart:true,controlConfirmIdle:true,disabledReasons:{}};
 await app._spRefreshState(false);
 for(const button of buttons) {assert.equal(button.disabled,false);assert.equal(button.draggable,true);}
 assert.equal(get('sp-system-capabilities').textContent,'');
 assert.equal(get('sp-system-capabilities').classList['fb-hidden'],true);
 assert.equal(get('sp-brightness-output').textContent,'52%');
 assert.equal(get('sp-touch-beep').disabled,false);
 assert.equal(get('sp-idle-sleep').disabled,false);
 assert.equal(get('sp-allow-restart').disabled,false);
 assert.equal(JSON.stringify(m.config),draft);
 let opened='',message='';app._spOpenProperties=mode=>opened=mode;app._spSetMessage=text=>message=text;
 app._spAddCard('restart');
 assert.equal(opened,'global');assert(message);assert.equal(JSON.stringify(m.config),draft);
 m.capabilities={brightnessContinuous:false,buzzer:false,touchWake:false};
 assert(!app._spSystemActionReason('buzzer').includes('hardware'));
}
(async()=>{await run(false);await run(true);console.log('PASS: raw/minified readiness recovery and restart permission');})().catch(e=>{console.error(e);process.exitCode=1;});
'''

class ScreenCapabilityReadinessTests(unittest.TestCase):
    def test_status_refresh_reenables_controls_without_editing_draft(self):
        result = subprocess.run(['node', '-e', PROBE], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', timeout=40)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_removed_redundant_action_link(self):
        html = (ROOT / 'web-src/pages/screen-panel.html').read_text(encoding='utf-8')
        self.assertNotIn('data-sp-nav="features"', html)
        self.assertIn('id="sp-allow-restart"', html)

if __name__ == '__main__':
    unittest.main()
