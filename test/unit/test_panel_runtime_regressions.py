"""Production snapshot ownership, built-in values and route allocation policy."""
from pathlib import Path
import json as jsonlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def block(text, marker):
    start = text.index('{', text.index(marker))
    depth = 0
    for token in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', text[start:]):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return text[start:start + token.end()]
    raise AssertionError(marker)


class PanelRuntimeRegressionTests(unittest.TestCase):
    def test_redraw_hash_tracks_visual_changes_not_receipt_time(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        production = runtime[runtime.index('uint32_t hashBytes('):runtime.index('void updateRuntimeState(')]
        self.compile_run(r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <ctime>
time_t testTime(time_t*) { return 123456; }
#define time testTime
TYPES
PRODUCTION
int main() {
    ScreenPanelSnapshot s; s.pageCount=1; s.cardCount=1; s.pages[0].cardCount=1;
    strcpy(s.cards[0].value,"22.5");
    auto first=snapshotHash(s,"");
    s.cards[0].updatedMs=500;
    assert(snapshotHash(s,"")==first);
    strcpy(s.cards[0].value,"22.6");
    assert(snapshotHash(s,"")!=first);
    first=snapshotHash(s,""); s.cards[0].pending=true;
    assert(snapshotHash(s,"")!=first);
    first=snapshotHash(s,""); s.cards[0].history[0].value=24;
    assert(snapshotHash(s,"")!=first);
    first=snapshotHash(s,""); strcpy(s.cards[0].targetPageId,"next");
    assert(snapshotHash(s,"")!=first);
}
'''.replace('TYPES',types).replace('PRODUCTION',production))

    def compile_run(self, source, includes=(), expected=0, extra_sources=()):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-panel-runtime-') as directory:
            base = Path(directory)
            cpp = base / 'case.cpp'
            exe = base / ('case.exe' if os.name == 'nt' else 'case')
            cpp.write_text(source, encoding='utf-8')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     *[f'-I{p}' for p in includes], str(cpp),
                                     *[str(p) for p in extra_sources], '-o', str(exe)],
                                    capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=40)
            self.assertEqual(result.returncode, 0, (result.stdout or "") + (result.stderr or ""))
            result = subprocess.run([str(exe)], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=10)
            if expected == 0:
                self.assertEqual(result.returncode, 0, (result.stdout or "") + (result.stderr or ""))
            else:
                self.assertEqual(result.returncode, expected,
                                 'Mutation must fail at the ownership assertion, not crash or hang: ' +
                                 (result.stdout or "") + (result.stderr or ""))

    def test_config_page_link_snapshot_dispatches_its_target(self):
        manager = (ROOT / 'src/core/ScreenPanelManager.cpp').read_text(encoding='utf-8')
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        snapshot_body = block(manager, 'bool ScreenPanelManager::getSnapshotImpl(')
        begin = snapshot_body.index('copyText(cardOut.id,')
        end = snapshot_body.index('cardOut.rect.x', begin)
        populate = snapshot_body[begin:end]
        parse_kind = 'bool parseKind(const String& value, ScreenCardKind& out)' + block(manager, 'bool parseKind(')
        parse_visual = 'bool parseVisual(const String& value, ScreenVisualType& out)' + block(manager, 'bool parseVisual(')
        handle = 'void handleTap(StripeRenderer& renderer, ScreenPanelSnapshot& snapshot, uint16_t x, uint16_t y, char* confirmationCardId, size_t confirmationCapacity, ScreenControlSession* controlSession=nullptr, uint32_t now=0)' + block(runtime, 'void handleTap(')
        # The preserved layout uses the four-tab footer. Navigation is
        # deliberately independent from page cards, so there are no nested
        # page-link controls to dispatch.
        layout = (ROOT / 'docs/device-test-configs/com8-com9-esp32s3-touch/config/screen_panel.json').read_text(encoding='utf-8')
        pages = jsonlib.loads(layout)['config']['pages']
        self.assertEqual(4, len(pages))
        self.assertFalse(any(card['kind'] == 'page-link' for page in pages for card in page['cards']))
        source = r'''
#include <ArduinoJson.h>
#include "screen/ScreenInteraction.h"
using FastBee::Screen::ScreenControlSession;
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
using String=std::string;
TYPES
template<size_t N>void copyText(char(&to)[N],const String& from){snprintf(to,N,"%s",from.c_str());}
size_t strlcpy(char* to,const char* from,size_t n){if(n)snprintf(to,n,"%s",from);return strlen(from);}
PARSE_KIND
PARSE_VISUAL
void populateCard(JsonObjectConst page,JsonObjectConst card,ScreenPanelCardSnapshot& cardOut){POPULATE}
enum class NumericControlHit : uint8_t { NONE = 0, DECREMENT, INCREMENT, EXECUTE };
class StripeRenderer {
public:
 NumericControlHit numericHit=NumericControlHit::NONE;
 bool confirmVisible=false,confirmAnswer=false,stopHit=false,lastWasMotor=false;
 bool navigationOpen()const{return false;} void toggleNavigation(){} void closeNavigation(){}
 int pageMenuHit(const ScreenPanelSnapshot&,uint16_t,uint16_t)const{return -1;}
 int navigationPageIndex(const ScreenPanelSnapshot& s,uint8_t i)const{return i<s.pageCount?i:-1;}
 int pageNavigationHit(const ScreenPanelSnapshot&,uint16_t x,uint16_t y)const{return y>=436 ? std::min<int>(4, x/96) : -1;}
 bool confirmationStopHit(uint16_t,uint16_t)const{return stopHit;}
 bool homeHitTest(uint16_t x,uint16_t y)const{return x<96&&y>=436&&y<480;}
 int hitTest(const ScreenPanelSnapshot&,uint16_t,uint16_t)const{return 0;}
 NumericControlHit numericControlHitTest(const ScreenPanelSnapshot&,const ScreenPanelCardSnapshot&,uint16_t,uint16_t)const{return numericHit;}
 bool confirmationChoice(uint16_t,uint16_t,bool& confirmed,bool motor=false){lastWasMotor=motor;confirmed=confirmAnswer;return confirmVisible;}
};
class ScreenPanelManager {
public:
 String selected,lastCommand; unsigned selectedCount=0,stopCount=0;
 static ScreenPanelManager& instance(){static ScreenPanelManager manager;return manager;}
 bool setActivePage(const String& page){selected=page;++selectedCount;return true;}
 bool dispatchAction(const char*,ScreenActionKind kind,const String& value,uint32_t&,String&,const char* expected=nullptr){assert(kind==ScreenActionKind::MOTOR_COMMAND);assert(expected&&std::string(expected)=="modbus.motor");lastCommand=value;++stopCount;return true;}
};
bool handleContentTap(StripeRenderer&,ScreenPanelSnapshot&,ScreenPanelCardSnapshot&,uint16_t,uint16_t,char*,size_t,ScreenControlSession*,uint32_t){return false;}
unsigned dispatched=0; bool dispatchSucceeds=true;
bool dispatchCardAction(ScreenPanelCardSnapshot&,bool=false,uint32_t=0){++dispatched;return dispatchSucceeds;}
HANDLE
int main(){
 JsonDocument doc;
 assert(!deserializeJson(doc,R"layout(LAYOUT)layout"));
 auto& manager=ScreenPanelManager::instance();
 StripeRenderer renderer;ScreenPanelSnapshot snapshot; snapshot.cardCount=1; snapshot.pageCount=5;
 for (int i=0;i<5;++i) snprintf(snapshot.pages[i].id,sizeof(snapshot.pages[i].id),"page%d",i);
 char confirmation[41]{};
 assert(doc["config"]["pages"].size()==4);
 // The preserved layout has four pages. Independently cover five synthetic
 // footer targets and ensure navigation never dispatches the card underneath.
 handleTap(renderer,snapshot,40,458,confirmation,sizeof(confirmation));
 assert(manager.selected=="page0"&&manager.selectedCount==1&&dispatched==0);
 handleTap(renderer,snapshot,140,458,confirmation,sizeof(confirmation));
 assert(manager.selected=="page1"&&manager.selectedCount==2&&dispatched==0);
 handleTap(renderer,snapshot,240,458,confirmation,sizeof(confirmation));
 assert(manager.selected=="page2"&&manager.selectedCount==3&&dispatched==0);
 handleTap(renderer,snapshot,340,458,confirmation,sizeof(confirmation));
 assert(manager.selected=="page3"&&manager.selectedCount==4&&dispatched==0);
 handleTap(renderer,snapshot,440,458,confirmation,sizeof(confirmation));
 assert(manager.selected=="page4"&&manager.selectedCount==5&&dispatched==0);
 // Taps on the footer always stay navigation-only, even when the current
 // page has an interactive card beneath the same x-coordinate.
 snapshot.pageCount=2;strcpy(snapshot.pages[0].id,"first");strcpy(snapshot.pages[1].id,"second");
 snapshot.activePageIndex=0; strcpy(confirmation,"pending-motor");
 handleTap(renderer,snapshot,140,458,confirmation,sizeof(confirmation));
 assert(manager.selected=="second"&&manager.selectedCount==6&&confirmation[0]==0&&dispatched==0);
 snapshot.activePageIndex=1;
 handleTap(renderer,snapshot,40,458,confirmation,sizeof(confirmation));
 assert(manager.selected=="first"&&manager.selectedCount==7&&dispatched==0);
 // Populate a page-link using the production snapshot fields, then dispatch
 // its target through the production tap handler without invoking an action.
 JsonDocument link;
 assert(!deserializeJson(link,R"({"id":"page-entry","kind":"page-link","visualType":"navigation","pageId":"second"})"));
 auto& pageLink=snapshot.cards[0];
 populateCard(doc["config"]["pages"][0].as<JsonObjectConst>(),link.as<JsonObjectConst>(),pageLink);
 assert(pageLink.kind==ScreenCardKind::PAGE_LINK&&pageLink.visualType==ScreenVisualType::NAVIGATION);
 assert(std::string(pageLink.targetPageId)=="second");
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));
 assert(manager.selected=="second"&&manager.selectedCount==8&&dispatched==0);
 // An unconfigured target must not select a page or submit a control action.
 link.remove("pageId");
 populateCard(doc["config"]["pages"][0].as<JsonObjectConst>(),link.as<JsonObjectConst>(),pageLink);
 assert(!pageLink.targetPageId[0]);
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));
 assert(manager.selected=="second"&&manager.selectedCount==8&&dispatched==0);
 // Numeric +/- edits retain precision and bounds; only Execute submits.
 auto& numeric=snapshot.cards[0]; numeric={}; strcpy(numeric.id,"numeric");
 numeric.kind=ScreenCardKind::CONTROL; numeric.visualType=ScreenVisualType::NUMBER_CONTROL;
 numeric.rangeMin=0;numeric.rangeMax=1;numeric.numericValue=.9f;numeric.numericStep=.1f;numeric.precision=1;
 renderer.numericHit=NumericControlHit::INCREMENT;
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));
 assert(numeric.numericValue==1.f&&dispatched==0);
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));
 assert(numeric.numericValue==1.f&&dispatched==0);
 renderer.numericHit=NumericControlHit::DECREMENT;
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));
 assert(std::fabs(numeric.numericValue-.9f)<.0001f&&dispatched==0);
 numeric.pending=true;handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));
 assert(std::fabs(numeric.numericValue-.9f)<.0001f&&dispatched==0);numeric.pending=false;
 renderer.numericHit=NumericControlHit::NONE;
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));assert(dispatched==0);
 renderer.numericHit=NumericControlHit::EXECUTE;
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));assert(dispatched==1);
 numeric.requiresConfirmation=true;
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));
 assert(std::string(confirmation)=="numeric"&&dispatched==1);
 renderer.confirmVisible=true;renderer.confirmAnswer=false;
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));assert(!confirmation[0]&&dispatched==1);
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));renderer.confirmAnswer=true;
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation));
 assert(!confirmation[0]&&dispatched==2&&!renderer.lastWasMotor);
 // A motor session is approved only after accepted dispatch; stop remains direct in a modal.
 auto& motor=snapshot.cards[0];motor={};strcpy(motor.id,"motor");strcpy(motor.controlRef,"modbus.motor");
 motor.kind=ScreenCardKind::CONTROL;motor.visualType=ScreenVisualType::MOTOR_CONTROL;
 ScreenControlSession session;session.observe(snapshot.revision,100,60);
 strcpy(confirmation,"motor");dispatchSucceeds=false;
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation),&session,100);
 assert(!session.allows(motor.controlRef)&&renderer.lastWasMotor&&dispatched==3);
 strcpy(confirmation,"motor");dispatchSucceeds=true;
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation),&session,200);
 assert(session.allows(motor.controlRef)&&dispatched==4);
 strcpy(confirmation,"motor");motor.motorConfirmAction=2;renderer.stopHit=true;
 handleTap(renderer,snapshot,30,100,confirmation,sizeof(confirmation),&session,300);
 assert(manager.stopCount==1&&manager.lastCommand=="stop"&&dispatched==4);
 assert(!confirmation[0]&&motor.motorConfirmAction==0&&motor.pending);
 session.observe(snapshot.revision,60200,60);assert(!session.allows(motor.controlRef));
}
'''.replace('TYPES', types).replace('PARSE_KIND', parse_kind).replace('PARSE_VISUAL', parse_visual).replace('POPULATE', populate).replace('HANDLE', handle).replace('LAYOUT', layout)
        json = ROOT / '.pio/libdeps/esp32s3-F16R8/ArduinoJson/src'
        self.compile_run(source, [json, ROOT / "include"])

    def test_state_owns_all_snapshot_strings_after_snapshot_destruction(self):
        manager = (ROOT / 'src/core/ScreenPanelManager.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        # Poison temporary storage before delete to make the original dangling
        # string bug reproducible without depending on allocator reuse timing.
        types = types.replace('std::array<ScreenPanelCardSnapshot, MAX_CARDS> cards{};',
            'std::array<ScreenPanelCardSnapshot, MAX_CARDS> cards{};\n'
            '~ScreenPanelSnapshot(){std::memset(static_cast<void*>(cards.data()),0,sizeof(cards));}')
        production = 'void ScreenPanelManager::appendState(JsonObject out) const ' + block(
            manager, 'void ScreenPanelManager::appendState(JsonObject out) const')
        source = r'''
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
TYPES
const char* qualityName(ScreenValueQuality) {return "good";}
class ScreenPanelManager {
public:
 void appendState(JsonObject) const;
 bool getSnapshot(ScreenPanelSnapshot& value) const {
    value.cardCount=1;
    auto& c=value.cards[0];
    std::strcpy(c.id,"temperature"); std::strcpy(c.sourceRef,"sensor.temperature");
    std::strcpy(c.controlRef,"rule.switch");std::strcpy(c.value,"23.5");std::strcpy(c.unit,"C");
    std::strcpy(c.error,"sample error");std::strcpy(c.historyError,"history error");
    std::strcpy(c.weatherCondition,"sunny");std::strcpy(c.weatherCity,"Shanghai");
    c.seriesCount=1;std::strcpy(c.series[0].sourceRef,"system.uptime");
    std::strcpy(c.series[0].label,"uptime");std::strcpy(c.series[0].unit,"s");
    std::strcpy(c.series[0].value,"42");return true;
 }
};
void appendPanelClock(JsonObject) {}
PRODUCTION
int main(){
 JsonDocument doc; ScreenPanelManager manager; manager.appendState(doc.to<JsonObject>());
 std::string serialized; serializeJson(doc,serialized);JsonDocument restored;
 if(deserializeJson(restored,serialized))return 1;
 auto card=restored["cards"][0];auto state=restored["items"]["rule.switch"];
 if(card["id"]!="temperature"||card["sourceRef"]!="sensor.temperature"||
    card["controlRef"]!="rule.switch"||card["value"]!="23.5"||card["unit"]!="C"||
    card["error"]!="sample error"||card["historyError"]!="history error")return 2;
 if(state["cardId"]!="temperature"||state["value"]!="23.5"||state["unit"]!="C"||
    state["condition"]!="sunny"||state["city"]!="Shanghai"||
    state["series"][0]["sourceRef"]!="system.uptime"||
    state["series"][0]["label"]!="uptime"||state["series"][0]["unit"]!="s"||
    state["series"][0]["value"]!="42")return 3;
}
'''.replace('TYPES', types).replace('PRODUCTION', production)
        json = ROOT / '.pio/libdeps/esp32s3-F16R8/ArduinoJson/src'
        self.assertTrue(json.exists())
        self.compile_run(source, [json, ROOT / 'include'],
                         extra_sources=[ROOT / 'src/network/FixedJsonField.cpp'])
        # The positive test above still serializes after actual destruction.
        # The mutant deliberately borrows card.id; freeing that pointer before
        # serialization invokes UB (on Windows it can hang in a crash dialog).
        # Poison but retain live snapshot storage for this negative case only.
        # The serialized wrong ID must produce the exact normal return code 2.
        retained = r'''
