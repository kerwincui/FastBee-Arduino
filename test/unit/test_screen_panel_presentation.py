"""Exercise production panel presentation, metadata admission and OTA guards."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_screen_snapshot_cache import definition

ROOT = Path(__file__).resolve().parents[2]


class ScreenPanelPresentationTests(unittest.TestCase):
    def test_real_web_editor_and_preview_in_source_and_minified_builds(self):
        result = subprocess.run(["node", "-e", WEB], cwd=ROOT, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("source/minified presentation passed", result.stdout)

    def test_device_clock_series_metadata_state_and_ota_admission(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Requires a C++17 compiler")
        source = (ROOT / "src/core/ScreenPanelManager.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/core/ScreenPanelManager.h").read_text(encoding="utf-8")
        snapshots = header[header.index("enum class ScreenCardKind"):header.index("struct ScreenPanelSnapshotCache;")]
        restart = definition(source, 'if (cardKind == "system")')
        schema = definition(source, 'if (!card["sourceRefs"].isNull())')
        admission = source[source.index("            String seriesUnit;"):source.index('            if (kind != "control") continue;')]
        pieces = {
            "@SNAPSHOTS@": snapshots,
            "@METADATA@": definition(source, "bool panelNumericSourceUnit("),
            "@CLOCK@": definition(source, "void appendPanelClock("),
            "@QUALITY@": definition(source, "const char* qualityName("),
            "@REFERENCE@": definition(source, "bool validReference("),
            "@STATE@": definition(source, "void ScreenPanelManager::appendState("),
            "@SCHEMA@": schema,
            "@ADMISSION@": admission,
            "@RESTART@": restart,
        }
        harness = CPP
        for marker, value in pieces.items():
            harness = harness.replace(marker, value)
        with tempfile.TemporaryDirectory(prefix="fastbee-panel-presentation-") as temp:
            folder = Path(temp)
            cpp = folder / "presentation.cpp"
            binary = folder / ("presentation.exe" if os.name == "nt" else "presentation")
            cpp.write_text(harness, encoding="utf-8")
            env = dict(os.environ)
            env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
            json = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h")).parent
            build = subprocess.run([compiler, "-std=c++17", "-DUNIT_TEST",
                                    "-DFASTBEE_ENABLE_OTA=1", "-DFASTBEE_ENABLE_REMOTE_RESOURCES=1",
                                    "-DARDUINOJSON_ENABLE_STD_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                                    f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}", f"-I{json}", str(cpp),
                                    str(ROOT / "src/network/FixedJsonField.cpp"), "-o", str(binary)],
                                   capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, env=env)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=15, env=env)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


WEB = r'''
const assert = require('node:assert/strict');
const fs = require('fs');
const vm = require('vm');
const { minifyJS } = require('./scripts/minify-js');
async function probe(minified) {
  const nodes = {};
  const sandbox = { console, setTimeout, clearTimeout, AppState: {},
    escapeHtml: x => String(x).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;'),
    document: { getElementById: id => nodes[id] || null },
    apiGet: () => Promise.reject(new Error('offline')),
    Date: class { constructor() { throw new Error('Preview must not use the browser wall clock'); } static now() { return 1700000000000; } }
  };
  sandbox.window = sandbox;
  const context = vm.createContext(sandbox);
  for (const module of ['screen-panel-model', 'screen-panel']) {
    const raw = fs.readFileSync(`web-src/modules/runtime/${module}.js`, 'utf8');
    vm.runInContext(minified ? minifyJS(raw) : raw, context);
  }
  const app = sandbox.AppState, helper = app._screenPanelModelHelpers, model = app._spEnsureModel();
  const clock = { id: 'clock', kind: 'clock' };
  assert.match(app._spCardContent(clock), /时间未同步/);
  model.state.clock = { valid: true, quality: 'good', time: '02:03', date: '2031-04-05' };
  assert.match(app._spCardContent(clock), /02:03/);
  assert.match(app._spCardContent(clock), /2031-04-05/);
  await app._spRefreshState(false);
  assert.match(app._spCardContent(clock), /设备时间未更新/);
  sandbox.apiGet = () => Promise.resolve({ success: true, data: { clock: { valid: false, quality: 'unknown' } } });
  await app._spRefreshState(false);
  assert.match(app._spCardContent(clock), /时间未同步/);
  assert.doesNotMatch(app._spCardContent(clock), /02:03/);
  model.choices = ['a','b','c','d'].map(ref => ({ sourceRef: ref, kind: 'data', type: 'number', unit: 'C', label: 'Sensor ' + ref }));
  model.choices.push({ sourceRef:'bad', kind:'data', type:'number', unit:'V' }, { sourceRef:'switch', kind:'data', type:'boolean' });
  model.config = helper.normalizeConfig({ pages: [{ id:'home', title:'Home', layoutMode:'canvas', logicalWidth:480, logicalHeight:480, cards:[
    { id:'bar', kind:'chart', visualType:'bar', sourceRef:'a', sourceRefs:['a','b','c','d'], rect:{x:0,y:0,w:300,h:196}, rangeMin:0, rangeMax:100 }
  ] }] });
  model.selectedPageId = 'home'; model.selectedCardId = 'bar';
  let bar = app._spCurrentCard();
  assert.equal(app._spValidateConfig(model.config).errors.length, 0);
  assert.equal(helper.sanitizeConfig(model.config, model.choices).pages[0].cards[0].sourceRefs.join(','), 'a,b,c,d');
  bar.sourceRefs = ['a','a']; assert.match(app._spValidateConfig(model.config).errors.join(), /不重复/);
  bar.sourceRefs = ['a','bad']; assert.match(app._spValidateConfig(model.config).errors.join(), /同单位/);
  bar.sourceRefs = ['a','switch']; assert.match(app._spValidateConfig(model.config).errors.join(), /同单位/);
  bar.sourceRefs = ['a','b','c','d','bad']; assert.match(app._spValidateConfig(model.config).errors.join(), /四个/);
  assert.equal(helper.normalizeConfig({pages:[{cards:[{kind:'bar',sourceRefs:bar.sourceRefs}]}]}).pages[0].cards[0].sourceRefs.length, 5);
  bar.sourceRefs = ['a','b'];
  const series = [{ sourceRef:'a',label:'Primary',value:'25',unit:'C',quality:'good',updatedMs:12300 },
    { sourceRef:'b',label:'<img onerror=attack()>',value:'75',unit:'C',quality:'stale',updatedMs:4000 }];
  model.state = { items:{ a:{series:[{sourceRef:'a',value:999}]} }, cards:[{id:'bar',sourceRef:'a',series}] };
  const html = app._spCardContent(bar);
  assert.match(html, /25\.0 C/); assert.match(html, /75\.0 C/); assert.match(html, /T\+12s/); assert.match(html, /已过期/);
  assert.match(html, /&lt;img/); assert.doesNotMatch(html, /<img|999/);
  series[1].unit = 'V'; assert.match(app._spCardContent(bar), /单位不一致/); assert.doesNotMatch(app._spCardContent(bar), /75\.0/);
  series[1].unit = 'C'; series[1].quality = 'unknown'; assert.doesNotMatch(app._spCardContent(bar), /75\.0/);
  bar.sourceRefs = ['a','c']; assert.doesNotMatch(app._spCardContent(bar), /75\.0/);
  app._spMutate = mutate => mutate(model.config); app._spSetMessage = message => { sandbox.message = message; };
  Object.assign(nodes, { 'sp-card-source':{value:'a'},'sp-card-source-1':{value:'b'},'sp-card-source-2':{value:'c'},'sp-card-source-3':{value:'d'} });
  app._spApplyInspector(); assert.equal(bar.sourceRefs.join(','), 'a,b,c,d');
  nodes['sp-card-source-2'].value = 'b'; app._spApplyInspector();
  assert.equal(bar.sourceRefs.join(','), 'a,b,c,d'); assert.match(sandbox.message,/不重复/);
  model.config.pages[0].layoutMode = 'template'; app._spLayoutTemplatePage(model.config.pages[0]);
  assert.ok(bar.rect.h >= 196);
}
Promise.resolve().then(() => probe(false)).then(() => probe(true)).then(() => console.log('source/minified presentation passed')).catch(error => { console.error(error); process.exit(1); });
'''

CPP = r'''
#include <Arduino.h>
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include <array>
#include <cassert>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
inline void convertFromJson(JsonVariantConst source, String& out) { out = source.as<std::string>(); }
@SNAPSHOTS@
struct ObservationSnapshot { bool bindingValid=true; String valueType="number", unit; };
struct PeriphExecManager {
    std::map<std::string, ObservationSnapshot> values;
    static PeriphExecManager& getInstance() { static PeriphExecManager value; return value; }
    bool getObservation(const String& ref, ObservationSnapshot& out) { auto it=values.find(ref.c_str()); if(it==values.end())return false; out=it->second;return true; }
};
namespace FastBee {
struct RemoteResourceManager {
    JsonDocument config;
    static RemoteResourceManager& instance(){static RemoteResourceManager value;return value;}
    void appendChoices(JsonArray sources, JsonArray) { for(JsonVariantConst source:config.as<JsonArrayConst>())sources.add(source); }
};
}
@METADATA@
@REFERENCE@
@QUALITY@
static time_t deviceEpoch=0;
static bool localAvailable=true;
time_t panelTime(time_t*) { return deviceEpoch; }
tm* panelLocalTime(const time_t*,tm* out) { if(!localAvailable)return nullptr; *out={};out->tm_year=131;out->tm_mon=3;out->tm_mday=5;out->tm_hour=2;out->tm_min=3;return out; }
#define time panelTime
#define localtime_r panelLocalTime
#define localtime_s(out, input) (panelLocalTime(input, out) ? 0 : 1)
@CLOCK@
#undef time
#undef localtime_r
#undef localtime_s
struct ScreenPanelManager {
    ScreenPanelSnapshot snapshot;
    bool getSnapshot(ScreenPanelSnapshot& out)const { out=snapshot;return true; }
    void appendState(JsonObject)const;
};
@STATE@
bool schema(JsonObjectConst card, ScreenVisualType visual, int w, int h, String& error) {
    const String cardPath="card";
    @SCHEMA@
    return true;
}
bool admission(JsonObject card, String& error) {
    auto validateSource=[](const String&){return true;};
    @ADMISSION@
    return true;
}
struct FakeOta { bool busy=false; bool isOTAInProgress()const{return busy;} };
static FakeOta ota;
class FastBeeFramework {
public:
    static FastBeeFramework* getInstance(){static FastBeeFramework value;return &value;}
    FakeOta* getOTAManager(){return &ota;}
};
enum class RestartReason {USER_COMMAND};
struct SystemRebooter { static int count; static bool isScheduled(){return count>0;} static void scheduleReboot(const char*,int,RestartReason){++count;} };
int SystemRebooter::count=0;
struct PanelLock { explicit PanelLock(void*){} bool locked()const{return true;} };
bool _restartSaveFailed=false,_restartWithoutHistorySave=false;
void* _mutex=nullptr;
std::array<String,24> _operationCardIds{};
std::array<uint32_t,24> _operationIds{};
bool restart(JsonDocument& config, bool target, uint32_t& operationId, String& error) {
    operationId=0;error="";
    const char* cardId="restart";
    const String cardKind="system";
    const ScreenActionKind kind=ScreenActionKind::SYSTEM_RESTART;
    JsonObjectConst selected=config["card"].as<JsonObjectConst>();
    @RESTART@
    return false;
}
int main() {
    JsonDocument clock; appendPanelClock(clock.to<JsonObject>());
    assert(!clock["valid"].as<bool>() && clock["time"].isNull());
    deviceEpoch=1790000000;clock.clear();appendPanelClock(clock.to<JsonObject>());
    assert(clock["valid"].as<bool>() && String(clock["time"]|"")=="02:03" && String(clock["date"]|"")=="2031-04-05");
    localAvailable=false;clock.clear();appendPanelClock(clock.to<JsonObject>());assert(!clock["valid"].as<bool>());localAvailable=true;
    auto& observations=PeriphExecManager::getInstance().values;
    observations["a"].unit="C";observations["b"].unit="C";observations["v"].unit="V";
    observations["bool"].valueType="boolean";observations["invalid"].bindingValid=false;
    String unit;assert(panelNumericSourceUnit("a",unit)&&unit=="C");
    assert(!panelNumericSourceUnit("bool",unit)&&!panelNumericSourceUnit("invalid",unit)&&!panelNumericSourceUnit("system.network",unit));
    auto remote=FastBee::RemoteResourceManager::instance().config.to<JsonArray>().add<JsonObject>();
    remote["sourceRef"]="remote.temp";remote["type"]="number";remote["unit"]="C";
    assert(panelNumericSourceUnit("remote.temp",unit)&&unit=="C");
    JsonDocument config;auto card=config.to<JsonObject>();card["sourceRef"]="a";
    auto refs=card["sourceRefs"].to<JsonArray>();refs.add("a");refs.add("b");String error;
    assert(schema(card,ScreenVisualType::BAR,300,116,error)&&admission(card,error));
    assert(!schema(card,ScreenVisualType::LINE,300,116,error));assert(!schema(card,ScreenVisualType::BAR,300,115,error));
    refs[1]="a";assert(!schema(card,ScreenVisualType::BAR,300,116,error));
    refs[1]="v";assert(!admission(card,error));refs[1]="bool";assert(!admission(card,error));
    refs[0]="b";assert(!schema(card,ScreenVisualType::BAR,300,116,error));
    ScreenPanelManager manager;manager.snapshot.cardCount=2;
    for(int i=0;i<2;++i) {
        auto& output=manager.snapshot.cards[i];std::strcpy(output.id,i?"second":"first");std::strcpy(output.sourceRef,"a");output.seriesCount=2;
        std::strcpy(output.series[0].sourceRef,"a");std::strcpy(output.series[1].sourceRef,i?"c":"b");
    }
    JsonDocument state;manager.appendState(state.to<JsonObject>());
    assert(String(state["cards"][0]["series"][1]["sourceRef"]|"")=="b");
    assert(String(state["cards"][1]["series"][1]["sourceRef"]|"")=="c");
    assert(state["clock"]["valid"].as<bool>());
    config.clear();config["settings"]["allowSystemRestart"]=true;config["card"]["systemActionId"]="restart";config["card"]["requireConfirmation"]=true;
    uint32_t operation=999;ota.busy=true;_restartSaveFailed=true;
    assert(!restart(config,false,operation,error)&&operation==0&&SystemRebooter::count==0&&_restartSaveFailed);
    assert(error.indexOf("OTA")>=0 && !restart(config,true,operation,error));
    ota.busy=false;assert(restart(config,false,operation,error)&&operation&&SystemRebooter::count==1);
    assert(!restart(config,false,operation,error)&&SystemRebooter::count==1);
}
'''
