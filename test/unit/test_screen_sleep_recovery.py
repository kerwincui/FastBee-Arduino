"""Exercise production revision-wake and idle-sleep paths without hardware."""
from pathlib import Path
import unittest

from test.unit import test_panel_runtime_regressions as helpers

ROOT = Path(__file__).resolve().parents[2]


class ScreenSleepRecoveryTests(unittest.TestCase):
    compile_run = helpers.PanelRuntimeRegressionTests.compile_run

    def test_saved_revision_wakes_and_clears_control_permission(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        marker = 'if (controlSessionRevision != snapshot->revision)'
        production = marker + helpers.block(runtime, marker)
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include "screen/ScreenInteraction.h"
using FastBee::Screen::ScreenControlSession;
struct Snapshot { uint32_t revision=38; uint8_t brightnessPercent=52; };
struct TouchGesture { bool down=false, suppressUntilRelease=false; };
struct Display {
    bool brightnessOk=true, wakeOk=true; int brightnessCalls=0, wakeCalls=0;
    bool setBrightness(uint8_t v) { assert(v==52); ++brightnessCalls; return brightnessOk; }
    bool setSleep(bool value) { assert(!value); ++wakeCalls; return wakeOk; }
};
struct Device { Display d; Display* display() { return &d; } };
struct Renderer {
    int invalidations=0; void closeNavigation() {}
    void invalidate() { ++invalidations; } int bufferBytes() { return 16; }
};
struct Harness {
    Snapshot s; Snapshot* snapshot=&s; Device device; Renderer renderer;
    ScreenControlSession controlSession; TouchGesture gesture;
    uint32_t controlSessionRevision=37, lastInteractionMs=1, now=500;
    uint8_t appliedBrightness=52; bool sleeping=true, forceRender=false;
    char confirmationCardId[32]="motor-approval";
    int snapshotBytes=10, framebufferBytes=100, updates=0;
    Harness() { gesture.down=true; }
    void updateRuntimeState(bool running,bool asleep,int bytes) {
        assert(running&&!asleep&&bytes==126); ++updates;
    }
    void refresh() { PRODUCTION }
};
int main() {
    Harness h; h.controlSession.approve("motor.example",0); h.refresh();
    assert(!h.sleeping&&h.updates==1&&h.device.d.wakeCalls==1);
    assert(h.gesture.suppressUntilRelease&&!h.gesture.down);
    assert(!h.controlSession.allows("motor.example")&&!h.confirmationCardId[0]);
    assert(h.lastInteractionMs==500&&h.forceRender&&h.controlSessionRevision==38);
    h.refresh(); assert(h.device.d.wakeCalls==1&&h.updates==1);
    Harness brightnessFailure; brightnessFailure.device.d.brightnessOk=false;
    brightnessFailure.refresh();
    assert(brightnessFailure.sleeping&&brightnessFailure.updates==0&&brightnessFailure.device.d.wakeCalls==0);
    Harness wakeFailure; wakeFailure.device.d.wakeOk=false; wakeFailure.refresh();
    assert(wakeFailure.sleeping&&wakeFailure.updates==0&&!wakeFailure.gesture.suppressUntilRelease);
}
'''.replace('PRODUCTION', production), [ROOT / 'include'])

    def test_disabled_timeout_stays_awake_and_sleep_requires_ready_touch(self):
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        start = runtime.index('const bool canWakeLocally = device.touch()')
        fragment = runtime[start:]
        prefix = fragment[:fragment.index('{')]
        production = prefix + helpers.block(fragment, 'if (')
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
#include "screen/ScreenInteraction.h"
using FastBee::Screen::ScreenControlSession;
struct Touch { bool ok=true; bool ready() { return ok; } };
struct Display { int calls=0; bool ok=true; bool setSleep(bool asleep) { assert(asleep); ++calls; return ok; } };
struct Device { Touch t; Display d; bool attached=true; Touch* touch() { return attached?&t:nullptr; } Display* display() { return &d; } };
struct Renderer { int bufferBytes() { return 16; } };
struct Harness {
    Device device; Renderer renderer; ScreenControlSession controlSession;
    uint32_t now=200000, lastInteractionMs=0, idleSleepSeconds=0;
    bool sleeping=false; char confirmationCardId[32]="approved";
    int updates=0,snapshotBytes=10,framebufferBytes=100;
    void updateRuntimeState(bool running,bool asleep,int bytes) { assert(running&&asleep&&bytes==126); ++updates; }
    void poll() { PRODUCTION }
};
int main() {
    Harness h; h.poll(); assert(!h.sleeping&&h.device.d.calls==0);
    h.now=0xffffffffU; h.poll(); assert(!h.sleeping&&h.device.d.calls==0);
    h.idleSleepSeconds=30; h.now=29999; h.poll(); assert(!h.sleeping);
    h.now=30000; h.controlSession.approve("motor.example",0); h.poll();
    assert(h.sleeping&&h.updates==1&&!h.confirmationCardId[0]&&!h.controlSession.allows("motor.example"));
    h.poll(); assert(h.device.d.calls==1);
    Harness missing; missing.idleSleepSeconds=30; missing.device.attached=false;
    missing.poll(); assert(!missing.sleeping&&missing.device.d.calls==0);
    missing.device.attached=true; missing.device.t.ok=false; missing.poll(); assert(!missing.sleeping);
    missing.device.t.ok=true; missing.device.d.ok=false; missing.poll(); assert(!missing.sleeping&&missing.updates==0);
}
'''.replace('PRODUCTION', production), [ROOT / 'include'])
