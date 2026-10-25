"""Run the production touch and deferred settings paths with deterministic I/O."""
from pathlib import Path
import unittest
from test.unit import test_panel_runtime_regressions as helpers

block = helpers.block

ROOT = Path(__file__).resolve().parents[2]


class TouchScreenInteractionTests(unittest.TestCase):
    compile_run = helpers.PanelRuntimeRegressionTests.compile_run
    def test_pending_motor_refreshes_promptly_without_interrupting_a_held_touch(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        constants = runtime[runtime.index('constexpr uint32_t SNAPSHOT_INTERVAL_MS'):runtime.index('constexpr uint32_t START_TIMEOUT_MS')]
        production = 'uint32_t snapshotIntervalMs(const ScreenPanelSnapshot& snapshot)' + block(runtime, 'uint32_t snapshotIntervalMs(')
        start = runtime.index('if (forceRender || (!gesture.down && (requestedRefresh')
        condition = runtime[start:runtime.index('{', start)]
        self.compile_run(r'''
#include <Arduino.h>
#include <array>
#include <cassert>
#include <cstring>
TYPES
CONSTANTS
PRODUCTION
bool refresh(const ScreenPanelSnapshot& s, uint32_t now, uint32_t lastSnapshotMs,
             bool held=false, bool requestedRefresh=false, bool forceRender=false) {
 const auto* snapshot=&s;struct {bool down;} gesture{held};
 CONDITION {return true;} return false;
}
int main(){
 ScreenPanelSnapshot s;
 assert(!refresh(s,499,0)&&refresh(s,500,0));
 s.pageCount=2;strcpy(s.pages[0].id,"motor");strcpy(s.pages[1].id,"other");
 s.cardCount=2;auto& c=s.cards[0];c.pending=true;c.visualType=ScreenVisualType::MOTOR_CONTROL;
 strcpy(c.pageId,"motor");
 assert(!refresh(s,99,0)&&refresh(s,100,0));
 assert(!refresh(s,500,0,true)&&refresh(s,100,0,true,false,true));
 assert(!refresh(s,100,0,true,true)&&refresh(s,1,0,false,true));
 c.pending=false;assert(!refresh(s,100,0)&&refresh(s,500,0));
 c.pending=true;s.activePageIndex=1;assert(!refresh(s,100,0));
 auto& other=s.cards[1];other.pending=true;strcpy(other.pageId,"other");
 assert(!refresh(s,100,0));other.visualType=ScreenVisualType::MOTOR_CONTROL;assert(refresh(s,100,0));
 s.activePageIndex=2;assert(!refresh(s,100,0));
 s.activePageIndex=0;assert(!refresh(s,83,0xfffffff0U)&&refresh(s,84,0xfffffff0U));
}
'''.replace('TYPES', types).replace('CONSTANTS', constants).replace('PRODUCTION', production).replace('CONDITION', condition), [ROOT / 'test/mocks', ROOT / 'include'])

    def test_motor_readback_sync_survives_command_error_and_preserves_drafts(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        start = runtime.index('if (card.visualType == ScreenVisualType::MOTOR_CONTROL && !card.pending')
        condition = runtime[start:runtime.index('{', start)]
        production = condition + block(runtime[start:], condition)
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
enum class ScreenVisualType { MOTOR_CONTROL, OTHER };
struct Card {
 ScreenVisualType visualType=ScreenVisualType::MOTOR_CONTROL;
 bool pending=false,motorKnown=true,motorSpeedDirty=false,motorPulseDirty=false;
 uint16_t motorSpeed=25,motorPulse=400,motorSpeedInput=10,motorPulseInput=100;
 char error[64]="TRAVEL_CALIBRATION_REQUIRED";
};
void sync(Card& card){PRODUCTION}
int main(){
 Card c;sync(c);assert(c.motorSpeedInput==25&&c.motorPulseInput==400&&c.error[0]);
 c.motorSpeedDirty=true;c.motorSpeedInput=30;c.motorSpeed=35;c.motorPulse=500;
 sync(c);assert(c.motorSpeedInput==30&&c.motorSpeedDirty&&c.motorPulseInput==500);
 c.motorKnown=false;c.motorSpeed=30;c.motorPulse=600;sync(c);
 assert(c.motorSpeedDirty&&c.motorPulseInput==500);
 c.motorKnown=true;c.pending=true;sync(c);assert(c.motorSpeedDirty&&c.motorPulseInput==500);
 c.pending=false;sync(c);assert(!c.motorSpeedDirty&&c.motorPulseInput==600);
 c.motorSpeed=40;sync(c);assert(c.motorSpeedInput==40);
}
'''.replace('PRODUCTION', production))

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

    def test_sleep_time_switch_toggles_keep_awake_and_preserves_failures(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        content = block(runtime, 'bool handleContentTap(')
        production = block(content, 'if (card.kind == ScreenCardKind::SYSTEM && !strcmp(card.systemActionId, "sleep-time") && card.visualType == ScreenVisualType::SWITCH)')
        self.compile_run(r'''
#define FASTBEE_TEST_NEEDS_STRLCPY
#include <Arduino.h>
#include <array>
#include <cassert>
#include <cstring>
#include "screen/ScreenInteraction.h"
using namespace FastBee::Screen;
TYPES
#undef assert
#define assert(expression) do { if (!(expression)) { std::fprintf(stderr,"CHECK line %d: %s\n",__LINE__,#expression); std::exit(1); } } while(false)
struct ScreenPanelManager {
 int writes=0; uint32_t value=999; bool fail=false;
 static ScreenPanelManager& instance(){static ScreenPanelManager manager;return manager;}
 bool queueSetting(const String& key,uint32_t next,String& error){
  assert(key=="idleSleepSeconds");++writes;value=next;
  if(fail){error="save failed";return false;}error="";return true;
 }
};
bool apply(ScreenPanelSnapshot& snapshot,ScreenPanelCardSnapshot& card){PRODUCTION}
int main(){
 auto& manager=ScreenPanelManager::instance();
 ScreenPanelSnapshot snapshot{};snapshot.idleSleepSeconds=30;
 ScreenPanelCardSnapshot card{};card.kind=ScreenCardKind::SYSTEM;
 card.visualType=ScreenVisualType::SWITCH;
 std::strcpy(card.systemActionId,"sleep-time");
 std::strcpy(card.value,"false");card.numericValue=30;std::strcpy(card.error,"old");
 assert(apply(snapshot,card));
 assert(manager.writes==1&&manager.value==0);
 assert(snapshot.idleSleepSeconds==0&&card.numericValue==0);
 assert(!std::strcmp(card.value,"true")&&!card.error[0]);
 assert(apply(snapshot,card));
 assert(manager.writes==2&&manager.value==30);
 assert(snapshot.idleSleepSeconds==30&&card.numericValue==30);
 assert(!std::strcmp(card.value,"false")&&!card.error[0]);
 manager.fail=true;
 assert(apply(snapshot,card));
 assert(manager.writes==3&&manager.value==0);
 assert(snapshot.idleSleepSeconds==30&&card.numericValue==30);
 assert(!std::strcmp(card.value,"false"));
 assert(!std::strcmp(card.error,"save failed"));
}
'''.replace('TYPES', types).replace('PRODUCTION', production), [ROOT / 'test/mocks', ROOT / 'include'])

    def test_motor_text_uses_device_speed_precision(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        renderer = (ROOT / 'src/screen/StripeRenderer.h').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        draw = 'void drawMotor(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card, const PixelRect& rect, const Palette& palette) const' + block(renderer, 'void drawMotor(')
        self.compile_run(r'''
#include <Arduino.h>
#include <array>
#include <cassert>
#include <cmath>
#include "screen/ScreenInteraction.h"
#include "screen/PanelStatusText.h"
using namespace FastBee::Screen;
TYPES
using PixelRect=ScreenPixelRect;struct Palette{int muted=0,text=0,backgroundAlt=0,cardBorder=0,warning=0,accent=0,card=0,success=0,danger=0;};
constexpr uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return ((r&0xf8U)<<8U)|((g&0xfcU)<<3U)|(b>>3U);}
uint16_t blend565(uint16_t color,uint16_t,uint8_t){return color;}
uint16_t textOn565(uint16_t){return 0xffff;}
struct StripeCanvas{std::vector<std::string> labels;void text(int,int,const char* s,int,int,int){labels.push_back(s);}void roundedRect(int,int,int,int,int,int,int){}};
struct Renderer{bool controlPressed(const ScreenPanelCardSnapshot&,const PixelRect&)const{return false;} DRAW};
int main(){Renderer r;StripeCanvas c;ScreenPanelCardSnapshot s;s.motorKnown=true;s.motorSpeed=500;s.motorSpeedInput=501;s.motorPulse=1600;s.motorPrecision=1;
r.drawMotor(c,s,{0,0,448,400},{});bool speed=false,pulse=false,input=false;for(const auto& l:c.labels){speed|=l=="读回 50.0";pulse|=l=="读回 1600";input|=l=="50.1";}assert(speed&&pulse&&input);
auto has=[&](const char* text){for(const auto& label:c.labels)if(label.find(text)!=std::string::npos)return true;return false;};
s.motorCommandKnown=true;s.motorCommandSuccess=true;s.motorKnown=false;strcpy(s.value,"stop");
c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});
assert(has("停止指令已应答")&&has("参考位置未确认")&&!has("已停止"));
s.motorSoftLimitEnabled=s.motorReferenceKnown=true;c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});
assert(has("启动前自动读取参数")&&!has("禁止启动"));
s.motorSoftLimitEnabled=s.motorReferenceKnown=false;
s.motorCommandSuccess=false;strcpy(s.value,"reverse");c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});
assert(has("反转指令失败")&&!has("反转指令已应答"));
s.motorKnown=true;s.motorPulse=1600;s.motorSoftLimitEnabled=false;
c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});
assert(has("参考位置未确认")&&!has("软限位仅按指令估算"));
s.motorSoftLimitEnabled=true;c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});
assert(!has("软限位未启用")&&has("参考位置未确认"));
s.motorReferenceKnown=true;c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});
assert(has("换向时自动先停止，再启动"));
s.motorStationaryConfirmed=true;s.motorForwardReady=true;c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});
assert(has("软限位已启用")&&!has("参考位置未确认"));
strcpy(s.error,"未启动：请先完成行程标定");screenRememberMotorFeedback(s,100);
s.error[0]=0;c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});
assert(has("未启动：请先完成行程标定"));
screenExpireMotorFeedback(s,5099);assert(s.motorUiFeedback[0]);
screenExpireMotorFeedback(s,5100);assert(!s.motorUiFeedback[0]);
c.labels.clear();r.drawMotor(c,s,{0,0,448,400},{});assert(!has("未启动："));
strcpy(s.error,"TEST_WRAP");screenRememberMotorFeedback(s,0xfffffff0U);
screenExpireMotorFeedback(s,100);assert(s.motorUiFeedback[0]);
screenExpireMotorFeedback(s,4984);assert(!s.motorUiFeedback[0]);
}
'''.replace('TYPES', types).replace('DRAW', draw), [ROOT / 'test/mocks', ROOT / 'include'])

    def test_control_session_revision_binding_inactivity_and_layout(self):
        self.compile_run(r'''
