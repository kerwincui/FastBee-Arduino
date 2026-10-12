#pragma once
#include "RuntimeResourceSnapshot.h"
#include <memory>

namespace FastBee {
enum class ManagedGpioMode : uint8_t { Input, PullUp, PullDown, Output };
struct GpioResourceSnapshot {
    static constexpr size_t CAPACITY = 49; // Existing largest target: ESP32-S3 GPIO 0..48.
    ResourceObservation status = ResourceObservation::Observed;
    uint8_t count = 0;
    struct Lease {
        uint8_t pin = 255;
        ResourceConsumerIdentity consumer{};
        ManagedGpioMode mode = ManagedGpioMode::Input;
        uint8_t interruptType = 0;
        bool commandScript = false, interruptAttached = false;
        bool interruptCleanupPending = false;
        bool fault = false, cleanupPending = false;
    } leases[CAPACITY]{};
};
struct DacResourceSnapshot {
    static constexpr size_t CAPACITY = 2;
    ResourceObservation status = ResourceObservation::Observed;
    bool supported = false;
    uint8_t count = 0;
    struct Lease {
        uint8_t pin = 255;
        ResourceConsumerIdentity consumer{};
        bool commandScript = false, fault = false, cleanupPending = false;
    } leases[CAPACITY]{};
};

// Only PM holds this permanent state, under its existing recursive mutex.
// IDF acknowledgements describe our last operation, not exclusive global pad
// ownership or physical wiring. No observation calls SDK/Arduino/periman.
class ManagedPinIo {
public:
    using Isr = void (*)(void*);
    bool configure(const char* id, bool script, uint8_t pin, ManagedGpioMode mode, bool initialHigh);
    bool write(const char* id, bool script, uint8_t pin, bool high);
    bool read(const char* id, uint8_t pin, bool& high);
    bool attach(const char* id, uint8_t pin, Isr handler, void* arg, uint8_t type);
    bool detach(const char* id, uint8_t pin);
    bool release(const char* id, bool script, uint8_t pin, bool inactiveHigh = false);
    bool writeDac(const char* id, bool script, uint8_t pin, uint8_t value);
    bool releaseDac(const char* id, bool script, uint8_t pin);
    bool conflicts(const char* id, bool script, uint8_t pin) const;
    bool hasGpio(uint8_t pin) const;
    bool hasDac(uint8_t pin) const;
    bool interruptCurrent(const char* id, uint8_t pin) const;
    bool observeGpio(uint8_t pin, GpioResourceSnapshot::Lease& out) const;
    void snapshot(GpioResourceSnapshot& out) const;
    void snapshot(DacResourceSnapshot& out) const;
private:
    struct DacSlot { DacResourceSnapshot::Lease lease{}; void* handle = nullptr; };
    struct GpioDeleter {
        void operator()(GpioResourceSnapshot::Lease* lease) const noexcept;
    };
    // PM's mutex protects allocation and release. ISR arguments are pin values
    // or separate encoder generation tokens, never these lease addresses.
    // Faulted leases remain allocated until checked full release succeeds.
    std::unique_ptr<GpioResourceSnapshot::Lease, GpioDeleter> gpio_[GpioResourceSnapshot::CAPACITY]{};
    DacSlot dac_[DacResourceSnapshot::CAPACITY]{};
    static GpioResourceSnapshot::Lease* allocateGpio() noexcept;
    static bool identity(ResourceConsumerIdentity& out, const char* id, bool script);
    static bool matches(const ResourceConsumerIdentity& consumer, bool wasScript, const char* id, bool script);
};
} // namespace FastBee
