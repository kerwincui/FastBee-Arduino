#include "screen/ScreenWorkerIsolation.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using FastBee::Screen::ScreenIsolationTitle;
using FastBee::Screen::ScreenWorkerIsolation;
using Phase = ScreenWorkerIsolation::Phase;

static void checkTimeline(uint32_t started) {
    ScreenWorkerIsolation gate(started);
    assert(gate.phase(started) == Phase::Hold);
    assert(gate.needsPhaseFrame(Phase::Hold));
    assert(gate.allowSnapshot(Phase::Hold));
    assert(gate.allowRender(Phase::Hold));
    assert(!gate.allowTouch(Phase::Hold));
    gate.rendered(Phase::Hold);
    assert(!gate.needsPhaseFrame(Phase::Hold));
    assert(!gate.allowSnapshot(Phase::Hold));
    assert(!gate.allowRender(Phase::Hold));

    // Arbitrarily many task notifications cannot consume a real-time phase.
    for (unsigned notification = 0; notification < 10000; ++notification)
        assert(gate.phase(started + 89999) == Phase::Hold);
    assert(gate.phase(started + 90000) == Phase::Snapshot);
    assert(gate.allowSnapshot(Phase::Snapshot));
    assert(gate.allowRender(Phase::Snapshot));  // One visible phase marker.
    gate.rendered(Phase::Snapshot);
    assert(gate.allowSnapshot(Phase::Snapshot));
    assert(!gate.allowRender(Phase::Snapshot));
    assert(!gate.allowTouch(Phase::Snapshot));
    assert(gate.phase(started + 179999) == Phase::Snapshot);
    assert(gate.phase(started + 180000) == Phase::Render);
    assert(gate.allowSnapshot(Phase::Render));
    assert(gate.allowRender(Phase::Render));
    gate.rendered(Phase::Render);
    assert(gate.allowRender(Phase::Render));
    assert(!gate.allowTouch(Phase::Render));
    assert(gate.phase(started + 269999) == Phase::Render);
    assert(gate.phase(started + 270000) == Phase::Normal);
    assert(gate.needsPhaseFrame(Phase::Normal));
    assert(gate.allowSnapshot(Phase::Normal));
    assert(gate.allowRender(Phase::Normal));
    assert(gate.allowTouch(Phase::Normal));
    assert(ScreenWorkerIsolation::title(Phase::Normal) == nullptr);
    // After normal recovery, another millis() wrap must not restart isolation.
    assert(gate.phase(started) == Phase::Normal);
}

static bool failedRender(char (&title)[65]) {
    ScreenIsolationTitle<sizeof(title)> marker(title, "C: RENDER");
    assert(std::strcmp(title, "C: RENDER") == 0);
    return false;
}

int main() {
    checkTimeline(1000);
    checkTimeline(UINT32_MAX - 40000);  // All deadlines cross millis() wrap.

    // Snapshot or render failure must not prevent the automatic exit.
    ScreenWorkerIsolation neverRendered(400);
    assert(neverRendered.phase(400 + 90000) == Phase::Snapshot);
    assert(neverRendered.phase(400 + 180000) == Phase::Render);
    assert(neverRendered.phase(400 + 270000) == Phase::Normal);
    assert(neverRendered.allowTouch(Phase::Normal));

    char title[65] = "Original configured page title";
    char original[65];
    std::memcpy(original, title, sizeof(title));
    for (const auto phase : {Phase::Hold, Phase::Snapshot, Phase::Render}) {
        {
            ScreenIsolationTitle<sizeof(title)> marker(title, ScreenWorkerIsolation::title(phase));
            assert(std::strcmp(title, ScreenWorkerIsolation::title(phase)) == 0);
        }
        assert(std::memcmp(title, original, sizeof(title)) == 0);
    }
    assert(!failedRender(title));
    assert(std::memcmp(title, original, sizeof(title)) == 0);
    {
        ScreenIsolationTitle<sizeof(title)> noMarker(title, nullptr);
        assert(std::memcmp(title, original, sizeof(title)) == 0);
    }
    {
        ScreenIsolationTitle<65> noPage(nullptr, "A: HOLD");
    }
    char shortTitle[4] = "old";
    {
        ScreenIsolationTitle<sizeof(shortTitle)> marker(shortTitle, "B: SNAPSHOT");
        assert(std::strcmp(shortTitle, "B: ") == 0);
    }
    assert(std::strcmp(shortTitle, "old") == 0);
    std::puts("screen worker isolation: time gates, wrap, render failure and title restoration passed");
}
