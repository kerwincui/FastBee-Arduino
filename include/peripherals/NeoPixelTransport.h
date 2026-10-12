#pragma once
#include <stdint.h>
#include <stdbool.h>

// ABI implemented by the version/digest-checked Adafruit 1.15.5 IDF5 patch.
// OK confirms the SDK transmission completed; it does not prove physical LEDs.
typedef enum {
    FASTBEE_NEO_OK = 0,
    FASTBEE_NEO_BUSY,
    FASTBEE_NEO_NO_MEMORY,
    FASTBEE_NEO_INVALID,
    FASTBEE_NEO_QUARANTINED,
    FASTBEE_NEO_INIT_FAILED,
    FASTBEE_NEO_WRITE_FAILED,
    FASTBEE_NEO_RELEASE_FAILED,
    FASTBEE_NEO_OWNER_CHANGED
} FastBeeNeoPixelResult;

#ifdef __cplusplus
extern "C" {
#endif
FastBeeNeoPixelResult fastbeeNeoPixelWrite(uint8_t pin, const uint8_t* pixels,
                                         uint32_t numBytes, bool is800KHz);
FastBeeNeoPixelResult fastbeeNeoPixelRelease(uint8_t pin);
// Passive allocation-free quarantine reservation; never enters the SDK.
bool fastbeeNeoPixelPinRetained(uint8_t pin);
// Project-level failed teardown also retains a pin when no SDK write started.
bool fastbeeNeoPixelProjectPinRetained(uint8_t pin);
#ifdef __cplusplus
}
#endif
