#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "core/ChipConfig.h"

#if (defined(FASTBEE_ENABLE_SCREEN_UI) && FASTBEE_ENABLE_SCREEN_UI == 1) || \
    (!defined(FASTBEE_ENABLE_SCREEN_UI) && \
     (!defined(FASTBEE_ENABLE_SCREEN_PANEL) || FASTBEE_ENABLE_SCREEN_PANEL == 1))
#define FASTBEE_RGB_RECOVERY_SCREEN_ENABLED 1
#else
#define FASTBEE_RGB_RECOVERY_SCREEN_ENABLED 0
#endif

#if ((defined(FASTBEE_RGB_RESTART_ON_DEMAND) && FASTBEE_RGB_RESTART_ON_DEMAND == 1) || \
     (defined(FASTBEE_RGB_ISR_IRAM_OVERRIDE) && FASTBEE_RGB_ISR_IRAM_OVERRIDE == 1)) && \
    FASTBEE_RGB_RECOVERY_SCREEN_ENABLED && CHIP_IS_ESP32S3 && \
    defined(FASTBEE_USE_PSRAM) && FASTBEE_USE_PSRAM == 1
#define FASTBEE_RGB_RECOVERY_AVAILABLE 1
#else
#define FASTBEE_RGB_RECOVERY_AVAILABLE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    FASTBEE_RGB_RECOVERY_STARTUP = 1U,
    FASTBEE_RGB_RECOVERY_UNDERFLOW = 2U,
    FASTBEE_RGB_RECOVERY_MANUAL = 4U
};

typedef struct {
    uint32_t vsyncs;
    uint32_t udfEvents;
    uint32_t l1;
    uint32_t l3;
    uint32_t requested;
    uint32_t applied;
    uint32_t requestErrors;
    uint32_t resetErrors;
    uint32_t startErrors;
    uint32_t suppressed;
    uint32_t startupRequests;
    uint32_t lastReason;
    uint32_t lastRequestMs;
    uint32_t lastAppliedVsync;
    uint32_t pendingReasons;
    bool active;
    bool pending;
    bool inFlight;
} FastBeeRgbRecoveryStats;

/* Attach/detach/service belong to the display-owning task. The getter may be
 * called by another task. Unsupported builds keep these APIs as no-op stubs.
 * A successful request is not proof that the following VSYNC applied it. */
#if FASTBEE_RGB_RECOVERY_AVAILABLE
bool fastbee_rgb_recovery_attach(void* panel, uint32_t nowMs);
void fastbee_rgb_recovery_detach(void* panel);
void fastbee_rgb_recovery_service(uint32_t nowMs);
bool fastbee_rgb_recovery_request(void);
bool fastbee_rgb_recovery_get_stats(FastBeeRgbRecoveryStats* output);
#else
static inline bool fastbee_rgb_recovery_attach(void* panel, uint32_t nowMs)
{ (void)panel; (void)nowMs; return true; }
static inline void fastbee_rgb_recovery_detach(void* panel) { (void)panel; }
static inline void fastbee_rgb_recovery_service(uint32_t nowMs) { (void)nowMs; }
static inline bool fastbee_rgb_recovery_request(void) { return false; }
static inline bool fastbee_rgb_recovery_get_stats(FastBeeRgbRecoveryStats* output)
{
#ifdef __cplusplus
    const FastBeeRgbRecoveryStats empty{};
#else
    const FastBeeRgbRecoveryStats empty = {0};
#endif
    if (output) *output = empty;
    return false;
}
#endif

#ifdef __cplusplus
}
#endif
