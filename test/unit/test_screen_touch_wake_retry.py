"""Exercise production touch-wake retries and release-build diagnostics."""
from pathlib import Path
import unittest

from test.unit import test_panel_runtime_regressions as helpers

ROOT = Path(__file__).resolve().parents[2]


class ScreenTouchWakeRetryTests(unittest.TestCase):
    compile_run = helpers.PanelRuntimeRegressionTests.compile_run

    def run_wake_case(self, case):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        touch = runtime[runtime.rindex('ITouchBackend* touch = device.touch();'):]
        event = 'if (sleeping)' + helpers.block(touch, 'if (sleeping)')
        retry_marker = 'if (touchWakePending && sleeping &&'
        retry = ''
        if retry_marker in touch:
            start = touch.index(retry_marker)
            retry = touch[start:touch.index('const bool requestedRefresh', start)]
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "screen/ScreenInteraction.h"
using FastBee::Screen::ScreenControlSession;
// The release profile strips LOG_INFOF but must retain wake diagnostics.
#define LOG_INFOF(...) ((void)0)
struct Logger {
    std::vector<std::string> info, warnings;
    template<class... Args>
    void append(std::vector<std::string>& messages, const char* format, Args... args) {
        char message[256];
        const int length=std::snprintf(message,sizeof(message),format,args...);
        assert(length>=0&&static_cast<size_t>(length)<sizeof(message));
        messages.emplace_back(message);
    }
    template<class... Args> void infof(const char* format, Args... args) { append(info,format,args...); }
    template<class... Args> void warningf(const char* format, Args... args) { append(warnings,format,args...); }
} LOGGER;
#define LOG_WARNINGF(...) LOGGER.warningf(__VA_ARGS__)
struct TouchPoint { bool pressed; uint16_t x=10,y=20; };
struct TouchGesture { bool down=false,suppressUntilRelease=false; };
struct Snapshot { uint8_t brightnessPercent=52; };
struct Display {
    bool brightnessOk=true,wakeOk=true; int brightnessCalls=0,wakeCalls=0;
    uint8_t brightness=0;
    bool setBrightness(uint8_t value) { ++brightnessCalls; brightness=value; return brightnessOk; }
    bool setSleep(bool asleep) { assert(!asleep); ++wakeCalls; return wakeOk; }
};
struct Device { Display d; Display* display() { return &d; } };
struct Renderer { int invalidations=0; void invalidate() { ++invalidations; } int bufferBytes() { return 16; } };
struct Harness {
    Device device; Renderer renderer; Snapshot s; Snapshot* snapshot=&s;
    ScreenControlSession controlSession; TouchGesture gesture;
    bool sleeping=true,brightnessOverridden=false,forceRender=false;
    bool touchWakePending=false,touchWakeContactDown=false;
    uint8_t appliedBrightness=35,touchWakeAttempts=0;
    uint32_t touchWakeNextAttemptMs=0,lastInteractionMs=0;
    int updates=0,snapshotBytes=10,framebufferBytes=100;
    char confirmationCardId[32]="motor-confirmation";
    void updateRuntimeState(bool running,bool asleep,int bytes) { assert(running&&!asleep&&bytes==126); ++updates; }
    void process(bool pressed,uint32_t now) { TouchPoint point{pressed}; (void)point; (void)now; EVENT tick(now); }
    void tick(uint32_t now) { (void)now; RETRY }
};
int main() {
CASE
}
'''.replace('EVENT', event).replace('RETRY', retry).replace('CASE', case), [ROOT / 'include'])

    def test_wake_retry_survives_release_is_bounded_and_consumes_contact(self):
        self.run_wake_case(r'''
    // A short tap must survive an I2C failure even after the finger lifts.
    Harness h; h.controlSession.approve("motor.test",0); h.device.d.brightnessOk=false;
    h.process(true,0); assert(h.sleeping&&h.device.d.brightnessCalls==1&&h.device.d.wakeCalls==0);
    h.device.d.brightnessOk=true; h.process(false,50); assert(h.device.d.brightnessCalls==1);
    h.tick(100); assert(!h.sleeping&&h.updates==1&&!h.gesture.suppressUntilRelease);
    assert(!h.controlSession.allows("motor.test")&&!h.confirmationCardId[0]);
    assert(h.forceRender&&h.renderer.invalidations==1&&h.lastInteractionMs==100);
    h.tick(200); assert(h.device.d.brightnessCalls==2&&h.updates==1);
    // A successful immediate wake consumes the held contact, not a control.
    Harness held; held.brightnessOverridden=true; held.process(true,5);
    assert(!held.sleeping&&held.gesture.suppressUntilRelease&&held.device.d.brightness==35);
    // Failure of the OFF-to-ON write also retries; a held finger cannot reset
    // the attempt budget and create an unbounded I2C loop.
    Harness failed; failed.device.d.wakeOk=false;
    failed.process(true,1000); failed.process(true,1050); failed.process(true,1100);
    failed.process(true,1200); failed.process(true,1300); failed.tick(1400);
    assert(failed.sleeping&&failed.device.d.wakeCalls==3&&!failed.touchWakePending&&failed.updates==0);
    failed.device.d.wakeOk=true; failed.process(false,1500); failed.process(true,1510);
    assert(!failed.sleeping&&failed.device.d.wakeCalls==4&&failed.gesture.suppressUntilRelease);
    // Cancellation after a different wake path must never reapply stale work.
    Harness canceled; canceled.device.d.wakeOk=false; canceled.process(true,1);
    canceled.sleeping=false; canceled.tick(101);
    assert(!canceled.touchWakePending&&!canceled.touchWakeContactDown&&canceled.device.d.wakeCalls==1);
    // Deadline checks must remain correct across millis() rollover.
    Harness wrapped; wrapped.device.d.wakeOk=false; wrapped.process(true,0xfffffff0U);
    wrapped.device.d.wakeOk=true; wrapped.process(false,20); assert(wrapped.sleeping);
    wrapped.tick(84); assert(!wrapped.sleeping&&wrapped.updates==1);
