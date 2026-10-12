#pragma once
#include "core/FeatureOwnerLifetime.h"

namespace FastBee {

// Counts actual owned TLS transport allocations, including retained cellular
// transports. It does not identify the currently selected connection or claim
// a successful handshake, certificate validation, or measured memory release.
using TlsTransportOwners = BasicFeatureOwnerGroup<0x00ffffffu, UINT32_MAX, 8>;
inline TlsTransportOwners& tlsTransportOwners() {
    static TlsTransportOwners owners;
    static const FeatureInstanceObserver observer{"mqtts", [](const void* context) {
        auto value = static_cast<const TlsTransportOwners*>(context)->snapshot();
        value.scope = FeatureInstanceScope::TransportAllocation;
        if (value.state == FeatureInstanceState::OwnerAbsent) value.state = FeatureInstanceState::Stopped;
        return value;
    }, &owners};
    FeatureInstanceRegistry::instance().registerObserver(&observer);
    return owners;
}

class TlsTransportObservation {
public:
    TlsTransportObservation() : owners_(tlsTransportOwners()) {}
    // A lost owner without an explicit post-delete release is unknown forever:
    // manager destruction cannot erase evidence of a retained/leaked transport.
    ~TlsTransportObservation() { if (tracked_) owners_.invalidate(); }
    void allocating() {
        if (tracked_) { owners_.invalidate(); return; }
        tracked_ = owners_.acquire(slot_, identity_);
    }
    void allocated() { if (tracked_) owners_.publish(slot_, identity_, FeatureInstanceState::Allocated); }
    void retiring() { if (tracked_) owners_.publish(slot_, identity_, FeatureInstanceState::Retiring); }
    void released() {
        if (tracked_) { owners_.release(slot_, identity_); tracked_ = false; }
    }
    TlsTransportObservation(const TlsTransportObservation&) = delete;
    TlsTransportObservation& operator=(const TlsTransportObservation&) = delete;
private:
    TlsTransportOwners& owners_;
    uint32_t identity_ = 0;
    uint8_t slot_ = 0;
    bool tracked_ = false;
};
} // namespace FastBee