#include <vector>
std::vector<std::unique_ptr<ScreenPanelSnapshot>> retiredSnapshots;
struct RetainPoisonedSnapshot {
 void operator()(ScreenPanelSnapshot* snapshot) const {
    std::memset(static_cast<void*>(snapshot->cards.data()), 0, sizeof(snapshot->cards));
    retiredSnapshots.emplace_back(snapshot);
 }
};
'''
        mutation = source.replace('const char* qualityName', retained + '\nconst char* qualityName')
        original_owner = 'std::unique_ptr<ScreenPanelSnapshot> snapshot('
        self.assertEqual(mutation.count(original_owner), 1)
        self.assertGreater(mutation.count('JsonString(card.id)'), 0)
        mutation = mutation.replace(original_owner,
            'std::unique_ptr<ScreenPanelSnapshot, RetainPoisonedSnapshot> snapshot(')
        mutation = mutation.replace('JsonString(card.id)', 'card.id')
        self.compile_run(mutation, [json, ROOT / 'include'], expected=2,
                         extra_sources=[ROOT / 'src/network/FixedJsonField.cpp'])

    def test_system_sources_use_live_values_for_cards_and_series(self):
        manager = (ROOT / 'src/core/ScreenPanelManager.cpp').read_text(encoding='utf-8')
        production = 'template <typename Snapshot> bool fillPanelSystemSource(const String& sourceRef, Snapshot& target) ' + block(
            manager, 'bool fillPanelSystemSource(const String& sourceRef, Snapshot& target)')
        source = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <cassert>
struct String:std::string{using std::string::string;bool startsWith(const char* s)const{return rfind(s,0)==0;}};
enum class ScreenValueQuality {UNKNOWN,GOOD,OFFLINE};
enum class NetworkStatus {CONNECTED,DISCONNECTED,CONNECTING,AP_MODE,CONNECTION_FAILED};
enum {MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,WL_CONNECTED=3};
int64_t esp_timer_get_time(){return 5000000000000LL;}
uint32_t millis(){return 123456;}
size_t heap_caps_get_free_size(int caps){assert(caps==3);return 65536;}
struct Wifi {int state=3;int status(){return state;}int RSSI(){return -52;}} WiFi;
struct Info {NetworkStatus status=NetworkStatus::CONNECTED;};
struct FBNetworkManager {Info info;Info getStatusInfo(){return info;}} network;
struct FastBeeFramework {
 static FastBeeFramework* getInstance(){static FastBeeFramework instance;return &instance;}
 FBNetworkManager* getNetworkManager(){return &network;}
};
template<size_t N> void copyText(char(&out)[N],const String& value){std::snprintf(out,N,"%s",value.c_str());}
struct Card {char value[65]{};char unit[17]{};ScreenValueQuality quality{};uint32_t updatedMs=0;};
struct Series {char value[33]{};char unit[17]{};ScreenValueQuality quality{};uint32_t updatedMs=0;};
PRODUCTION
int main(){
 Card c;assert(fillPanelSystemSource("system.uptime",c));
 assert(std::string(c.value)=="5000000"&&std::string(c.unit)=="s"&&c.updatedMs==123456);
 c={};assert(fillPanelSystemSource("system.freeHeap",c));assert(std::string(c.value)=="65536");
 Series s;assert(fillPanelSystemSource("system.wifiRssi",s));assert(std::string(s.value)=="-52");
 WiFi.state=0;assert(fillPanelSystemSource("system.wifiRssi",s));
 assert(s.value[0]==0&&s.quality==ScreenValueQuality::OFFLINE);
 for(auto state:{NetworkStatus::CONNECTED,NetworkStatus::DISCONNECTED,NetworkStatus::CONNECTING,
                 NetworkStatus::AP_MODE,NetworkStatus::CONNECTION_FAILED}){
   network.info.status=state;assert(fillPanelSystemSource("system.network",c));
   assert(c.quality==ScreenValueQuality::GOOD&&c.value[0]);
 }
 assert(std::string(c.value)=="failed");
 assert(!fillPanelSystemSource("system.missing",c));assert(!fillPanelSystemSource("remote.test",s));
}
'''.replace('PRODUCTION', production)
        self.compile_run(source)
        self.assertIn('bool populated = fillPanelSystemSource(sourceRef, cardOut);', manager)
        self.assertIn('bool seriesPopulated = fillPanelSystemSource(seriesRef, seriesOut);', manager)

    def test_route_psram_scope_restores_policy_and_skips_unavailable_psram(self):
        header = (ROOT / 'include/utils/RoutePsramScope.h').read_text(encoding='utf-8')
        production = header[header.index('namespace FastBee'):]
        source = r'''
#include <cassert>
#include <cstddef>
#include <vector>
enum {MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2};
size_t freeBytes=1024*1024;std::vector<size_t> thresholds;
size_t heap_caps_get_free_size(int){return freeBytes;}
void heap_caps_malloc_extmem_enable(size_t n){thresholds.push_back(n);}
PRODUCTION
void earlyReturn(){FastBee::RoutePsramScope scope;return;}
int main(){
 earlyReturn();
#if FASTBEE_USE_PSRAM
 assert((thresholds==std::vector<size_t>{0,256}));
#else
 assert(thresholds.empty());
#endif
 thresholds.clear();freeBytes=65535;earlyReturn();assert(thresholds.empty());
}
'''.replace('PRODUCTION', production)
        self.compile_run('#define FASTBEE_USE_PSRAM 1\n' + source)
        self.compile_run('#define FASTBEE_USE_PSRAM 0\n' + source)
        web = (ROOT / 'src/network/WebConfigManager.cpp').read_text(encoding='utf-8')
        init = block(web, 'bool WebConfigManager::initialize()')
        self.assertLess(init.index('RoutePsramScope'), init.index('new StaticRouteHandler'))
        self.assertLess(init.index('RoutePsramScope'), init.index('setupAllRoutes()'))


if __name__ == '__main__':
    unittest.main()