''')

    def test_no_contact_or_release_only_does_not_start_wake(self):
        self.run_wake_case(r'''
    Harness idle;
    idle.tick(0); idle.process(false,100); idle.tick(200); idle.process(false,500);
    assert(idle.sleeping&&!idle.touchWakePending&&idle.touchWakeAttempts==0);
    assert(idle.device.d.brightnessCalls==0&&idle.device.d.wakeCalls==0);
    assert(idle.updates==0&&!idle.forceRender&&idle.renderer.invalidations==0);
    assert(LOGGER.info.empty()&&LOGGER.warnings.empty());
''')

    def test_persistent_brightness_failure_stops_without_enabling_backlight(self):
        self.run_wake_case(r'''
    Harness failed;
    failed.controlSession.approve("motor.test",0);
    failed.device.d.brightnessOk=false;
    failed.process(true,0);
    for (uint32_t now=1;now<=1000;++now) failed.process(true,now);
    assert(failed.sleeping&&!failed.touchWakePending&&failed.touchWakeAttempts==3);
    assert(failed.device.d.brightnessCalls==3&&failed.device.d.wakeCalls==0);
    assert(failed.updates==0&&!failed.forceRender&&failed.renderer.invalidations==0);
    assert(failed.controlSession.allows("motor.test"));
    assert(std::strcmp(failed.confirmationCardId,"motor-confirmation")==0);
    assert(LOGGER.info.size()==1&&LOGGER.warnings.size()==3);
    assert(LOGGER.warnings.back()=="ScreenRuntime: touch wake failed stage=brightness attempt=3 retry=0");
    // A released contact by itself must not restart an exhausted request.
    failed.process(false,1100); failed.tick(1200);
    assert(failed.device.d.brightnessCalls==3&&failed.device.d.wakeCalls==0);
''')

    def test_released_tap_can_succeed_on_last_attempt_with_release_diagnostics(self):
        self.run_wake_case(r'''
    Harness h;
    h.device.d.brightnessOk=false;
    h.process(true,0);
    h.process(false,1);
    h.device.d.brightnessOk=true;
    h.device.d.wakeOk=false;
    h.tick(99); assert(h.device.d.brightnessCalls==1&&h.device.d.wakeCalls==0);
    h.tick(100); assert(h.sleeping&&h.device.d.wakeCalls==1);
    h.device.d.wakeOk=true;
    h.tick(199); assert(h.sleeping&&h.device.d.brightnessCalls==2);
    h.tick(200);
    assert(!h.sleeping&&!h.touchWakePending&&!h.gesture.suppressUntilRelease);
    assert(h.device.d.brightnessCalls==3&&h.device.d.wakeCalls==2);
    assert(h.lastInteractionMs==200&&h.updates==1&&h.renderer.invalidations==1);
    assert(LOGGER.info==std::vector<std::string>({
        "ScreenRuntime: touch wake requested x=10 y=20",
        "ScreenRuntime: touch wake applied attempt=3"}));
    assert(LOGGER.warnings==std::vector<std::string>({
        "ScreenRuntime: touch wake failed stage=brightness attempt=1 retry=1",
        "ScreenRuntime: touch wake failed stage=backlight-on attempt=2 retry=1"}));
    h.tick(1000);
    assert(h.device.d.brightnessCalls==3&&h.device.d.wakeCalls==2&&h.updates==1);
    assert(LOGGER.info.size()==2&&LOGGER.warnings.size()==2);
''')

    def test_repeated_sleep_cycles_reset_request_and_retry_budget(self):
        self.run_wake_case(r'''
    Harness h;
    for (uint32_t cycle=0;cycle<4;++cycle) {
        const uint32_t started=cycle*1000;
        h.sleeping=true;
        h.controlSession.approve("motor.test",started);
        std::strcpy(h.confirmationCardId,"motor-confirmation");
        h.device.d.wakeOk=false;
        h.process(true,started); h.process(false,started+10); h.tick(started+100);
        h.device.d.wakeOk=true;
        h.tick(started+200);
        assert(!h.sleeping&&!h.touchWakePending&&!h.touchWakeContactDown);
        assert(!h.gesture.suppressUntilRelease&&!h.controlSession.allows("motor.test"));
        assert(!h.confirmationCardId[0]&&h.lastInteractionMs==started+200);
        assert(h.device.d.wakeCalls==static_cast<int>((cycle+1)*3));
        assert(h.updates==static_cast<int>(cycle+1));
        h.tick(started+900);
        assert(h.device.d.wakeCalls==static_cast<int>((cycle+1)*3));
    }
    assert(LOGGER.info.size()==8&&LOGGER.warnings.size()==8);
''')