#include <cassert>
#include <cstring>
#include "screen/ScreenInteraction.h"
#include "screen/PanelStatusText.h"
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
        chain = touch[touch.index('if (sleeping) {'):touch.index('\n        }\n\n        // Retry a touch wake')]
        retry = touch[touch.index('if (touchWakePending && sleeping &&'):touch.index('const bool requestedRefresh')]
        chain += '\n' + retry
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
#include <cmath>
#include "screen/ScreenInteraction.h"
#include "screen/PanelStatusText.h"
using namespace FastBee::Screen;
#define LOG_INFOF(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)
struct Logger { template<class... Args> void infof(const char*, Args...) {} } LOGGER;
size_t strlcpy(char* to,const char* from,size_t n){if(n)snprintf(to,n,"%s",from);return strlen(from);}
constexpr int MIN_SWIPE_PX=40,TAP_MOVE_PX=14,TAP_MAX_MS=900;
constexpr unsigned REPEAT_START_MS=600,REPEAT_INTERVAL_MS=150,REPEAT_FAST_AFTER_MS=2000;constexpr int REPEAT_FAST_MULTIPLIER=10;
enum class ScreenVisualType{SLIDER,ACTION,MOTOR_CONTROL,NUMBER_CONTROL};
enum class NumericControlHit : uint8_t { NONE = 0, DECREMENT, INCREMENT, EXECUTE };
struct Card {ScreenVisualType visualType=ScreenVisualType::SLIDER;float numericValue=70;char id[41]="brightness",error[97]{};
 bool pending=false,motorSpeedDirty=false,motorPulseDirty=false;uint16_t motorSpeedInput=10,motorPulseInput=100,motorSpeedStep=1,motorPulseStep=100;
 char motorUiFeedback[97]{},controlRef[97]{};float rangeMin=0,rangeMax=100,numericStep=1;uint8_t precision=1;};
