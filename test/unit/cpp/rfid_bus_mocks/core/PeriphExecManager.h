#pragma once
#include <Arduino.h>
#include <functional>
struct PeriphExecManager {
    static inline std::function<void(const String&, const String&)> onEvent;
    static PeriphExecManager& getInstance() { static PeriphExecManager x; return x; }
    void dispatchEventMatchedRules(const String& event, const String& data) {
        if (onEvent) onEvent(event, data);
    }
};
