#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <vector>
#include "screen/ScreenInteraction.h"

enum class ScreenSystemAction : uint8_t {
    NONE = 0,
    SET_BRIGHTNESS,
    SLEEP,
    BUZZER,
};

struct DisplayCapabilities {
    bool brightness = false;
    bool sleep = false;
    bool buzzer = false;
};

struct TouchGesture {
    uint8_t marker = 0;
};

struct FakeDisplay {
    DisplayCapabilities caps{};
    std::deque<bool> buzzerResults;
    std::deque<bool> brightnessResults;
    std::deque<bool> sleepResults;
    std::vector<bool> buzzerCalls;
    std::vector<uint8_t> brightnessCalls;
    std::vector<bool> sleepCalls;

    static bool take(std::deque<bool>& results) {
        if (results.empty()) return true;
        const bool result = results.front();
        results.pop_front();
        return result;
    }

    DisplayCapabilities capabilities() const { return caps; }
    bool setBuzzer(bool enabled) {
        buzzerCalls.push_back(enabled);
        return take(buzzerResults);
    }
    bool setBrightness(uint8_t value) {
        brightnessCalls.push_back(value);
        return take(brightnessResults);
    }
    bool setSleep(bool enabled) {
        sleepCalls.push_back(enabled);
        return take(sleepResults);
    }
};

struct FakeTouch {
    bool isReady = true;
    bool ready() const { return isReady; }
};

struct FakeDevice {
    FakeDisplay* displayBackend = nullptr;
    FakeTouch* touchBackend = nullptr;
    FakeDisplay* display() { return displayBackend; }
    FakeTouch* touch() { return touchBackend; }
};

struct Snapshot {
    uint32_t revision = 17;
};

struct FakeRenderer {
    size_t bytes = 3072;
    size_t bufferBytes() const { return bytes; }
};

struct HarnessState {
    FakeDisplay displayBackend;
    FakeTouch touchBackend;
    FakeDevice device{&displayBackend, &touchBackend};
    Snapshot snapshot;
    FakeRenderer renderer;
    bool sleeping = false;
    uint8_t appliedBrightness = 255;
    bool brightnessOverridden = false;
    uint32_t brightnessOverrideRevision = 0;
    uint32_t lastInteractionMs = 0;
    bool buzzerNeedsOff = false;
    uint32_t buzzerOffAtMs = 0;
    uint32_t buzzerOffFailureLoggedAtMs = 0;
    TouchGesture gesture{};
    FastBee::Screen::ScreenControlSession controlSession;
    char confirmationCardId[41]{};
    size_t snapshotBytes = 4096;
    size_t framebufferBytes = 460800;
    unsigned warningCount = 0;
    unsigned runtimeUpdateCount = 0;
    bool runtimeRunning = false;
    bool runtimeSleeping = false;
    size_t runtimeAllocated = 0;
    bool lastApplied = false;
};

static unsigned delayCalls = 0;
static unsigned delayedMs = 0;

void delay(unsigned milliseconds) {
    ++delayCalls;
    delayedMs += milliseconds;
}

#define LOG_WARNING(message) do { (void)(message); ++state.warningCount; } while (0)

#include "screen_system_actions_production.inc"

void testBrightnessAndSleepDispatch() {
    HarnessState brightness;
    brightness.displayBackend.caps.brightness = true;
    brightness.sleeping = true;
    assert(runSystemAction(brightness, ScreenSystemAction::SET_BRIGHTNESS,
                           42, 500));
    assert(brightness.displayBackend.sleepCalls.size() == 1);
    assert(!brightness.displayBackend.sleepCalls[0]);
    assert(brightness.displayBackend.brightnessCalls.size() == 1);
    assert(brightness.displayBackend.brightnessCalls[0] == 42);
    assert(!brightness.sleeping);
    assert(brightness.appliedBrightness == 42);
    assert(brightness.brightnessOverridden);
    assert(brightness.brightnessOverrideRevision == brightness.snapshot.revision);
    assert(brightness.lastInteractionMs == 500);
    assert(brightness.runtimeUpdateCount == 1);

    HarnessState sleep;
    sleep.displayBackend.caps.sleep = true;
    sleep.gesture.marker = 9;
    sleep.controlSession.approve("motor", 500);
    std::strcpy(sleep.confirmationCardId, "motor");
    assert(runSystemAction(sleep, ScreenSystemAction::SLEEP, 0, 600));
    assert(sleep.displayBackend.sleepCalls.size() == 1);
    assert(sleep.displayBackend.sleepCalls[0]);
    assert(sleep.sleeping);
    assert(sleep.gesture.marker == 0);
    assert(!sleep.controlSession.allows("motor") && !sleep.confirmationCardId[0]);
    assert(sleep.runtimeUpdateCount == 1);

    HarnessState noTouch;
    noTouch.displayBackend.caps.sleep = true;
    noTouch.touchBackend.isReady = false;
    noTouch.controlSession.approve("motor", 500);
    std::strcpy(noTouch.confirmationCardId, "motor");
    assert(!runSystemAction(noTouch, ScreenSystemAction::SLEEP, 0, 700));
    assert(noTouch.displayBackend.sleepCalls.empty());
    assert(noTouch.controlSession.allows("motor") && std::strcmp(noTouch.confirmationCardId,"motor")==0);
}

