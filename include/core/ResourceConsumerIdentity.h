#pragma once
#include <stddef.h>
#include <stdint.h>

namespace FastBee {
// Labels supplied by the actual acquiring consumer. They are diagnostics, not
// authorization, and never reconstructed from a later configuration snapshot.
enum class ResourceConsumerRole : uint8_t {
    Unknown, Peripheral, Sensor, Display, Touch, SdCard, Ethernet, Modbus,
    Cellular, McpReset, PcaGate, DhtCapture
};
struct ResourceConsumerIdentity {
    static constexpr size_t ID_CAPACITY = 41;
    ResourceConsumerRole role = ResourceConsumerRole::Unknown;
    char id[ID_CAPACITY]{};

    bool specified() const { return role != ResourceConsumerRole::Unknown || id[0]; }
    bool equals(const ResourceConsumerIdentity& other) const {
        if (role != other.role) return false;
        for (size_t i = 0; i < ID_CAPACITY; ++i) if (id[i] != other.id[i]) return false;
        return true;
    }

    bool assign(ResourceConsumerRole value, const char* text = nullptr) {
        *this = ResourceConsumerIdentity{};
        if (value > ResourceConsumerRole::DhtCapture) return false;
        size_t length = 0;
        if (text) {
            while (length < ID_CAPACITY && text[length]) ++length;
            if (length == ID_CAPACITY) return false;
            for (size_t i = 0; i < length; ++i) id[i] = text[i];
        }
        role = value;
        return true;
    }
};
} // namespace FastBee
