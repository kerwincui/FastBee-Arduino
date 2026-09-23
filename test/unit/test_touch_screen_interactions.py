"""Run the production touch and deferred settings paths with deterministic I/O."""
from pathlib import Path
import unittest
from test.unit import test_panel_runtime_regressions as helpers

block = helpers.block

ROOT = Path(__file__).resolve().parents[2]


class TouchScreenInteractionTests(unittest.TestCase):
    compile_run = helpers.PanelRuntimeRegressionTests.compile_run
    def test_touch_beep_respects_live_toggle_duration_and_failed_output(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        production = 'const auto touchFeedback = [&](uint32_t now)' + block(runtime, 'const auto touchFeedback =') + ';'
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
struct Snapshot{bool touchBeepEnabled=false;};
struct Config{uint8_t touchBuzzerDurationMs=30;};
struct Display{struct Caps{bool buzzer=true;}caps;int calls=0;bool ok=true;Caps capabilities(){return caps;}bool setBuzzer(bool on){assert(on);++calls;return ok;}};
struct Device{Display d;Display* display(){return &d;}};
int main(){
 Device device;Snapshot s;Snapshot* snapshot=&s;Config c;Config* config=&c;
 bool touchBeepStarted=false,buzzerNeedsOff=false;uint32_t lastTouchBeepMs=0,buzzerOffAtMs=0;
 PRODUCTION
 touchFeedback(0);assert(device.d.calls==0&&!buzzerNeedsOff);
 s.touchBeepEnabled=true;touchFeedback(0);assert(device.d.calls==1&&buzzerNeedsOff&&buzzerOffAtMs==30);
 touchFeedback(99);assert(device.d.calls==1);
 c.touchBuzzerDurationMs=150;touchFeedback(100);assert(device.d.calls==2&&buzzerOffAtMs==250);
 s.touchBeepEnabled=false;touchFeedback(300);assert(device.d.calls==2);
 s.touchBeepEnabled=true;device.d.caps.buzzer=false;touchFeedback(400);assert(device.d.calls==2);
 device.d.caps.buzzer=true;device.d.ok=false;touchFeedback(500);assert(device.d.calls==3&&buzzerNeedsOff&&buzzerOffAtMs==500);
}
'''.replace('PRODUCTION', production))

    def test_motor_text_uses_device_speed_precision(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        draw = 'void drawMotor(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card, const PixelRect& rect, const Palette& palette) const' + block(runtime, 'void drawMotor(')
        self.compile_run(r'''
#include <Arduino.h>
#include <array>
#include <cassert>
#include <cmath>
#include "screen/ScreenInteraction.h"
using namespace FastBee::Screen;
TYPES
using PixelRect=ScreenPixelRect;struct Palette{int muted=0,text=0,backgroundAlt=0,cardBorder=0,warning=0,accent=0,card=0;};
uint16_t blend565(uint16_t color,uint16_t,uint8_t){return color;}
struct StripeCanvas{std::vector<std::string> labels;void text(int,int,const char* s,int,int,int){labels.push_back(s);}void roundedRect(int,int,int,int,int,int,int){}};
struct Renderer{DRAW};
int main(){Renderer r;StripeCanvas c;ScreenPanelCardSnapshot s;s.motorKnown=true;s.motorSpeed=500;s.motorSpeedInput=501;s.motorPulse=1600;s.motorPrecision=1;
r.drawMotor(c,s,{0,0,448,400},{});bool speed=false,pulse=false,input=false;for(const auto& l:c.labels){speed|=l=="读回 50.0";pulse|=l=="读回 1600";input|=l=="50.1";}assert(speed&&pulse&&input);
}
'''.replace('TYPES', types).replace('DRAW', draw), [ROOT / 'test/mocks', ROOT / 'include'])

    def test_control_session_revision_binding_inactivity_and_layout(self):
        self.compile_run(r'''
#include <cassert>
#include <cstring>
#include "screen/ScreenInteraction.h"
using namespace FastBee::Screen;
int main(){
 ScreenControlSession s;s.observe(1,1000,30);assert(!s.allows("motor.a"));
 s.approve("motor.a",1000);assert(s.allows("motor.a")&&!s.allows("motor.b"));
 s.used("motor.b",29999);s.observe(1,30999,30);assert(s.allows("motor.a"));
 s.observe(1,31000,30);assert(!s.allows("motor.a"));
 s.approve("motor.a",40000);s.used("motor.a",69999);s.observe(1,70000,30);assert(s.allows("motor.a"));
 s.observe(2,70001,30);assert(!s.allows("motor.a"));
 s.approve("motor.a",70002);s.reset();assert(!s.allows("motor.a"));
 s.approve("",1);s.approve(nullptr,1);assert(!s.allows(""));
 char tooLong[98];memset(tooLong,'a',97);tooLong[97]=0;s.approve(tooLong,1);assert(!s.allows(tooLong));
 s.approve("motor.a",0xfffffff0U);s.observe(2,100,1);assert(s.allows("motor.a"));
 s.observe(2,1000,1);assert(!s.allows("motor.a"));
 for(auto bounds:{ScreenPixelRect{0,0,240,280},ScreenPixelRect{16,55,448,400}}){
  ScreenMotorLayout m{bounds};assert(m.valid());
  for(unsigned row=0;row<2;++row)for(unsigned col=0;col<3;++col){auto r=m.adjust(row,col);
   assert(r.x>=bounds.x&&r.y>=bounds.y&&r.x+r.w<=bounds.x+bounds.w&&r.y+r.h<=bounds.y+bounds.h);
   assert(screenRectContains(r,r.x+r.w/2,r.y+r.h/2));assert(!screenRectContains(r,r.x+r.w,r.y));}
  auto left=m.direction(0),right=m.direction(1),stop=m.stop();
  assert(left.x+left.w<right.x&&left.y+left.h<stop.y);
 }
 assert(!ScreenMotorLayout({0,0,239,280}).valid());assert(!ScreenMotorLayout({0,0,240,279}).valid());
}
''', [ROOT / 'include'])

    def test_wake_touch_is_consumed_and_slider_drag_saves_once(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        touch = runtime[runtime.rindex('ITouchBackend* touch = device.touch();'):]
        chain = touch[touch.index('if (sleeping) {'):touch.index('\n        }\n\n        const bool requestedRefresh')]
        gesture = 'struct TouchGesture' + block(runtime, 'struct TouchGesture') + ';'
        self.compile_run(r'''
#include <Arduino.h>
#include <array>
#include <atomic>
#include <cassert>
#include "screen/ScreenInteraction.h"
using namespace FastBee::Screen;
size_t strlcpy(char* to,const char* from,size_t n){if(n)snprintf(to,n,"%s",from);return strlen(from);}
constexpr int MIN_SWIPE_PX=40,TAP_MOVE_PX=14,TAP_MAX_MS=900;
enum class ScreenVisualType{SLIDER,ACTION};
struct Card {ScreenVisualType visualType=ScreenVisualType::SLIDER;int numericValue=70;char id[41]="brightness",error[97]{};};
struct Snapshot{std::array<Card,1> cards;unsigned revision=1;uint8_t brightnessPercent=70;};
struct TouchPoint{uint16_t x,y;bool pressed;};
GESTURE
struct Display{struct Caps{bool brightnessContinuous=true;}caps;int calls=0,sleepCalls=0;bool brightnessOk=true,wakeOk=true;uint8_t brightness=70;Caps capabilities(){return caps;}bool setBrightness(uint8_t v){++calls;if(!brightnessOk)return false;brightness=v;return true;}bool setSleep(bool){++sleepCalls;return wakeOk;}};
struct Device{Display d;Display* display(){return &d;}};
struct Renderer{struct Rect{int x=0,y=0,w=240,h=100;};String pressed;int hit=0;bool feedbackAllowed=true,actionAllowed=true;Rect cardBounds(Snapshot&,Card&){return {};}int hitTest(Snapshot&,int,int){return hit;}int width(){return 480;}int bufferBytes(){return 100;}void setPressed(const char* id){pressed=id?id:"";}bool feedbackHitTest(Snapshot&,int,int){return feedbackAllowed;}template<class S,class C>bool actionHitTest(const S&,const C&,int,int){return actionAllowed;}};
int taps=0,pages=0;
void handleTap(Renderer&,Snapshot&,int,int,char*,size_t,ScreenControlSession*,unsigned){++taps;}
void changePage(Snapshot&,int){++pages;}
struct ScreenPanelManager{int writes=0;unsigned value=0;bool fail=false;static ScreenPanelManager& instance(){static ScreenPanelManager m;return m;}bool queueSetting(const String& k,unsigned v,String& e){assert(k=="brightnessPercent");++writes;value=v;e="storage unavailable";return !fail;}void reportApplication(bool,bool,const String&){};};
struct Harness{
Device device;Renderer renderer;Snapshot s;Snapshot* snapshot=&s;TouchGesture gesture;
ScreenControlSession controlSession;int updates=0,feedback=0;
struct State{std::atomic<bool> refreshRequested{false};}state;
bool sleeping=false,brightnessOverridden=false,forceRender=false;uint8_t appliedBrightness=70;
unsigned brightnessOverrideRevision=0;char confirmationCardId[41]{};size_t snapshotBytes=100,framebufferBytes=100;
void updateRuntimeState(bool,bool,size_t){++updates;}
void touchFeedback(unsigned){++feedback;}
void process(TouchPoint point,unsigned now){CHAIN}
};
int main(){auto& m=ScreenPanelManager::instance();Harness h;h.sleeping=true;h.s.cards[0].visualType=ScreenVisualType::ACTION;
h.controlSession.approve("motor.a",0);strcpy(h.confirmationCardId,"motor");
h.device.d.brightnessOk=false;h.process({100,80,true},0);assert(h.sleeping&&h.updates==0&&h.device.d.sleepCalls==0&&h.controlSession.allows("motor.a"));
h.device.d.brightnessOk=true;h.device.d.wakeOk=false;h.process({100,80,true},0);assert(h.sleeping&&h.updates==0&&!h.gesture.suppressUntilRelease);
int attempts=h.device.d.calls;h.process({100,80,false},0);assert(h.sleeping&&h.device.d.calls==attempts);
h.device.d.wakeOk=true;h.brightnessOverridden=true;h.appliedBrightness=35;
h.process({100,80,true},0);assert(!h.sleeping&&h.gesture.suppressUntilRelease);
assert(h.device.d.brightness==35&&h.updates==1&&!h.controlSession.allows("motor.a")&&!h.confirmationCardId[0]&&h.feedback==0);
h.process({100,80,true},100);h.process({100,80,false},120);assert(taps==0&&pages==0&&m.writes==0);
h.process({100,80,true},200);h.process({100,80,false},220);assert(taps==1);
int feedbackBefore=h.feedback,tapsBefore=taps;
h.process({160,80,true},300);h.process({60,82,true},320);h.process({60,82,false},340);
assert(pages==1&&taps==tapsBefore&&h.feedback==feedbackBefore);
h.s.cards[0].visualType=ScreenVisualType::SLIDER;int calls=h.device.d.calls;
h.process({20,80,true},1000);for(unsigned t=1010;t<1100;t+=10)h.process({100,80,true},t);
assert(h.device.d.calls==calls+1&&m.writes==0);h.process({210,80,true},1100);h.process({220,80,true},1120);h.process({220,80,false},1130);
assert(h.device.d.brightness==100&&m.writes==1&&m.value==100&&pages==1&&taps==1);
assert(h.renderer.pressed.isEmpty());
h.process({20,80,true},1140);h.process({20,80,false},1150);assert(h.device.d.brightness==0&&m.value==0&&m.writes==2);
m.fail=true;h.process({120,80,true},1200);h.process({120,80,false},1210);assert(String(h.s.cards[0].error)=="storage unavailable");
// Slider labels are not drag handles; a small label tap performs no brightness save.
Harness label;calls=label.device.d.calls;int writes=m.writes;
label.process({100,20,true},2000);assert(label.gesture.slider<0&&label.renderer.pressed=="brightness");
label.process({100,20,false},2010);assert(label.device.d.calls==calls&&m.writes==writes&&label.renderer.pressed.isEmpty());
}
'''.replace('GESTURE', gesture).replace('CHAIN', chain), [ROOT / 'test/mocks', ROOT / 'include'])


    def test_card_blank_areas_and_read_only_cards_are_not_action_targets(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        action = 'bool actionHitTest(const ScreenPanelSnapshot& snapshot, const ScreenPanelCardSnapshot& card, uint16_t x, uint16_t y) const' + block(runtime, 'bool actionHitTest(')
        self.compile_run(r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include "screen/ScreenInteraction.h"
using PixelRect=FastBee::Screen::ScreenPixelRect;
using FastBee::Screen::screenRectContains;
using FastBee::Screen::screenSelectButton;
using FastBee::Screen::ScreenMotorLayout;
__TYPES__
enum class NumericControlHit : uint8_t { NONE = 0, DECREMENT, INCREMENT, EXECUTE };
struct Renderer{
 int _width=480,_height=480; PixelRect fixed{16,16,448,208};
 PixelRect cardBounds(const ScreenPanelSnapshot&, const ScreenPanelCardSnapshot&) const { return fixed; }
 NumericControlHit numericControlHitTest(const ScreenPanelSnapshot&, const ScreenPanelCardSnapshot&, uint16_t, uint16_t) const { return NumericControlHit::NONE; }
 __ACTION_METHOD__
};
int main(){
 ScreenPanelSnapshot snapshot; ScreenPanelCardSnapshot card; Renderer r;
 card.kind=ScreenCardKind::DATA; card.visualType=ScreenVisualType::VALUE; assert(!r.actionHitTest(snapshot,card,120,80));
 card.kind=ScreenCardKind::CHART; card.visualType=ScreenVisualType::LINE; assert(!r.actionHitTest(snapshot,card,120,120));
 card.kind=ScreenCardKind::CONTROL; card.visualType=ScreenVisualType::ACTION;
 assert(!r.actionHitTest(snapshot,card,120,40));
 assert(r.actionHitTest(snapshot,card,120,124));
 card.visualType=ScreenVisualType::SWITCH;
 assert(!r.actionHitTest(snapshot,card,60,120));
 assert(r.actionHitTest(snapshot,card,420,116));
 r.fixed={16,16,448,448}; card.visualType=ScreenVisualType::MOTOR_CONTROL;
 assert(!r.actionHitTest(snapshot,card,240,72));
 assert(r.actionHitTest(snapshot,card,128,360));
 assert(r.actionHitTest(snapshot,card,240,410));
}
'''.replace('__TYPES__', types).replace('__ACTION_METHOD__', action), [ROOT / 'test/mocks', ROOT / 'include'])

    def test_trend_motor_and_timeout_taps(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        # Charts are read-only and every control card delegates to an explicit
        # hit region; this prevents outer-card taps from dispatching actions.
        self.assertIn('card.visualType == ScreenVisualType::LINE', runtime)
        self.assertIn('renderer.actionHitTest(snapshot, card, x, y)', runtime)
        self.assertIn('screenRectContains(layout.direction(0), x, y)', runtime)
        self.assertIn('screenRectContains(layout.stop(), x, y)', runtime)
        self.assertIn('if (pageTab >= 0)', runtime)

    def test_settings_coalesce_conflicts_failures_and_worker_restart(self):
        manager = (ROOT / 'src/core/ScreenPanelManager.cpp').read_text(encoding='utf-8')
        production = 'bool ScreenPanelManager::queueSetting(const String& key, uint32_t value, String& error)' + block(manager, 'bool ScreenPanelManager::queueSetting(')
        json = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h')).parent
        self.compile_run(r'''
#include <ArduinoJson.h>
#include <Arduino.h>
#include <cassert>
#include <functional>
#define ARDUINO_ARCH_ESP32 1
using SemaphoreHandle_t=void*;using TaskHandle_t=void*;
constexpr int pdTRUE=1,pdPASS=1,portMAX_DELAY=0;
unsigned now=0;bool failTask=false;void(*worker)(void*)=nullptr;void* workerArg=nullptr;
std::function<void()> duringDelay;
unsigned testMillis(){return now;}
#define millis testMillis
#define pdMS_TO_TICKS(x) (x)
void* xSemaphoreCreateMutex(){return reinterpret_cast<void*>(1);}
int xSemaphoreTake(void*,int){return 1;}void xSemaphoreGive(void*){}
void vTaskDelay(unsigned ms){now+=ms;if(duringDelay){auto f=duringDelay;duringDelay=nullptr;f();}}
void vTaskDelete(void*){}
int xTaskCreate(void(*f)(void*),const char*,int,void* a,int,void** h){if(failTask)return 0;worker=f;workerArg=a;*h=reinterpret_cast<void*>(1);return 1;}
class ScreenPanelManager {
public:
enum class SaveResult{SAVED,STORAGE_ERROR};
unsigned rev=1,saves=0,errors=0;bool failSave=false;JsonDocument config;
std::function<void()> duringSave;
static ScreenPanelManager& instance(){static ScreenPanelManager m;return m;}
unsigned revision(){return rev;}
bool copyConfig(JsonDocument& d,uint32_t& r,bool& p){d.set(config);r=rev;p=true;return true;}
SaveResult saveConfig(JsonVariantConst d,unsigned expected,String& e){assert(expected==rev);++saves;if(duringSave){auto f=duringSave;duringSave=nullptr;f();}if(failSave){e="disk failed";return SaveResult::STORAGE_ERROR;}config.set(d);++rev;return SaveResult::SAVED;}
void reportApplication(bool,bool,const String&){++errors;}
bool queueSetting(const String&,uint32_t,String&);
};
PRODUCTION
int main(){auto& m=ScreenPanelManager::instance();String error;
 assert(!m.queueSetting("bad",10,error));assert(!m.queueSetting("brightnessPercent",101,error));
 assert(m.queueSetting("brightnessPercent",20,error));assert(m.queueSetting("brightnessPercent",75,error));assert(m.queueSetting("idleSleepSeconds",300,error));
 worker(workerArg);assert(m.saves==1&&now>=2000&&m.config["settings"]["brightnessPercent"]==75&&m.config["settings"]["idleSleepSeconds"]==300);
 assert(m.queueSetting("brightnessPercent",40,error));++m.rev;worker(workerArg);assert(m.saves==1&&m.errors==1&&m.config["settings"]["brightnessPercent"]==75);
 failTask=true;assert(!m.queueSetting("brightnessPercent",10,error));failTask=false;
 assert(m.queueSetting("idleSleepSeconds",60,error));worker(workerArg);assert(m.config["settings"]["brightnessPercent"]==75);
 m.failSave=true;assert(m.queueSetting("brightnessPercent",25,error));worker(workerArg);assert(m.errors==2&&m.config["settings"]["brightnessPercent"]==75);m.failSave=false;
 assert(m.queueSetting("brightnessPercent",50,error));m.duringSave=[&]{assert(m.queueSetting("idleSleepSeconds",120,error));};worker(workerArg);
 assert(m.config["settings"]["brightnessPercent"]==50&&m.config["settings"]["idleSleepSeconds"]==120);
 assert(m.queueSetting("brightnessPercent",0,error));assert(m.queueSetting("touchBeepEnabled",1,error));
 assert(!m.queueSetting("controlConfirmIdleSeconds",14,error));assert(!m.queueSetting("controlConfirmIdleSeconds",601,error));
 assert(m.queueSetting("controlConfirmIdleSeconds",30,error));worker(workerArg);
 assert(m.config["settings"]["brightnessPercent"]==0&&m.config["settings"]["touchBeepEnabled"]==true&&m.config["settings"]["controlConfirmIdleSeconds"]==30);
}
'''.replace('PRODUCTION', production), [ROOT / 'test/mocks', json])