void testBuzzerClosesAfter80Ms() {
    HarnessState state;
    state.displayBackend.caps.buzzer = true;
    assert(runSystemAction(state, ScreenSystemAction::BUZZER, 0, 1000));
    assert(state.displayBackend.buzzerCalls.size() == 2);
    assert(!state.displayBackend.buzzerCalls[0]);
    assert(state.displayBackend.buzzerCalls[1]);
    assert(state.buzzerNeedsOff);
    assert(state.buzzerOffAtMs == 1080);

    runBuzzerAutoOff(state, 1079);
    assert(state.displayBackend.buzzerCalls.size() == 2);
    runBuzzerAutoOff(state, 1080);
    assert(state.displayBackend.buzzerCalls.size() == 3);
    assert(!state.displayBackend.buzzerCalls[2]);
    assert(!state.buzzerNeedsOff);
}

void testBuzzerOffFailureRetriesAfter100Ms() {
    HarnessState state;
    state.displayBackend.caps.buzzer = true;
    assert(runSystemAction(state, ScreenSystemAction::BUZZER, 0, 1000));
    state.displayBackend.buzzerCalls.clear();
    state.displayBackend.buzzerResults = {false, true};

    runBuzzerAutoOff(state, 1080);
    assert(state.buzzerNeedsOff);
    assert(state.buzzerOffAtMs == 1180);
    assert(state.warningCount == 1);
    assert(state.displayBackend.buzzerCalls.size() == 1);
    runBuzzerAutoOff(state, 1179);
    assert(state.displayBackend.buzzerCalls.size() == 1);
    runBuzzerAutoOff(state, 1180);
    assert(state.displayBackend.buzzerCalls.size() == 2);
    assert(!state.buzzerNeedsOff);
}

void testBuzzerDeadlineSurvivesMillisWrap() {
    HarnessState state;
    state.displayBackend.caps.buzzer = true;
    const uint32_t started = UINT32_MAX - 39U;
    assert(runSystemAction(state, ScreenSystemAction::BUZZER, 0, started));
    assert(state.buzzerOffAtMs == 40U);
    state.displayBackend.buzzerCalls.clear();
    runBuzzerAutoOff(state, 39U);
    assert(state.displayBackend.buzzerCalls.empty());
    runBuzzerAutoOff(state, 40U);
    assert(state.displayBackend.buzzerCalls.size() == 1);
    assert(!state.buzzerNeedsOff);
}

void testFailedBuzzerOnStillSchedulesOff() {
    HarnessState state;
    state.displayBackend.caps.buzzer = true;
    state.displayBackend.buzzerResults = {true, false, true};
    assert(!runSystemAction(state, ScreenSystemAction::BUZZER, 0, 2500));
    assert(state.buzzerNeedsOff);
    assert(state.buzzerOffAtMs == 2500);
    runBuzzerAutoOff(state, 2500);
    assert(state.displayBackend.buzzerCalls.size() == 3);
    assert(!state.displayBackend.buzzerCalls[2]);
    assert(!state.buzzerNeedsOff);

    HarnessState unsupported;
    assert(!runSystemAction(unsupported, ScreenSystemAction::BUZZER, 0, 1));
    assert(unsupported.displayBackend.buzzerCalls.empty());
}

void testStopCleanupAttemptsAtMostThreeWrites() {
    delayCalls = delayedMs = 0;
    HarnessState eventuallyOff;
    eventuallyOff.displayBackend.buzzerResults = {false, false, true};
    runStopCleanup(eventuallyOff);
    assert(eventuallyOff.displayBackend.buzzerCalls.size() == 3);
    assert(delayCalls == 2 && delayedMs == 4);

    delayCalls = delayedMs = 0;
    HarnessState stillFailing;
    stillFailing.displayBackend.buzzerResults = {false, false, false, true};
    runStopCleanup(stillFailing);
    assert(stillFailing.displayBackend.buzzerCalls.size() == 3);
    assert(delayCalls == 3 && delayedMs == 6);

    delayCalls = delayedMs = 0;
    HarnessState immediate;
    runStopCleanup(immediate);
    assert(immediate.displayBackend.buzzerCalls.size() == 1);
    assert(delayCalls == 0 && delayedMs == 0);
}

int main() {
    testBrightnessAndSleepDispatch();
    testBuzzerClosesAfter80Ms();
    testBuzzerOffFailureRetriesAfter100Ms();
    testBuzzerDeadlineSurvivesMillisWrap();
    testFailedBuzzerOnStillSchedulesOff();
    testStopCleanupAttemptsAtMostThreeWrites();
    std::cout << "screen-system-actions: ok\n";
    return 0;
}
