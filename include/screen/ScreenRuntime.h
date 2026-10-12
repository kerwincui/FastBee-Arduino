#pragma once

#include <Arduino.h>
#include "core/FeatureInstanceRegistry.h"
#include "core/RuntimeResourceSnapshot.h"

namespace FastBee {
namespace Screen {

enum class ScreenSystemAction : uint8_t {
    NONE = 0,
    SET_BRIGHTNESS,
    SLEEP,
    BUZZER
};

// Snapshot of actions that can currently be invoked from the physical screen.
// A compiled driver is not enough: the display task and touch wake path must
// both be operational before these actions are advertised to panel editors.
struct ScreenSystemActionCapabilities {
    bool displayReady = false;
    bool touchConfigured = false;
    bool touchReady = false;
    char touchError[64] = {};
    bool brightness = false;
    bool sleep = false;
    bool buzzer = false;
    bool restart = false;
    bool brightnessContinuous = false;
    uint8_t minVisibleBrightnessPercent = 10;
    bool touchWake = false;
};

// Owns the single screen task. All display, touch and renderer calls are made
// from that task; framework/network callers only control its lifecycle.
class ScreenRuntime {
public:
    static ScreenRuntime& instance();
    FeatureInstanceSnapshot observeFeatureInstance() const;
    static void snapshotBacklightResources(BacklightResourceSnapshot& result);

    bool start(String& error);
    // Requests task-owned renderer/backend shutdown and waits for completion.
    // A false result means callers must leave ScreenDevice configured and
    // active because the UI task may still be using it.
    bool stop(String* error = nullptr);
    void requestRefresh();
    // Queues a fixed, validated panel action for execution by the screen task.
    // This keeps display I/O off network and manager tasks.
    bool requestSystemAction(ScreenSystemAction action, uint8_t value,
                             String& error);
    ScreenSystemActionCapabilities systemActionCapabilities() const;

    bool running() const;
    bool sleeping() const;
    size_t allocatedBytes() const;
    void getLastError(String& out) const;

private:
    ScreenRuntime() = default;
    ScreenRuntime(const ScreenRuntime&) = delete;
    ScreenRuntime& operator=(const ScreenRuntime&) = delete;
};

}  // namespace Screen
}  // namespace FastBee
