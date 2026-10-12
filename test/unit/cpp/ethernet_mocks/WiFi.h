#pragma once
#include <map>
#include <vector>
using wifi_event_id_t = unsigned;
enum arduino_event_id_t { ARDUINO_EVENT_ETH_START, ARDUINO_EVENT_ETH_CONNECTED,
                         ARDUINO_EVENT_ETH_GOT_IP, ARDUINO_EVENT_ETH_DISCONNECTED };
struct arduino_event_info_t {};
using EthCallback = void (*)(arduino_event_id_t, arduino_event_info_t);
struct MockWiFi {
    bool failRegister = false;
    unsigned nextId = 1, registrations = 0, removals = 0;
    std::map<wifi_event_id_t, EthCallback> callbacks;
    std::vector<EthCallback> queued;
    wifi_event_id_t onEvent(EthCallback callback) {
        ++registrations;
        if (failRegister) return 0;
        queued.push_back(callback); callbacks[nextId] = callback; return nextId++;
    }
    void removeEvent(wifi_event_id_t id) { ++removals; callbacks.erase(id); }
    void deliverQueued() { for (auto callback : queued) callback(ARDUINO_EVENT_ETH_GOT_IP, {}); queued.clear(); }
};
inline MockWiFi WiFi;