struct Snapshot{std::array<Card,1> cards;unsigned revision=1;uint8_t brightnessPercent=70,activePageIndex=0;};
struct TouchPoint{uint16_t x,y;bool pressed;};
GESTURE
struct Display{struct Caps{bool brightnessContinuous=true;}caps;int calls=0,sleepCalls=0;bool brightnessOk=true,wakeOk=true;uint8_t brightness=70;Caps capabilities(){return caps;}bool setBrightness(uint8_t v){++calls;if(!brightnessOk)return false;brightness=v;return true;}bool setSleep(bool){++sleepCalls;return wakeOk;}};
struct Device{Display d;Display* display(){return &d;}};
struct Renderer{using Rect=ScreenPixelRect;String pressed;int hit=0,invalidations=0;void invalidate(){++invalidations;}bool feedbackAllowed=true,actionAllowed=true;Rect cardBounds(Snapshot&,Card&){return Rect{0,0,240,100};}int hitTest(Snapshot&,int x,int y){return x>=20&&x<221&&y>=50&&y<100?hit:-1;}int tapTarget(Snapshot& s,int x,int y,const char*){return hitTest(s,x,y)<0?0:(s.cards[0].visualType==ScreenVisualType::SLIDER?1:(x<120?1:2));}int width(){return 480;}int bufferBytes(){return 100;}void closeNavigation(){} void setPressed(const char* id,int=0,int=0){pressed=id?id:"";}bool feedbackHitTest(Snapshot&,int,int){return feedbackAllowed;}NumericControlHit numericControlHitTest(Snapshot&,Card&,int,int){return NumericControlHit::NONE;}template<class S,class C>bool actionHitTest(const S&,const C&,int,int){return actionAllowed;}};
int taps=0,pages=0;
void handleTap(Renderer&,Snapshot&,int,int,char*,size_t,ScreenControlSession*,unsigned){++taps;}
void changePage(Snapshot&,int){++pages;}
struct ScreenPanelManager{int writes=0;unsigned value=0;bool fail=false;static ScreenPanelManager& instance(){static ScreenPanelManager m;return m;}bool queueSetting(const String& k,unsigned v,String& e){assert(k=="brightnessPercent");++writes;value=v;e="storage unavailable";return !fail;}void reportApplication(bool,bool,const String&){};};
struct Harness{
Device device;Renderer renderer;Snapshot s;Snapshot* snapshot=&s;TouchGesture gesture;
ScreenControlSession controlSession;int updates=0,feedback=0;
struct State{std::atomic<bool> refreshRequested{false};}state;
bool sleeping=false,brightnessOverridden=false,forceRender=false;uint8_t appliedBrightness=70;
bool touchWakePending=false,touchWakeContactDown=false;uint8_t touchWakeAttempts=0;
uint32_t touchWakeNextAttemptMs=0,lastInteractionMs=0;
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
h.process({100,80,true},100);assert(!h.sleeping&&h.gesture.suppressUntilRelease);
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
        renderer = (ROOT / 'src/screen/StripeRenderer.h').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        action = 'bool actionHitTest(const ScreenPanelSnapshot& snapshot, const ScreenPanelCardSnapshot& card, uint16_t x, uint16_t y) const' + block(renderer, 'bool actionHitTest(')
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
 card.kind=ScreenCardKind::CHART; card.visualType=ScreenVisualType::LINE;
 // F6: a trend chart's whole surface selects the nearest plotted sample;
 // display-only cards keep delegating to their explicit control region.
 assert(r.actionHitTest(snapshot,card,120,120));
 card.kind=ScreenCardKind::CONTROL; card.visualType=ScreenVisualType::ACTION;
 assert(!r.actionHitTest(snapshot,card,120,40));
 assert(r.actionHitTest(snapshot,card,120,124));
 card.visualType=ScreenVisualType::SWITCH;
 // F4: the whole card is the toggle hot zone, not only the pill.
 assert(r.actionHitTest(snapshot,card,60,120));
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

