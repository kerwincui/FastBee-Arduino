"""Late reads must not discard a newer screen load or current selection."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE = r'''
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const {minifyJS}=require('./scripts/minify-js');
async function run(minified) {
 const box={AppState:{},structuredClone,console,URLSearchParams,location:{host:'device.test',search:''},
  document:{getElementById:()=>({classList:{toggle(){}},textContent:''})}};
 box.window=box;vm.createContext(box);
 for(const name of ['screen-panel-model','screen-panel']) {
  const source=fs.readFileSync('web-src/modules/runtime/'+name+'.js','utf8');vm.runInContext(minified?minifyJS(source):source,box);
 }
 const app=box.AppState,h=app._screenPanelModelHelpers,m=app._spEnsureModel(),reads=[];
 for(const name of ['_spSetupEvents','_spRenderAll','_spRenderWeather','_spStartStatePolling','_spPersistDraftSoon'])app[name]=()=>{};
 app._spLoadAllChoices=async()=>({items:[]});
 box.apiGet=box.apiGetFresh=async route=>{
  if(route==='/api/screen-panel/config')return new Promise(resolve=>reads.push(resolve));
  return {success:true,data:route==='/api/network/status'?{macAddress:'E8:3D:C1:F7:65:44'}:{}};
 };
 const older=app.loadScreenPanelPage(),newer=app.loadScreenPanelPage();
 const config=h.defaultConfig();config.pages[0].title='Newer';
 reads[1]({success:true,data:{revision:34,config}});await newer;
 reads[0]({success:true,data:{revision:33,config:h.defaultConfig()}});await older;
 assert.equal(m.revision,34);assert.equal(m.config.pages[0].title,'Newer');
 config.pages.push({...h.clone(config.pages[0]),id:'system',title:'Settings',cards:[{id:'clock',kind:'clock'}]});
 m.selectedPageId='system';m.selectedCardId='clock';
 const refresh=app.loadScreenPanelPage({noCache:true,preserveDraft:true});
 reads[2]({success:true,data:{revision:35,config}});await refresh;
 assert.equal(m.selectedPageId,'system');assert.equal(m.selectedCardId,'clock');assert.equal(m.revision,35);
 m.capabilities={brightnessContinuous:false,buzzer:false};
 const page={cards:['brightness','buzzer','touch-beep','clock','sleep'].map(kind=>({kind}))};
 app._spSupportedTemplate(page);assert.deepEqual(page.cards.map(c=>c.kind),['clock','sleep']);
}
(async()=>{await run(false);await run(true);})().catch(error=>{console.error(error);process.exit(1);});
'''


class ScreenLoadOrderTests(unittest.TestCase):
    def test_load_order_selection_and_template_capability_filters(self):
        result = subprocess.run(['node', '-e', PROBE], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
