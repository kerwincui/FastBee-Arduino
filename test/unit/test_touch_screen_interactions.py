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
#include <algorithm>
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
 c.touchBuzzerDurationMs=150;touchFeedback(100);assert(device.d.calls==1);
 buzzerNeedsOff=false;touchFeedback(100);assert(device.d.calls==2&&buzzerOffAtMs==250);
 buzzerNeedsOff=false;
 s.touchBeepEnabled=false;touchFeedback(300);assert(device.d.calls==2);
 s.touchBeepEnabled=true;device.d.caps.buzzer=false;touchFeedback(400);assert(device.d.calls==2);
 device.d.caps.buzzer=true;device.d.ok=false;touchFeedback(500);assert(device.d.calls==3&&buzzerNeedsOff&&buzzerOffAtMs==500);
 touchFeedback(900);assert(device.d.calls==3);
 buzzerNeedsOff=false;device.d.ok=true;c.touchBuzzerDurationMs=0;touchFeedback(1000);assert(buzzerOffAtMs==1020);
 buzzerNeedsOff=false;c.touchBuzzerDurationMs=255;touchFeedback(1200);assert(buzzerOffAtMs==1350);
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
constexpr uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return ((r&0xf8U)<<8U)|((g&0xfcU)<<3U)|(b>>3U);}
uint16_t blend565(uint16_t color,uint16_t,uint8_t){return color;}
struct StripeCanvas{std::vector<std::string> labels;void text(int,int,const char* s,int,int,int){labels.push_back(s);}void roundedRect(int,int,int,int,int,int,int){}};
struct Renderer{bool controlPressed(const ScreenPanelCardSnapshot&,const PixelRect&)const{return false;} DRAW};
int main(){Renderer r;StripeCanvas c;ScreenPanelCardSnapshot s;s.motorKnown=true;s.motorSpeed=500;s.motorSpeedInput=501;s.motorPulse=1600;s.motorPrecision=1;
r.drawMotor(c,s,{0,0,448,400},{});bool speed=false,pulse=false,input=false;for(const auto& l:c.labels){speed|=l=="读回 50.0";pulse|=l=="读回 1600";input|=l=="50.1";}assert(speed&&pulse&&input);
auto has=[&](const char* text){for(const auto& label:c.labels)if(label.find(text)!=std::string::npos)return true;return false;};
s.motorCommandKnown=true;s.motorCommandSuccess=true;s.motorKnown=false;strcpy(s.value,"stop");
c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});
assert(has("停止已应答，运动状态未知")&&has("参数未读回")&&!has("已停止"));
s.motorCommandSuccess=false;strcpy(s.value,"reverse");c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});
assert(has("反转未确认")&&!has("反转已应答"));
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
  assert(left.x+left.w<stop.x&&stop.x+stop.w<right.x);
  assert(left.y==stop.y&&stop.y==right.y);
  assert(left.h==64&&stop.h==64&&right.h==64);
  const auto parameter=m.parameter(1);assert(parameter.y+parameter.h+10<=stop.y);
  assert(stop.y+stop.h<=bounds.y+bounds.h-18);
 }
 assert(!ScreenMotorLayout({0,0,239,280}).valid());assert(!ScreenMotorLayout({0,0,240,279}).valid());
}
''', [ROOT / 'include'])

    def test_wake_touch_is_consumed_and_slider_drag_saves_once(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        touch = runtime[runtime.rindex('ITouchBackend* touch = device.touch();'):]
        chain = touch[touch.index('if (sleeping) {'):touch.index('\n        }\n\n        const bool requestedRefresh')]
        # Include the production post-poll invalidation, so consumed refresh
        # requests and a saved layout during a held press behave as on device.
        refresh = touch[touch.index('const bool requestedRefresh'):]
        chain += '\n' + refresh[:refresh.index('#if FASTBEE_SCREEN_WORKER_ISOLATION')]
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
struct Snapshot{std::array<Card,1> cards;unsigned revision=1;uint8_t brightnessPercent=70,activePageIndex=0;};
struct TouchPoint{uint16_t x,y;bool pressed;};
GESTURE
struct Display{struct Caps{bool brightnessContinuous=true;}caps;int calls=0,sleepCalls=0;bool brightnessOk=true,wakeOk=true;uint8_t brightness=70;Caps capabilities(){return caps;}bool setBrightness(uint8_t v){++calls;if(!brightnessOk)return false;brightness=v;return true;}bool setSleep(bool){++sleepCalls;return wakeOk;}};
struct Device{Display d;Display* display(){return &d;}};
struct Renderer{struct Rect{int x=0,y=0,w=240,h=100;};String pressed;int hit=0,invalidations=0;void invalidate(){++invalidations;}bool feedbackAllowed=true,actionAllowed=true;Rect cardBounds(Snapshot&,Card&){return {};}int hitTest(Snapshot&,int x,int y){return x>=20&&x<221&&y>=50&&y<100?hit:-1;}int tapTarget(Snapshot& s,int x,int y,const char*){return hitTest(s,x,y)<0?0:(s.cards[0].visualType==ScreenVisualType::SLIDER?1:(x<120?1:2));}int width(){return 480;}int bufferBytes(){return 100;}void closeNavigation(){} void setPressed(const char* id,int=0,int=0){pressed=id?id:"";}bool feedbackHitTest(Snapshot&,int,int){return feedbackAllowed;}template<class S,class C>bool actionHitTest(const S&,const C&,int,int){return actionAllowed;}};
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
assert(h.renderer.invalidations==1&&h.forceRender);
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
label.process({100,20,true},2000);assert(label.gesture.slider<0&&label.renderer.pressed.isEmpty());
label.process({100,20,false},2010);assert(label.device.d.calls==calls&&m.writes==writes&&label.renderer.pressed.isEmpty());
// The production gesture chain must capture the original control and cancel
// permanently on excursions, even if the pointer later returns to it.
Harness capture;capture.s.cards[0].visualType=ScreenVisualType::ACTION;
int before=taps;
capture.process({100,45,true},3000);capture.process({100,55,true},3010);capture.process({100,55,false},3020);assert(taps==before);
capture.process({115,80,true},3100);capture.process({125,80,true},3110);capture.process({125,80,false},3120);assert(taps==before);
capture.process({115,80,true},3200);capture.process({125,80,true},3210);capture.process({115,80,true},3220);capture.process({115,80,false},3230);assert(taps==before);
capture.process({100,80,true},3300);capture.process({100,80,true},3310);capture.process({100,80,false},3320);capture.process({100,80,false},3330);assert(taps==before+1);
before=taps;capture.process({100,80,true},3400);++capture.s.revision;capture.process({100,80,false},3410);assert(taps==before);
capture.process({100,80,true},3500);++capture.s.activePageIndex;capture.process({100,80,false},3510);assert(taps==before);
capture.process({100,80,true},3600);capture.state.refreshRequested=true;capture.process({100,80,false},3610);assert(taps==before);
capture.process({100,80,true},3700);capture.state.refreshRequested=true;capture.process({100,80,true},3710);
assert(!capture.gesture.down && capture.gesture.suppressUntilRelease);
capture.process({100,80,false},3720);assert(taps==before);
// A save on the release frame must not navigate using an obsolete layout.
int pageCount=pages;
capture.process({210,80,true},3730);capture.process({30,80,true},3740);
capture.state.refreshRequested=true;capture.process({30,80,false},3750);assert(pages==pageCount);
capture.process({210,80,true},3760);capture.process({30,80,true},3770);
capture.process({30,80,false},3780);assert(pages==pageCount+1);
// Even inside one large button, a drag that returns to its origin must not
// become a tap. Small hand jitter remains a single deliberate activation.
capture.process({80,70,true},3800);capture.process({100,70,true},3810);capture.process({80,70,true},3820);capture.process({80,70,false},3830);assert(taps==before);
capture.process({80,70,true},3900);capture.process({80,86,true},3910);capture.process({80,70,true},3920);capture.process({80,70,false},3930);assert(taps==before);
capture.process({80,70,true},4000);capture.process({94,84,true},4010);capture.process({94,84,false},4020);assert(taps==before+1);
// A prolonged hold never dispatches on release, including across uptime wrap.
before=taps;capture.process({80,70,true},4100);capture.process({80,70,false},5001);assert(taps==before);
capture.process({80,70,true},0xffffff00U);capture.process({80,70,false},700U);assert(taps==before);
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
 static PixelRect navigationButton(const PixelRect& r){return r;}
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
 ScreenMotorLayout layout{r.fixed};
 for(const auto& button:{layout.direction(0),layout.stop(),layout.direction(1)})
  assert(r.actionHitTest(snapshot,card,button.x+button.w/2,button.y+button.h/2));
 const auto forward=layout.direction(0);
 assert(!r.actionHitTest(snapshot,card,forward.x+forward.w+5,forward.y+20));
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