    def test_line_chart_tap_writes_nearest_sample_sequence(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        # The LINE branch of handleContentTap is the wiring between the
        # gesture state machine and the renderer's nearest-sample mapping:
        # it must record the tapped sample and consume the tap (no dispatch).
        branch = block(block(runtime, 'bool handleContentTap('),
                             'if (card.visualType == ScreenVisualType::LINE)')
        self.compile_run(r'''
#include <array>
#include <cassert>
#include <cstdint>
#include "screen/ScreenInteraction.h"
using FastBee::Screen::ScreenPixelRect;
TYPES
struct StubRenderer {
    int result;
    int historyTapIndex(const ScreenPanelCardSnapshot&, const ScreenPixelRect&, uint16_t tapX) const {
        (void)tapX;
        return result;
    }
};
bool tapChart(StubRenderer& renderer, ScreenPanelCardSnapshot& card,
              const ScreenPixelRect& bounds, uint16_t x)
BRANCH
int main() {
    ScreenPanelCardSnapshot card{};
    card.visualType = ScreenVisualType::LINE;
    card.historyCount = 10;
    for (int i = 0; i < 10; ++i) card.history[i].sequence = 500 + i;
    StubRenderer renderer{3};
    const ScreenPixelRect bounds{16, 16, 280, 300};
    assert(tapChart(renderer, card, bounds, 200));
    assert(card.selectedHistorySequence == 503);
    renderer.result = 0;
    assert(tapChart(renderer, card, bounds, 100));
    assert(card.selectedHistorySequence == 500);
    // No plottable sample: keep the previous selection, still consume the tap.
    renderer.result = -1;
    assert(tapChart(renderer, card, bounds, 40));
    assert(card.selectedHistorySequence == 500);
}
'''.replace('TYPES', types).replace('BRANCH', branch), [ROOT / 'test/mocks', ROOT / 'include'])

    def test_motor_colors_do_not_bypass_start_guards_or_block_stop(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/ScreenPanelManager.h').read_text(encoding='utf-8')
        types = header[header.index('enum class ScreenCardKind'):header.index('class ScreenPanelManager')]
        content = block(runtime, 'bool handleContentTap(')
        motor = block(content, 'if (card.visualType == ScreenVisualType::MOTOR_CONTROL)')
        confirm = block(block(runtime, 'bool dispatchCardAction('), 'if (card.visualType == ScreenVisualType::MOTOR_CONTROL)')
        self.compile_run(r'''
#include <Arduino.h>
#include <array>
#include <cassert>
#include "screen/ScreenInteraction.h"
using namespace FastBee::Screen;
__TYPES__
size_t strlcpy(char* target,const char* source,size_t size){
 const size_t length=std::strlen(source);
 if(size){const auto count=std::min(length,size-1);std::memcpy(target,source,count);target[count]=0;}
 return length;
}
struct ScreenPanelManager {
 std::vector<std::string> commands;bool ok=true;
 static ScreenPanelManager& instance(){static ScreenPanelManager manager;return manager;}
 bool dispatchAction(const char*,ScreenActionKind kind,const String& action,uint32_t& operation,String& error,const char*){
  assert(kind==ScreenActionKind::MOTOR_COMMAND);commands.emplace_back(action.c_str());
  if(!ok){error="TEST_REJECTED";return false;}operation=commands.size();return true;
 }
};
bool tap(ScreenPanelCardSnapshot& card,const ScreenPixelRect& bounds,uint16_t x,uint16_t y,
         char* confirmationCardId,size_t confirmationCapacity,ScreenControlSession* controlSession,uint32_t now)
__MOTOR__
bool confirm(ScreenPanelCardSnapshot& card)
__CONFIRM__
int main(){
 const ScreenPixelRect bounds{16,16,448,400};const ScreenMotorLayout layout{bounds};
 ScreenPanelCardSnapshot ready;ready.motorKnown=true;ready.motorPulse=1600;
 ready.motorSoftLimitEnabled=ready.motorReferenceKnown=ready.motorStationaryConfirmed=true;
 ready.motorForwardReady=ready.motorReverseReady=true;
 ready.motorPulseInput=1600;ready.motorSpeedInput=50;ready.pending=false;
 ready.requiresConfirmation=true;
 std::strcpy(ready.id,"motor");std::strcpy(ready.controlRef,"motor.8");
 auto& manager=ScreenPanelManager::instance();ScreenControlSession session;session.approve(ready.controlRef,100);
 auto invoke=[&](ScreenPanelCardSnapshot& card,ScreenPixelRect button,char* confirmation,ScreenControlSession* active){
  assert(tap(card,bounds,button.x+button.w/2,button.y+button.h/2,confirmation,41,active,101));
 };
 // Default manual mode needs no calibration, reference or confirmation dialog.
 for(unsigned direction=0;direction<2;++direction){
  auto card=ready;char confirmation[41]{};manager.commands.clear();
  card.motorKnown=false;card.motorPulse=100;card.motorSpeed=10;
  card.motorSoftLimitEnabled=card.motorReferenceKnown=card.motorStationaryConfirmed=false;
  card.motorForwardReady=card.motorReverseReady=false;
  card.requiresConfirmation=false;
  invoke(card,layout.direction(direction),confirmation,nullptr);
  assert(manager.commands.size()==1&&card.pending&&!confirmation[0]);
  assert(manager.commands[0]==(direction?"reverse":"forward"));
  card.pending=false;manager.commands.clear();card.motorSoftLimitEnabled=true;
  invoke(card,layout.direction(direction),confirmation,nullptr);
  assert(manager.commands.empty()&&card.motorUiFeedback[0]&&!card.pending);
  card.motorReferenceKnown=true;card.requiresConfirmation=true;
  if(direction)card.motorReverseReady=true;else card.motorForwardReady=true;
  invoke(card,layout.direction(direction),confirmation,nullptr);
  assert(manager.commands.empty()&&!std::strcmp(confirmation,"motor"));
  assert(card.motorConfirmAction==direction+1&&!card.motorUiFeedback[0]);
  assert(confirm(card)&&manager.commands.size()==1);
  assert(manager.commands[0]==(direction?"reverse":"forward"));
 }
 // A first tap delegates missing/zero parameters to the service. Repeated
 // taps cannot queue more starts while pending, but STOP remains available.
 for(bool known:{false,true}) for(unsigned zeroMask=0;zeroMask<4;++zeroMask)
 for(unsigned direction=0;direction<2;++direction){
  auto card=ready;char confirmation[41]{};manager.commands.clear();
  card.motorKnown=known;card.motorSpeed=(zeroMask&1)?0:10;card.motorPulse=(zeroMask&2)?0:100;
  card.motorSoftLimitEnabled=card.motorReferenceKnown=card.motorStationaryConfirmed=false;
  card.motorForwardReady=card.motorReverseReady=card.requiresConfirmation=false;
  invoke(card,layout.direction(direction),confirmation,nullptr);
  assert(card.pending&&manager.commands.size()==1&&!confirmation[0]);
  assert(manager.commands[0]==(direction?"reverse":"forward"));
  invoke(card,layout.direction(direction),confirmation,nullptr);
  invoke(card,layout.direction(1-direction),confirmation,nullptr);
  assert(manager.commands.size()==1&&!confirmation[0]);
  invoke(card,layout.stop(),confirmation,nullptr);
  assert(manager.commands.size()==2&&manager.commands.back()=="stop"&&!confirmation[0]);
 }
 for(unsigned mask=1;mask<32;++mask){
  if(mask==4)continue; // Zero parameters are initialized by the service on demand.
  auto blocked=ready;blocked.pending=mask&1;blocked.motorReferenceKnown=!(mask&2);
  if(mask&4)blocked.motorPulse=0;
  blocked.motorSpeedDirty=mask&8;blocked.motorPulseDirty=mask&16;
  for(unsigned direction=0;direction<2;++direction){
   auto card=blocked;char confirmation[41]{};manager.commands.clear();
   invoke(card,layout.direction(direction),confirmation,&session);
   assert(manager.commands.empty()&&card.error[0]&&!confirmation[0]);
   assert(!std::strcmp(card.motorUiFeedback,card.error));
   card.motorConfirmAction=direction+1;card.error[0]=0;card.motorUiFeedback[0]=0;
   assert(!confirm(card)&&manager.commands.empty()&&card.motorUiFeedback[0]);
   assert(!card.motorConfirmAction&&!std::strcmp(card.motorUiFeedback,card.error));
  }
  char confirmation[41]{};manager.commands.clear();
  invoke(blocked,layout.stop(),confirmation,nullptr);
  assert(manager.commands.size()==1&&manager.commands[0]=="stop"&&blocked.pending&&!confirmation[0]);
  assert(!blocked.motorUiFeedback[0]);
 }
 for(unsigned direction=0;direction<2;++direction){
  auto card=ready;char confirmation[41]{};manager.commands.clear();
  invoke(card,layout.direction(direction),confirmation,nullptr);
  assert(manager.commands.empty()&&card.motorConfirmAction==direction+1&&!std::strcmp(confirmation,"motor"));
  card=ready;confirmation[0]=0;invoke(card,layout.direction(direction),confirmation,&session);
  assert(manager.commands.size()==1&&manager.commands[0]==(direction?"reverse":"forward")&&card.pending);
  invoke(card,layout.direction(1-direction),confirmation,&session);assert(manager.commands.size()==1);
  invoke(card,layout.stop(),confirmation,&session);assert(manager.commands.size()==2&&manager.commands.back()=="stop");
 }
 auto card=ready;char confirmation[41]{};manager.commands.clear();manager.ok=false;
 invoke(card,layout.direction(0),confirmation,&session);
assert(!card.pending&&!std::strcmp(card.error,"TEST_REJECTED"));
assert(!std::strcmp(card.motorUiFeedback,"TEST_REJECTED"));
manager.ok=true;
// Expired feedback and unknown rest are resolved in one service operation,
// including when the screen still contains an old zero-pulse reading.
for(unsigned direction=0;direction<2;++direction){
 card=ready;card.motorKnown=false;card.motorPulse=0;card.motorStationaryConfirmed=false;
 card.motorForwardReady=card.motorReverseReady=false;manager.commands.clear();confirmation[0]=0;
 invoke(card,layout.direction(direction),confirmation,&session);
 assert(card.pending&&!confirmation[0]&&manager.commands.size()==1);
 assert(manager.commands[0]==(direction?"reverse":"forward"));
 invoke(card,layout.direction(direction),confirmation,&session);
 assert(manager.commands.size()==1);
}
}
'''.replace('__TYPES__', types).replace('__MOTOR__', motor).replace('__CONFIRM__', confirm), [ROOT / 'test/mocks', ROOT / 'include'])

    def test_trend_motor_and_timeout_taps(self):
        runtime = ((ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8') + '\n'

                   + (ROOT / 'src/screen/StripeRenderer.h').read_text(encoding='utf-8'))
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

    def test_plus_minus_hold_auto_repeat(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        touch = runtime[runtime.rindex('ITouchBackend* touch = device.touch();'):]
        chain = touch[touch.index('if (sleeping) {'):touch.index('\n        }\n\n        // Retry a touch wake')]
        retry = touch[touch.index('if (touchWakePending && sleeping &&'):touch.index('const bool requestedRefresh')]
        chain += '\n' + retry
        refresh = touch[touch.index('const bool requestedRefresh'):]
        chain += '\n' + refresh[:refresh.index('#if FASTBEE_SCREEN_WORKER_ISOLATION')]
        gesture = 'struct TouchGesture' + block(runtime, 'struct TouchGesture') + ';'
        self.compile_run(r'''
#include <Arduino.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include "screen/ScreenInteraction.h"
#include "screen/PanelStatusText.h"
using namespace FastBee::Screen;
#define LOG_INFOF(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)
struct Logger { template<class... Args> void infof(const char*, Args...) {} } LOGGER;
size_t strlcpy(char* to,const char* from,size_t n){if(n)snprintf(to,n,"%s",from);return strlen(from);}
constexpr int MIN_SWIPE_PX=40,TAP_MOVE_PX=14,TAP_MAX_MS=900;
constexpr unsigned REPEAT_START_MS=600,REPEAT_INTERVAL_MS=150,REPEAT_FAST_AFTER_MS=2000;constexpr int REPEAT_FAST_MULTIPLIER=10;
enum class ScreenVisualType{SLIDER,ACTION,MOTOR_CONTROL,NUMBER_CONTROL};
enum class NumericControlHit : uint8_t { NONE = 0, DECREMENT, INCREMENT, EXECUTE };
struct Card {ScreenVisualType visualType=ScreenVisualType::MOTOR_CONTROL;float numericValue=5;char id[41]="card",error[97]{};
 bool pending=false,motorSpeedDirty=false,motorPulseDirty=false;uint16_t motorSpeedInput=10,motorPulseInput=100,motorSpeedStep=1,motorPulseStep=100;
 char motorUiFeedback[97]{},controlRef[97]{};float rangeMin=0,rangeMax=9,numericStep=1;uint8_t precision=0;};
struct Snapshot{std::array<Card,2> cards;unsigned revision=1;uint8_t brightnessPercent=70,activePageIndex=0;};
struct TouchPoint{uint16_t x,y;bool pressed;};
GESTURE
struct Display{struct Caps{bool brightnessContinuous=true;}caps;Caps capabilities(){return caps;}bool setBrightness(uint8_t){return true;}bool setSleep(bool){return true;}};
struct Device{Display d;Display* display(){return &d;}};
struct Renderer{using Rect=ScreenPixelRect;String pressed;int invalidations=0;void invalidate(){++invalidations;}
 Rect cardBounds(Snapshot&,Card&){return Rect{0,0,448,400};}
 int hitTest(Snapshot&,int,int y){return y<350?0:1;}
 int tapTarget(Snapshot&,int,int,const char*){return 1;}
 int width(){return 480;}int bufferBytes(){return 100;}
 void closeNavigation(){}void setPressed(const char* id,int=0,int=0){pressed=id?id:"";}
 bool feedbackHitTest(Snapshot&,int,int){return false;}
 NumericControlHit numericControlHitTest(Snapshot&,Card&,int x,int y){return y>=420?NumericControlHit::NONE:(x>=224?NumericControlHit::INCREMENT:NumericControlHit::DECREMENT);}
 template<class S,class C>bool actionHitTest(const S&,const C&,int,int){return true;}};
int taps=0,pages=0;
void handleTap(Renderer&,Snapshot&,int,int,char*,size_t,ScreenControlSession*,unsigned){++taps;}
void changePage(Snapshot&,int){++pages;}
struct ScreenPanelManager{static ScreenPanelManager& instance(){static ScreenPanelManager m;return m;}bool queueSetting(const String&,unsigned,String&){return true;}void reportApplication(bool,bool,const String&){}};
struct Harness{
 Device device;Renderer renderer;Snapshot s;Snapshot* snapshot=&s;TouchGesture gesture;
 ScreenControlSession controlSession;int updates=0,feedback=0;
 struct State{std::atomic<bool> refreshRequested{false};}state;
 bool sleeping=false,brightnessOverridden=false,forceRender=false;uint8_t appliedBrightness=70;
 bool touchWakePending=false,touchWakeContactDown=false;uint8_t touchWakeAttempts=0;
 uint32_t touchWakeNextAttemptMs=0,lastInteractionMs=0;
 unsigned brightnessOverrideRevision=0;char confirmationCardId[41]{};size_t snapshotBytes=100,framebufferBytes=100;
 void updateRuntimeState(bool,bool,size_t){++updates;}
 void touchFeedback(unsigned){++feedback;}
 Harness(){s.cards[1].visualType=ScreenVisualType::NUMBER_CONTROL;}
 void process(TouchPoint point,unsigned now){CHAIN}
};
TouchPoint tp(unsigned x,unsigned y,bool pressed){return TouchPoint{static_cast<uint16_t>(x),static_cast<uint16_t>(y),pressed};}
const ScreenMotorLayout motorLayout{ScreenPixelRect{0,0,448,400}};
const ScreenPixelRect pulsePlus=motorLayout.adjust(1,1),pulseMinus=motorLayout.adjust(1,0),pulseSet=motorLayout.adjust(1,2);
const unsigned PX=pulsePlus.x+pulsePlus.w/2,PY=pulsePlus.y+pulsePlus.h/2;
const unsigned MX=pulseMinus.x+pulseMinus.w/2,SX=pulseSet.x+pulseSet.w/2;
void hold(Harness& h,unsigned x,unsigned y,unsigned from,unsigned to){
 for(unsigned t=from+10;t-from<to-from;t+=10)h.process(tp(x,y,true),t);
}
int main(){
 // A short press still dispatches exactly one tap and arms no repeat.
 {Harness h;taps=0;h.process(tp(PX,PY,true),1000);hold(h,PX,PY,1000,1290);h.process(tp(PX,PY,false),1290);
  assert(taps==1&&h.s.cards[0].motorPulseInput==100&&!h.s.cards[0].motorPulseDirty);}
 // First tick at 600ms, then one step every 150ms; a release inside the
 // 900ms tap window after a repeat must not double-step.
 {Harness h;taps=0;auto& c=h.s.cards[0];
  h.process(tp(PX,PY,true),0);hold(h,PX,PY,0,590);assert(c.motorPulseInput==100);
  h.process(tp(PX,PY,true),600);assert(c.motorPulseInput==200&&c.motorPulseDirty);
  hold(h,PX,PY,600,750);assert(c.motorPulseInput==200);
  h.process(tp(PX,PY,true),750);assert(c.motorPulseInput==300);
  hold(h,PX,PY,750,880);assert(c.motorPulseInput==300);
  h.process(tp(PX,PY,false),880);
  assert(taps==0&&c.motorPulseInput==300&&h.renderer.pressed.isEmpty());}
 // Holding past 2s multiplies each step by ten: ten x1 ticks from 600ms to
 // 1950ms, then four x10 ticks at 4100/4250/4400/4550ms.
 {Harness h;taps=0;auto& c=h.s.cards[0];
  h.process(tp(PX,PY,true),2000);hold(h,PX,PY,2000,4600);h.process(tp(PX,PY,false),4600);
  assert(c.motorPulseInput==100+10*100+4*1000&&taps==0);}
 // Decrement clamps at zero instead of underflowing the draft.
 {Harness h;auto& c=h.s.cards[0];c.motorPulseInput=250;
  h.process(tp(MX,PY,true),0);hold(h,MX,PY,0,1200);h.process(tp(MX,PY,false),1200);
  assert(c.motorPulseInput==0);}
 // Moving past the tap threshold before 600ms disarms the repeat for good.
 {Harness h;taps=0;auto& c=h.s.cards[0];
  h.process(tp(PX,PY,true),0);h.process(tp(PX+20,PY,true),20);hold(h,PX+20,PY,20,1000);h.process(tp(PX+20,PY,false),1000);
  assert(c.motorPulseInput==100&&taps==0);}
 // The motor SET button and the numeric EXECUTE region are not repeat
 // targets; a hold there keeps the single-tap behaviour.
 {Harness h;taps=0;auto& c=h.s.cards[0];
  h.process(tp(SX,PY,true),0);hold(h,SX,PY,0,800);h.process(tp(SX,PY,false),800);
  assert(c.motorPulseInput==100&&taps==1);}
 {Harness h;taps=0;auto& c=h.s.cards[1];
  h.process(tp(300,440,true),0);hold(h,300,440,0,800);h.process(tp(300,440,false),800);
  assert(c.numericValue==5&&taps==1);}
 // NUMBER_CONTROL + repeats and clamps at the configured range maximum.
 {Harness h;taps=0;auto& c=h.s.cards[1];
  h.process(tp(300,380,true),0);hold(h,300,380,0,2000);h.process(tp(300,380,false),2000);
  assert(taps==0&&c.numericValue==9);}
 // NUMBER_CONTROL - repeats downward on the same cadence.
 {Harness h;taps=0;auto& c=h.s.cards[1];
  h.process(tp(100,380,true),0);hold(h,100,380,0,900);h.process(tp(100,380,false),900);
  assert(taps==0&&c.numericValue==3);}
 // A pending motor card ticks without touching the draft and the release
 // still does not become a tap.
 {Harness h;taps=0;auto& c=h.s.cards[0];c.pending=true;
  h.process(tp(PX,PY,true),0);hold(h,PX,PY,0,1000);h.process(tp(PX,PY,false),1000);
  assert(c.motorPulseInput==100&&taps==0);}
 // A layout refresh during the contact retires the gesture; the rest of the
 // hold cannot repeat and the release dispatches nothing.
 {Harness h;taps=0;auto& c=h.s.cards[0];
  h.process(tp(PX,PY,true),0);hold(h,PX,PY,0,700);assert(c.motorPulseInput==200);
  h.state.refreshRequested=true;h.process(tp(PX,PY,true),720);
  assert(!h.gesture.down&&h.gesture.suppressUntilRelease);
  hold(h,PX,PY,720,1200);h.process(tp(PX,PY,false),1200);
  assert(c.motorPulseInput==200&&taps==0);}
 // A snapshot revision change mid-hold stops the repeat immediately.
 {Harness h;taps=0;auto& c=h.s.cards[0];
  h.process(tp(PX,PY,true),0);hold(h,PX,PY,0,700);assert(c.motorPulseInput==200);
  ++h.s.revision;hold(h,PX,PY,700,1200);h.process(tp(PX,PY,false),1200);
  assert(c.motorPulseInput==200&&taps==0);}
 // millis() wrap: the 600ms deadline, the 150ms cadence and the release
 // guard all survive a rollover of the uptime counter.
 {Harness h;taps=0;auto& c=h.s.cards[0];
  h.process(tp(PX,PY,true),0xfffffff0U);hold(h,PX,PY,0xfffffff0U,0x300U);h.process(tp(PX,PY,false),0x300U);
  assert(c.motorPulseInput==300&&taps==0);}
}
'''.replace('GESTURE', gesture).replace('CHAIN', chain), [ROOT / 'test/mocks', ROOT / 'include'])
