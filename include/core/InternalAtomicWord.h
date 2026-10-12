#pragma once
#include <atomic>
#include <cstdint>
#include <new>
#include <type_traits>

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include "ChipConfig.h"
#if !CHIP_HAS_REVIEWED_INTERNAL_ATOMIC_STORAGE
#error "Non-waiting atomic storage must be reviewed for this ESP target"
#endif
#endif

namespace FastBee {
// ESP-IDF's Xtensa atomic library uses S32C1I for internal DRAM, but an
// indefinitely waiting critical section for PSRAM. The compiler consequently
// (correctly) does not advertise all possible addresses as always lock-free.
// C3's single-core SDK uses a bounded interrupt mask; C6 uses hardware atomics.
// This narrow check covers only bool/uint32_t, not arbitrary atomic types.
template<class T>
bool atomicStorageDoesNotWait(const std::atomic<T>& value) {
    static_assert(std::is_same<T, bool>::value || std::is_same<T, uint32_t>::value,
                  "Only bool and uint32_t atomic storage was reviewed");
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    const auto* first = reinterpret_cast<const unsigned char*>(&value);
    return esp_ptr_in_dram(first) && esp_ptr_in_dram(first + sizeof(value) - 1);
#else
    static_assert(std::atomic<T>::is_always_lock_free, "Host atomic storage must not wait");
    (void)value;
    return true;
#endif
}

// Construction may allocate once if the enclosing object/stack is in PSRAM.
// Admission/release never allocate or retry allocation. Failure is permanent
// for this object's lifetime and callers MUST fail closed when get() is null.
class InternalAtomicWord {
public:
    using Storage = std::atomic<uint32_t>;
#if !defined(ARDUINO_ARCH_ESP32) && !defined(ESP_PLATFORM)
    static_assert(Storage::is_always_lock_free, "Host atomic word must not wait");
#endif
    explicit InternalAtomicWord(uint32_t initial = 0) : _local(initial), _value(&_local) {
        static_assert(sizeof(Storage) == sizeof(uint32_t), "Expected one atomic word");
        static_assert(alignof(Storage) >= alignof(uint32_t), "Atomic word alignment");
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
        if (!atomicStorageDoesNotWait(_local)) {
            void* memory = heap_caps_malloc(sizeof(Storage), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            _value = memory ? new (memory) Storage(initial) : nullptr;
            if (_value && !atomicStorageDoesNotWait(*_value)) {
                _value->~Storage();
                heap_caps_free(_value);
                _value = nullptr;
            }
        }
#endif
    }
    ~InternalAtomicWord() {
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
        if (_value && _value != &_local) {
            _value->~Storage();
            heap_caps_free(_value);
        }
#endif
    }
    InternalAtomicWord(const InternalAtomicWord&) = delete;
    InternalAtomicWord& operator=(const InternalAtomicWord&) = delete;
    InternalAtomicWord(InternalAtomicWord&&) = delete;
    InternalAtomicWord& operator=(InternalAtomicWord&&) = delete;
    Storage* get() { return _value; }
    const Storage* get() const { return _value; }
private:
    Storage _local;
    Storage* _value;
};
} // namespace FastBee
