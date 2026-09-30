"""Exercise screen editor multi-resource saves and weather status with real methods."""
import json
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE = r"""
const fs=require('fs'),vm=require('vm');
const {minifyJS}=require('./scripts/minify-js');
async function run(minified) {
 const fields={}; const get=id=>fields[id]||(fields[id]={value:'',checked:false,textContent:'',classList:{toggle(){}}});
 const sandbox={AppState:{},console,document:{getElementById:get},escapeHtml:String,setTimeout,clearTimeout};sandbox.window=sandbox;
 const context=vm.createContext(sandbox);
 for(const file of ['screen-panel-model','screen-panel']) {const src=fs.readFileSync('web-src/modules/runtime/'+file+'.js','utf8');vm.runInContext(minified?minifyJS(src):src,context);}
 const app=sandbox.AppState,m=app._spEnsureModel();
 const labels=['Air temperature','Air humidity','System uptime','心知天气 temperature','用户自定义'].map(label=>app._screenPanelModelHelpers.choiceLabel({label}));m.weather={revision:3,enabled:true,hasApiKey:true};m.historyConfig={revision:7,enabled:true,sourceAllowlist:['legacy.missing']};m.historyDraft=structuredClone(m.historyConfig);
 Object.assign(app,{guardDeveloperModeAction:()=>true,_spRenderAll(){},_spRenderDirtyState(){},_spRenderHistorySettings(){},_spRenderWeather(){},_spSetMessage(message){get('sp-page-message').textContent=message;},_spValidateConfig(){return {errors:[]};}});
 get('sp-weather-enabled').checked=true;get('sp-weather-location').value='beijing';get('sp-weather-interval').value='1800';get('sp-weather-key').value='';
 app._spWeatherStatus({blocked:true,pending:false,automaticRetryAfterMs:null,manualRetryAfterMs:9000,error:'waiting-time'});
 const blocked={text:get('sp-weather-status').textContent,disabled:get('sp-weather-refresh').disabled};
 app._spWeatherStatus({blocked:false,pending:false,automaticRetryAfterMs:2400,manualRetryAfterMs:0,error:'network'});
 const scheduled={text:get('sp-weather-status').textContent,disabled:get('sp-weather-refresh').disabled};
 app._spWeatherStatus({pending:false,manualRetryAfterMs:3000,automaticRetryAfterMs:null,seenAt:Date.now()-5000});
 const expired={disabled:get('sp-weather-refresh').disabled,remaining:m.weatherStatus.manualRetryAfterMs};
 const applicationCases = [
  { application:{applied:true,revision:21,status:'applied'} },
  { application:{status:'running',appliedRevision:21} },
  { appliedRevision:21,application:{status:'running'} },
  { application:{applied:false,revision:21,status:'pending'} },
  { application:{applied:true,revision:21,status:'apply_failed',error:'driver failed'} },
  { application:{applied:true,revision:21,status:'applied',restartRequired:true} },
  { revision:21,application:{revision:21} },
  { application:{status:'running',appliedRevision:20} }
 ];
 const applications=[];
 for (const state of applicationCases) {
  sandbox.apiGet=async()=>({success:true,data:state}); sandbox.apiGetFresh=sandbox.apiGet;
  await app._spRefreshState(false); app._spApplyStatus();
  applications.push({revision:app._screenPanelModelHelpers.displayStateMeta(m.application).appliedRevision,summary:get('sp-apply-summary').textContent});
 }
 const posts=[];sandbox.apiGet=async()=>({success:true,data:[]});sandbox.apiGetFresh=sandbox.apiGet;
 sandbox.apiPostJson=async(path,body)=>{posts.push({path,body});if(path.includes('/weather'))return {success:true,data:{revision:4,enabled:true,hasApiKey:true}};return {success:true,data:{config:{revision:8,enabled:body.enabled,sourceAllowlist:body.sourceAllowlist}}};};
 app._spLoadAllChoices=async()=>({items:[]});app._spSave=async()=>{get('sp-page-message').textContent='layout rejected';return false;};
 m.config.pages[0].cards=[{id:'trend',kind:'line',sourceRef:'sensor.temp'}];m.dirty=true;m.weatherDirty=true;
 m.historyConfig.maxSources=1;
 const invalid=await app._spSaveAll();const preflight={result:invalid,posts:posts.length};
 m.historyConfig.maxSources=16;
 const partial=await app._spSaveAll();const first=JSON.parse(JSON.stringify(posts));
 const retained={weatherRevision:m.weather.revision,weatherDirty:m.weatherDirty,historyRevision:m.historyConfig.revision,dirty:m.dirty,message:get('sp-page-message').textContent};
 app._spSave=async()=>{m.dirty=false;m.persisted=true;return true;};
 const retried=await app._spSaveAll();
 return {applications,labels,blocked,scheduled,expired,preflight,partial,first,retained,retried,postCount:posts.length};
}
(async()=>console.log(JSON.stringify([await run(false),await run(true)])))().catch(e=>{console.error(e);process.exit(1)});
"""

class ScreenEditorSaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run(['node', '-e', PROBE], cwd=ROOT, text=True, encoding='utf-8', capture_output=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.results = json.loads(result.stdout)

    def test_live_and_legacy_applied_versions_require_positive_status(self):
        for result in self.results:
            self.assertEqual([21, 21, 21, -1, -1, -1, -1, 20], [item['revision'] for item in result['applications']])
            for item in result['applications']:
                self.assertNotIn('设备已应用版本', item['summary'])
            self.assertEqual('driver failed', result['applications'][4]['summary'])

    def test_known_default_labels_are_chinese_and_custom_names_unchanged(self):
        for result in self.results:
            self.assertEqual(['空气温度', '空气湿度', '运行时长', '心知天气温度', '用户自定义'], result['labels'])

    def test_real_scheduler_status_separates_auto_retry_and_manual_cooldown(self):
        for result in self.results:
            self.assertEqual({'text': 'waiting-time', 'disabled': True}, result['blocked'])
            self.assertIn('3 秒后', result['scheduled']['text'])
            self.assertFalse(result['scheduled']['disabled'])
            self.assertEqual({'disabled': False, 'remaining': 0}, result['expired'])

    def test_invalid_shared_history_prevents_any_weather_write(self):
        for result in self.results:
            self.assertEqual({'result': False, 'posts': 0}, result['preflight'])

    def test_partial_save_preserves_draft_and_retries_only_unsaved_layout(self):
        for result in self.results:
            self.assertFalse(result['partial'])
            self.assertTrue(result['retried'])
            self.assertEqual(2, result['postCount'])
            self.assertEqual(3, result['first'][0]['body']['expectedRevision'])
            self.assertEqual(7, result['first'][1]['body']['expectedRevision'])
            self.assertEqual(['legacy.missing', 'sensor.temp'], result['first'][1]['body']['sourceAllowlist'])
            self.assertEqual(8, result['retained']['historyRevision'])
            self.assertEqual(4, result['retained']['weatherRevision'])
            self.assertFalse(result['retained']['weatherDirty'])
            self.assertTrue(result['retained']['dirty'])
            self.assertIn('layout rejected', result['retained']['message'])
