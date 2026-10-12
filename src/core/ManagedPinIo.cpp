#include "core/ManagedPinIo.h"
#include "core/ChipConfig.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp32-hal-periman.h>
#include <cstring>
#include <new>
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif
#if CHIP_HAS_DAC
#include <driver/dac_oneshot.h>
#endif

namespace FastBee {
namespace {
bool untouchedArduinoPin(uint8_t pin) {
    // Mutation-time refusal only. Never adopt or implicitly detach an Arduino
    // driver's bus. periman is not a synchronized global ownership service.
    return perimanGetPinBusType(pin) == ESP32_BUS_TYPE_INIT;
}
bool configurePad(uint8_t pin, ManagedGpioMode mode, uint8_t interruptType) {
    gpio_config_t config{};
    config.pin_bit_mask = uint64_t(1) << pin;
    config.mode = mode == ManagedGpioMode::Output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    config.pull_up_en = mode == ManagedGpioMode::PullUp ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    config.pull_down_en = mode == ManagedGpioMode::PullDown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    config.intr_type = static_cast<gpio_int_type_t>(interruptType);
    return gpio_config(&config) == ESP_OK;
}
}
GpioResourceSnapshot::Lease* ManagedPinIo::allocateGpio() noexcept {
    using Lease = GpioResourceSnapshot::Lease;
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    void* memory = heap_caps_malloc(sizeof(Lease), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    void* memory = ::operator new(sizeof(Lease), std::nothrow);
#endif
    return memory ? new (memory) Lease{} : nullptr;
}
void ManagedPinIo::GpioDeleter::operator()(GpioResourceSnapshot::Lease* lease) const noexcept {
    if (!lease) return;
    lease->~Lease();
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    heap_caps_free(lease);
#else
    ::operator delete(lease, std::nothrow);
#endif
}
bool ManagedPinIo::identity(ResourceConsumerIdentity& out, const char* id, bool script) {
    if (!id || (script ? id[0] != '\0' : id[0] == '\0')) return false;
    return out.assign(script ? ResourceConsumerRole::Unknown : ResourceConsumerRole::Peripheral, id);
}
bool ManagedPinIo::matches(const ResourceConsumerIdentity& consumer, bool wasScript, const char* id, bool script) {
    ResourceConsumerIdentity requested;
    return wasScript == script && identity(requested, id, script) && consumer.equals(requested);
}
bool ManagedPinIo::hasGpio(uint8_t pin) const { return pin < GpioResourceSnapshot::CAPACITY && gpio_[pin] && gpio_[pin]->pin != 255; }
bool ManagedPinIo::hasDac(uint8_t pin) const {
    if (pin == 255) return false;
    for (const auto& slot : dac_) if (slot.lease.pin == pin) return true;
    return false;
}
bool ManagedPinIo::conflicts(const char* id, bool script, uint8_t pin) const {
    if (pin == 255) return false;
    if (hasGpio(pin) && !matches(gpio_[pin]->consumer, gpio_[pin]->commandScript, id, script)) return true;
    for (const auto& slot : dac_)
        if (slot.lease.pin == pin && !matches(slot.lease.consumer, slot.lease.commandScript, id, script)) return true;
    return false;
}
bool ManagedPinIo::configure(const char* id, bool script, uint8_t pin, ManagedGpioMode mode, bool high) {
    ResourceConsumerIdentity consumer;
    if (pin >= GpioResourceSnapshot::CAPACITY || mode > ManagedGpioMode::Output ||
        pin > CHIP_MAX_GPIO || !identity(consumer, id, script) || hasDac(pin) || conflicts(id, script, pin)) return false;
    if (!untouchedArduinoPin(pin)) {
        if (gpio_[pin]) gpio_[pin]->fault = true;
        return false;
    }
    if (!gpio_[pin]) {
        // Allocation failure cannot touch hardware or publish an owner.
        gpio_[pin].reset(allocateGpio());
        if (!gpio_[pin]) return false;
    }
    auto& slot = *gpio_[pin];
    if (slot.pin != 255) {
        if (slot.fault || slot.cleanupPending || slot.mode != mode) return false;
        // Repeated setup must not reset an already registered ISR. The owner
        // remains unchanged; each ensuing output write still checks IDF.
        return mode != ManagedGpioMode::Output || write(id, script, pin, high);
    }
    slot.pin = pin; slot.consumer = consumer; slot.commandScript = script; slot.mode = mode;
    slot.fault = true; // A failed SDK call need not be side-effect free.
    if (mode == ManagedGpioMode::Output && gpio_set_level(static_cast<gpio_num_t>(pin), high) != ESP_OK) return false;
    if (!configurePad(pin, mode, GPIO_INTR_DISABLE)) return false;
    slot.fault = false;
    return true;
}
bool ManagedPinIo::write(const char* id, bool script, uint8_t pin, bool high) {
    if (!hasGpio(pin) || !matches(gpio_[pin]->consumer, gpio_[pin]->commandScript, id, script)) return false;
    auto& slot = *gpio_[pin];
    if (slot.mode != ManagedGpioMode::Output || slot.fault || slot.cleanupPending) return false;
    if (!untouchedArduinoPin(pin)) { slot.fault = true; return false; }
    if (gpio_set_level(static_cast<gpio_num_t>(pin), high) != ESP_OK) { slot.fault = true; return false; }
    return true;
}
bool ManagedPinIo::read(const char* id, uint8_t pin, bool& high) {
    high = false;
    if (!hasGpio(pin) || !matches(gpio_[pin]->consumer, gpio_[pin]->commandScript, id, false)) return false;
    auto& slot = *gpio_[pin];
    if (slot.mode == ManagedGpioMode::Output || slot.fault || slot.cleanupPending) return false;
    if (!untouchedArduinoPin(pin)) { slot.fault = true; return false; }
    // IDF has no error-valued level read. Admission is based on our last
    // checked setup; the sampled pad is not evidence of wiring correctness.
    high = gpio_get_level(static_cast<gpio_num_t>(pin)) != 0;
    return true;
}
bool ManagedPinIo::interruptCurrent(const char* id, uint8_t pin) const {
    return hasGpio(pin) && matches(gpio_[pin]->consumer, gpio_[pin]->commandScript, id, false) &&
        gpio_[pin]->interruptAttached && !gpio_[pin]->fault && !gpio_[pin]->cleanupPending;
}
bool ManagedPinIo::observeGpio(uint8_t pin, GpioResourceSnapshot::Lease& out) const {
    out = GpioResourceSnapshot::Lease{};
    if (!hasGpio(pin)) return false;
    out = *gpio_[pin];
    return true;
}
bool ManagedPinIo::attach(const char* id, uint8_t pin, Isr handler, void* arg, uint8_t type) {
    if (!handler || !hasGpio(pin) || !matches(gpio_[pin]->consumer, gpio_[pin]->commandScript, id, false)) return false;
    auto& slot = *gpio_[pin];
    if (slot.fault || slot.cleanupPending || slot.mode == ManagedGpioMode::Output ||
        (type != GPIO_INTR_POSEDGE && type != GPIO_INTR_NEGEDGE && type != GPIO_INTR_ANYEDGE)) return false;
    if (!untouchedArduinoPin(pin)) { slot.fault = true; return false; }
    if (slot.interruptAttached) return slot.interruptType == type;
    slot.fault = true;
    const esp_err_t service = gpio_install_isr_service(ARDUINO_ISR_FLAG);
    if (service != ESP_OK && service != ESP_ERR_INVALID_STATE) return false;
    if (gpio_set_intr_type(static_cast<gpio_num_t>(pin), static_cast<gpio_int_type_t>(type)) != ESP_OK) return false;
    // Conservatively retain a possibly changed handler after a failed add.
    slot.interruptCleanupPending = true; slot.interruptType = type;
    if (gpio_isr_handler_add(static_cast<gpio_num_t>(pin), handler, arg) != ESP_OK) return false;
    slot.interruptAttached = true; slot.interruptCleanupPending = false;
    slot.fault = false;
    return true;
}
bool ManagedPinIo::detach(const char* id, uint8_t pin) {
    if (!hasGpio(pin)) return true;
    auto& slot = *gpio_[pin];
    if (!matches(slot.consumer, slot.commandScript, id, false)) return false;
    if (!untouchedArduinoPin(pin)) { slot.fault = true; slot.cleanupPending = true; return false; }
    if (!slot.interruptAttached && !slot.interruptCleanupPending && !slot.cleanupPending) return true;
    slot.cleanupPending = true;
    if (gpio_intr_disable(static_cast<gpio_num_t>(pin)) != ESP_OK) { slot.fault = true; return false; }
    if (slot.interruptAttached || slot.interruptCleanupPending) {
        if (gpio_isr_handler_remove(static_cast<gpio_num_t>(pin)) != ESP_OK) { slot.fault = true; return false; }
        slot.interruptAttached = false; slot.interruptCleanupPending = false;
    }
    if (gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_DISABLE) != ESP_OK) { slot.fault = true; return false; }
    slot.interruptAttached = false; slot.interruptCleanupPending = false;
    slot.interruptType = GPIO_INTR_DISABLE; slot.cleanupPending = false;
    return true;
}
bool ManagedPinIo::release(const char* id, bool script, uint8_t pin, bool inactiveHigh) {
    if (!hasGpio(pin)) return true;
    auto& slot = *gpio_[pin];
    if (!matches(slot.consumer, slot.commandScript, id, script)) return false;
    if (!untouchedArduinoPin(pin)) { slot.fault = true; slot.cleanupPending = true; return false; }
    slot.cleanupPending = true;
    if ((slot.interruptAttached || slot.interruptCleanupPending) && !detach(id, pin)) return false;
    slot.cleanupPending = true;
    // RF active-low requires HIGH before releasing the pad. Preserve the
    // historical LOW default for existing digital/script callers.
    if (slot.mode == ManagedGpioMode::Output && gpio_set_level(static_cast<gpio_num_t>(pin), inactiveHigh) != ESP_OK) { slot.fault = true; return false; }
    if (!configurePad(pin, ManagedGpioMode::Input, GPIO_INTR_DISABLE)) { slot.fault = true; return false; }
    // detach() alone keeps a valid GPIO lease. Only complete hardware release
    // permits reclamation; no callback or diagnostic retains this address.
    gpio_[pin].reset();
    return true;
}
bool ManagedPinIo::writeDac(const char* id, bool script, uint8_t pin, uint8_t value) {
#if CHIP_HAS_DAC
    ResourceConsumerIdentity consumer;
    if ((pin != 25 && pin != 26) || !identity(consumer, id, script) || hasGpio(pin) ||
        conflicts(id, script, pin)) return false;
    auto& slot = dac_[pin - 25];
    if (!untouchedArduinoPin(pin)) {
        if (slot.lease.pin != 255) slot.lease.fault = true;
        return false;
    }
    if (slot.lease.pin != 255 && (slot.lease.fault || slot.lease.cleanupPending)) return false;
    if (!slot.handle) {
        slot.lease.pin = pin; slot.lease.consumer = consumer; slot.lease.commandScript = script;
        slot.lease.fault = true;
        dac_oneshot_config_t config{}; config.chan_id = pin == 25 ? DAC_CHAN_0 : DAC_CHAN_1;
        dac_oneshot_handle_t handle = nullptr;
        const auto result = dac_oneshot_new_channel(&config, &handle);
        slot.handle = handle;
        if (result != ESP_OK || !handle) return false;
    }
    if (dac_oneshot_output_voltage(static_cast<dac_oneshot_handle_t>(slot.handle), value) != ESP_OK) {
        slot.lease.fault = true; return false;
    }
    slot.lease.fault = false;
    return true;
#else
    (void)id; (void)script; (void)pin; (void)value;
    return false;
#endif
}
bool ManagedPinIo::releaseDac(const char* id, bool script, uint8_t pin) {
    if (pin == 255) return true;
    for (auto& slot : dac_) {
        if (slot.lease.pin != pin) continue;
        if (!matches(slot.lease.consumer, slot.lease.commandScript, id, script)) return false;
        if (!untouchedArduinoPin(pin)) { slot.lease.fault = true; slot.lease.cleanupPending = true; return false; }
        slot.lease.cleanupPending = true;
#if CHIP_HAS_DAC
        if (slot.handle && (dac_oneshot_output_voltage(static_cast<dac_oneshot_handle_t>(slot.handle), 0) != ESP_OK ||
                           dac_oneshot_del_channel(static_cast<dac_oneshot_handle_t>(slot.handle)) != ESP_OK)) {
            slot.lease.fault = true; return false;
        }
        slot = DacSlot{};
        return true;
#else
        return false;
#endif
    }
    return true;
}
void ManagedPinIo::snapshot(GpioResourceSnapshot& out) const {
    out = GpioResourceSnapshot{};
    for (const auto& slot : gpio_) if (slot && slot->pin != 255) out.leases[out.count++] = *slot;
}
void ManagedPinIo::snapshot(DacResourceSnapshot& out) const {
    out = DacResourceSnapshot{};
    out.supported = CHIP_HAS_DAC != 0;
    for (const auto& slot : dac_) if (slot.lease.pin != 255) out.leases[out.count++] = slot.lease;
}
} // namespace FastBee
