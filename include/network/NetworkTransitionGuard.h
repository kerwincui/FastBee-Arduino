#pragma once
#include "protocols/MqttClientLease.h"
#include <functional>

namespace FastBee {
// An admitted dependent protocol returns its exclusive lifetime lease, held
// until ALL adapter destruction/creation and fallback paths have finished.
// A void notification or a momentary stop is insufficient: status/API callers
// could rebuild the client while an adapter is being destroyed.
using NetworkProtocolAdmission = std::function<MqttClientLifetime::Write()>;

class NetworkTransitionGuard {
    MqttClientLifetime::Write _network;
    MqttClientLifetime::Write _protocol;
    bool _ready = false;
    bool _storageUnavailable = false;
public:
    NetworkTransitionGuard(MqttClientLifetime& network,
                           const NetworkProtocolAdmission& admission)
        : _network(network) {
        _storageUnavailable = _network.storageUnavailable();
        if (!_network) return;
        if (admission) {
            _protocol = admission();
            _storageUnavailable = _protocol.storageUnavailable();
            if (!_protocol) return;
        }
        // No callback is normal during initial boot / MQTT-disabled builds.
        _ready = true;
    }
    NetworkTransitionGuard(const NetworkTransitionGuard&) = delete;
    NetworkTransitionGuard& operator=(const NetworkTransitionGuard&) = delete;
    explicit operator bool() const { return _ready; }
    bool storageUnavailable() const { return _storageUnavailable; }
};
} // namespace FastBee
