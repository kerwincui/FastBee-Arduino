#include "core/ComplementaryInputMonitor.h"
#include <cassert>

int main() {
    using FastBee::ComplementaryInputMonitor;
    ComplementaryInputMonitor monitor;
    assert(!monitor.sample(1, 0, 0, 200));
    assert(!monitor.sample(1, 0, 200, 200));
    assert(!monitor.fault());
    // A short NO/NC overlap followed by the other normal state is harmless.
    assert(!monitor.sample(1, 1, 220, 200));
    assert(!monitor.sample(0, 1, 240, 200));
    assert(!monitor.sample(0, 1, 440, 200));
    assert(!monitor.fault());
    assert(!monitor.sample(1, 1, 460, 200));
    assert(!monitor.sample(1, 1, 659, 200));
    assert(monitor.sample(1, 1, 660, 200));
    assert(monitor.fault() && monitor.publishDue(660));
    assert(!monitor.sample(1, 1, 680, 200));
    // Failed delivery is throttled, successful delivery ends this episode.
    monitor.published(660, false);
    assert(!monitor.publishDue(5659));
    assert(monitor.publishDue(5660));
    monitor.published(5660, true);
    assert(!monitor.publishDue(99999));
    // Equal-low remains the same continuous fault, with no extra alarm.
    assert(!monitor.sample(0, 0, 6000, 200));
    assert(!monitor.sample(0, 0, 6200, 200));
    assert(monitor.fault() && !monitor.publishDue(6200));
    // Stable recovery re-arms, not a transient normal combination.
    assert(!monitor.sample(1, 0, 6400, 200));
    assert(!monitor.sample(0, 0, 6500, 200));
    assert(!monitor.sample(0, 0, 6700, 200));
    assert(monitor.fault());
    assert(!monitor.sample(1, 0, 6800, 200));
    assert(!monitor.sample(1, 0, 7000, 200));
    assert(!monitor.fault());
    assert(!monitor.sample(0, 0, 7200, 200));
    assert(monitor.sample(0, 0, 7400, 200));
    assert(monitor.publishDue(7400));
    // Offline alarms that recover are not sent as stale faults on reconnect.
    assert(!monitor.sample(0, 1, 7600, 200));
    assert(!monitor.sample(0, 1, 7800, 200));
    assert(!monitor.publishDue(7800));
    // Startup fault, unreadable input, and unsigned millis rollover.
    ComplementaryInputMonitor startup;
    assert(!startup.sample(-1, 0, UINT32_MAX - 100, 200));
    assert(!startup.sample(-1, 0, 98, 200));
    assert(startup.sample(-1, 0, 99, 200));
    startup.published(UINT32_MAX - 100, false);
    assert(!startup.publishDue(4898));
    assert(startup.publishDue(4899));
    ComplementaryInputMonitor high;
    assert(!high.sample(1, 1, 0, 200));
    assert(high.sample(1, 1, 200, 200));
    ComplementaryInputMonitor low;
    assert(!low.sample(0, 0, 0, 200));
    assert(low.sample(0, 0, 200, 200));
}
