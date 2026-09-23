/* The SDK RGB object is replaced only in the explicitly prepared S3 build.
 * CONFIG_* is local to this translation unit; no installed SDK is changed.
 */
#include "core/ChipConfig.h"
#include "screen/RgbRecovery.h"
#ifndef FASTBEE_RGB_ISR_IRAM_OVERRIDE
#define FASTBEE_RGB_ISR_IRAM_OVERRIDE 0
#endif
#ifndef FASTBEE_RGB_RESTART_ON_DEMAND
#define FASTBEE_RGB_RESTART_ON_DEMAND 0
#endif
/* FeatureFlags.h also declares C++ JSON helpers, so keep its panel default
 * here without including C++ declarations in the official C driver. */
#ifndef FASTBEE_ENABLE_SCREEN_UI
#ifdef FASTBEE_ENABLE_SCREEN_PANEL
#define FASTBEE_ENABLE_SCREEN_UI FASTBEE_ENABLE_SCREEN_PANEL
#else
#define FASTBEE_ENABLE_SCREEN_UI 1
#endif
#endif

#if (FASTBEE_RGB_ISR_IRAM_OVERRIDE == 1 || FASTBEE_RGB_RESTART_ON_DEMAND == 1) && \
    FASTBEE_ENABLE_SCREEN_UI == 1 && CHIP_IS_ESP32S3 && \
    defined(FASTBEE_USE_PSRAM) && FASTBEE_USE_PSRAM == 1
#if FASTBEE_RGB_ISR_IRAM_OVERRIDE == 1
#undef CONFIG_LCD_RGB_ISR_IRAM_SAFE
#define CONFIG_LCD_RGB_ISR_IRAM_SAFE 1
#endif
#if FASTBEE_RGB_RESTART_ON_DEMAND == 1
/* Keep the SDK interrupt/cache behavior. Only stop the unconditional VBlank
 * restart; explicit esp_lcd_rgb_panel_restart() requests remain supported. */
#undef CONFIG_LCD_RGB_RESTART_IN_VSYNC
#define CONFIG_LCD_RGB_RESTART_IN_VSYNC 0
#endif

/* Include the declarations before redirecting this driver's calls, so the
 * installed SDK and every other GDMA user remain unchanged. */
#include "esp_private/gdma.h"
#include "esp_attr.h"
#include "esp_lcd_panel_rgb.h"
#include "hal/gdma_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

/* FASTBEE_RGB_RECOVERY_STATE_BEGIN */
#define FASTBEE_RGB_RECOVERY_GRACE_MS 2000U
#define FASTBEE_RGB_RECOVERY_INTERVAL_MS 1000U
#define FASTBEE_RGB_UDF_MASK \
    (GDMA_LL_EVENT_TX_L1_FIFO_UDF | GDMA_LL_EVENT_TX_L3_FIFO_UDF)

typedef struct {
    esp_lcd_panel_handle_t panel;
    gdma_channel_handle_t dma;
    gdma_dev_t* hardware;
    int channel;
    FastBeeRgbRecoveryStats stats;
    uint32_t attachedAt;
    uint32_t pendingReasons;
    uint32_t inFlightReasons;
    bool startupQueued;
    bool suppressionRecorded;
    bool resetSeen;
    bool resetSucceeded;
    bool manualForcePending;
} FastBeeRgbRecoveryState;

/* S3 has one RGB controller. This small state stays in internal DRAM and does
 * not allocate a framebuffer, bounce buffer, task, or timer. */
static FastBeeRgbRecoveryState s_rgb_recovery;
static portMUX_TYPE s_rgb_recovery_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t fastbee_rgb_gdma_reset(gdma_channel_handle_t dma);
static esp_err_t fastbee_rgb_gdma_start(gdma_channel_handle_t dma, intptr_t descriptor);
/* FASTBEE_RGB_RECOVERY_STATE_END */

#define gdma_reset fastbee_rgb_gdma_reset
#define gdma_start fastbee_rgb_gdma_start
#include "../../vendor/esp-idf-rgb-735507283d/esp_lcd_panel_rgb.c"
#undef gdma_reset
#undef gdma_start

/* FASTBEE_RGB_RECOVERY_IMPLEMENTATION_BEGIN */
static bool IRAM_ATTR fastbee_rgb_recovery_vsync(
    esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t* event,
    void* context)
{
    (void)event;
    (void)context;
    portENTER_CRITICAL_ISR(&s_rgb_recovery_lock);
    if (s_rgb_recovery.stats.active && s_rgb_recovery.panel == panel) {
        ++s_rgb_recovery.stats.vsyncs;
        const uint32_t raw = gdma_ll_tx_get_interrupt_status(
            s_rgb_recovery.hardware, s_rgb_recovery.channel, true) & FASTBEE_RGB_UDF_MASK;
        if (raw) {
            /* Observe raw bits without enabling new interrupts. Clear only our
             * sampled underflow bits; GDMA still owns EOF/error handling. */
            gdma_ll_tx_clear_interrupt_status(s_rgb_recovery.hardware,
                                              s_rgb_recovery.channel, raw);
            ++s_rgb_recovery.stats.udfEvents;
            if (raw & GDMA_LL_EVENT_TX_L1_FIFO_UDF) ++s_rgb_recovery.stats.l1;
            if (raw & GDMA_LL_EVENT_TX_L3_FIFO_UDF) ++s_rgb_recovery.stats.l3;
            s_rgb_recovery.pendingReasons |= FASTBEE_RGB_RECOVERY_UNDERFLOW;
        }
    }
    portEXIT_CRITICAL_ISR(&s_rgb_recovery_lock);
    return false;
}

static esp_err_t IRAM_ATTR fastbee_rgb_gdma_reset(gdma_channel_handle_t dma)
{
    const esp_err_t result = gdma_reset(dma);
    portENTER_CRITICAL_SAFE(&s_rgb_recovery_lock);
    if (s_rgb_recovery.stats.active && s_rgb_recovery.dma == dma &&
        s_rgb_recovery.stats.inFlight) {
        s_rgb_recovery.resetSeen = true;
        s_rgb_recovery.resetSucceeded = result == ESP_OK;
        if (result != ESP_OK) ++s_rgb_recovery.stats.resetErrors;
    }
    portEXIT_CRITICAL_SAFE(&s_rgb_recovery_lock);
    return result;
}

static esp_err_t IRAM_ATTR fastbee_rgb_gdma_start(gdma_channel_handle_t dma,
                                                intptr_t descriptor)
{
    portENTER_CRITICAL_SAFE(&s_rgb_recovery_lock);
    if (s_rgb_recovery.stats.active && s_rgb_recovery.dma == dma &&
        s_rgb_recovery.stats.inFlight && s_rgb_recovery.resetSeen) {
        /* Clear reset-related flags before starting the new transmission.
         * Real underflows after this start remain observable at VSYNC. */
        gdma_ll_tx_clear_interrupt_status(s_rgb_recovery.hardware,
                                          s_rgb_recovery.channel, FASTBEE_RGB_UDF_MASK);
    }
    portEXIT_CRITICAL_SAFE(&s_rgb_recovery_lock);
    const esp_err_t result = gdma_start(dma, descriptor);
    portENTER_CRITICAL_SAFE(&s_rgb_recovery_lock);
    if (s_rgb_recovery.stats.active && s_rgb_recovery.dma == dma &&
        s_rgb_recovery.stats.inFlight && s_rgb_recovery.resetSeen) {
        if (result != ESP_OK) ++s_rgb_recovery.stats.startErrors;
        if (s_rgb_recovery.resetSucceeded && result == ESP_OK) {
            ++s_rgb_recovery.stats.applied;
            s_rgb_recovery.stats.lastReason = s_rgb_recovery.inFlightReasons;
            s_rgb_recovery.stats.lastAppliedVsync = s_rgb_recovery.stats.vsyncs;
            /* This reset also covers UDF observed between the request and this
             * VSYNC. Preserve later manual requests, but do not replay an old
             * hardware event after it has already been recovered. */
            s_rgb_recovery.pendingReasons &= ~FASTBEE_RGB_RECOVERY_UNDERFLOW;
        } else {
            s_rgb_recovery.pendingReasons |= s_rgb_recovery.inFlightReasons;
        }
        s_rgb_recovery.stats.inFlight = false;
        s_rgb_recovery.inFlightReasons = 0;
        s_rgb_recovery.resetSeen = false;
        s_rgb_recovery.suppressionRecorded = false;
    }
    portEXIT_CRITICAL_SAFE(&s_rgb_recovery_lock);
    return result;
}

bool fastbee_rgb_recovery_attach(void* panel, uint32_t nowMs)
{
    if (!panel) return false;
    esp_rgb_panel_t* rgb = __containerof((esp_lcd_panel_handle_t)panel,
                                       esp_rgb_panel_t, base);
    /* The vendor already detects missed bounce refills. This added recovery
     * applies only to the continuous direct-PSRAM transport. */
    if (rgb->bb_size || !rgb->flags.stream_mode) return true;
    int group = -1;
    int channel = -1;
    if (gdma_get_group_channel_id(rgb->dma_chan, &group, &channel) != ESP_OK ||
        group != 0 || channel < 0 || channel >= GDMA_LL_AHB_PAIRS_PER_GROUP) return false;
    portENTER_CRITICAL(&s_rgb_recovery_lock);
    if (s_rgb_recovery.stats.active) {
        portEXIT_CRITICAL(&s_rgb_recovery_lock);
        return false;
    }
    memset(&s_rgb_recovery, 0, sizeof(s_rgb_recovery));
    s_rgb_recovery.panel = (esp_lcd_panel_handle_t)panel;
    s_rgb_recovery.dma = rgb->dma_chan;
    s_rgb_recovery.hardware = GDMA_LL_GET_HW(group);
    s_rgb_recovery.channel = channel;
    s_rgb_recovery.attachedAt = nowMs;
    s_rgb_recovery.stats.active = true;
    gdma_ll_tx_clear_interrupt_status(s_rgb_recovery.hardware, channel,
                                      FASTBEE_RGB_UDF_MASK);
    portEXIT_CRITICAL(&s_rgb_recovery_lock);
    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_vsync = fastbee_rgb_recovery_vsync,
    };
    if (esp_lcd_rgb_panel_register_event_callbacks(
            (esp_lcd_panel_handle_t)panel, &callbacks, NULL) != ESP_OK) {
        fastbee_rgb_recovery_detach(panel);
        return false;
    }
    return true;
}

void fastbee_rgb_recovery_detach(void* panel)
{
    bool wasAttached = false;
    portENTER_CRITICAL(&s_rgb_recovery_lock);
    if (panel && s_rgb_recovery.panel == panel && s_rgb_recovery.stats.active) {
        s_rgb_recovery.stats.active = false;
        s_rgb_recovery.stats.inFlight = false;
        s_rgb_recovery.panel = NULL;
        s_rgb_recovery.dma = NULL;
        s_rgb_recovery.pendingReasons = 0;
        wasAttached = true;
    }
    portEXIT_CRITICAL(&s_rgb_recovery_lock);
    if (wasAttached) {
        const esp_lcd_rgb_panel_event_callbacks_t callbacks = {0};
        esp_lcd_rgb_panel_register_event_callbacks(
            (esp_lcd_panel_handle_t)panel, &callbacks, NULL);
    }
}

bool fastbee_rgb_recovery_request(void)
{
    portENTER_CRITICAL(&s_rgb_recovery_lock);
    const bool active = s_rgb_recovery.stats.active;
    if (active) {
        s_rgb_recovery.pendingReasons |= FASTBEE_RGB_RECOVERY_MANUAL;
        s_rgb_recovery.manualForcePending = true;
    }
    portEXIT_CRITICAL(&s_rgb_recovery_lock);
    return active;
}

void fastbee_rgb_recovery_service(uint32_t nowMs)
{
    esp_lcd_panel_handle_t panel = NULL;
    portENTER_CRITICAL(&s_rgb_recovery_lock);
    if (s_rgb_recovery.stats.active) {
        const bool settled = (uint32_t)(nowMs - s_rgb_recovery.attachedAt) >=
                             FASTBEE_RGB_RECOVERY_GRACE_MS;
        if (settled && !s_rgb_recovery.startupQueued) {
            /* One counted alignment after startup, not a periodic restart. */
            s_rgb_recovery.startupQueued = true;
            s_rgb_recovery.pendingReasons |= FASTBEE_RGB_RECOVERY_STARTUP;
        }
        const bool manual = s_rgb_recovery.manualForcePending;
        if (s_rgb_recovery.pendingReasons && !s_rgb_recovery.stats.inFlight && (settled || manual)) {
            const bool coolingDown = s_rgb_recovery.stats.requested &&
                (uint32_t)(nowMs - s_rgb_recovery.stats.lastRequestMs) < FASTBEE_RGB_RECOVERY_INTERVAL_MS;
            if (coolingDown && !manual) {
                if (!s_rgb_recovery.suppressionRecorded) {
                    ++s_rgb_recovery.stats.suppressed;
                    s_rgb_recovery.suppressionRecorded = true;
                }
            } else {
                panel = s_rgb_recovery.panel;
                s_rgb_recovery.inFlightReasons = s_rgb_recovery.pendingReasons;
                s_rgb_recovery.pendingReasons = 0;
                s_rgb_recovery.manualForcePending = false;
                s_rgb_recovery.stats.inFlight = true;
                s_rgb_recovery.resetSeen = false;
                s_rgb_recovery.suppressionRecorded = false;
                s_rgb_recovery.stats.lastRequestMs = nowMs;
                ++s_rgb_recovery.stats.requested;
                if (s_rgb_recovery.inFlightReasons & FASTBEE_RGB_RECOVERY_STARTUP)
                    ++s_rgb_recovery.stats.startupRequests;
            }
        }
    }
    portEXIT_CRITICAL(&s_rgb_recovery_lock);
    if (panel) {
        /* Public restart queues work for the next VSYNC. Only the wrapped
         * GDMA reset/start pair above records an applied recovery. */
        const esp_err_t result = esp_lcd_rgb_panel_restart(panel);
        if (result != ESP_OK) {
            portENTER_CRITICAL(&s_rgb_recovery_lock);
            ++s_rgb_recovery.stats.requestErrors;
            s_rgb_recovery.pendingReasons |= s_rgb_recovery.inFlightReasons;
            s_rgb_recovery.inFlightReasons = 0;
            s_rgb_recovery.stats.inFlight = false;
            portEXIT_CRITICAL(&s_rgb_recovery_lock);
        }
    }
}

bool fastbee_rgb_recovery_get_stats(FastBeeRgbRecoveryStats* output)
{
    if (!output) return false;
    portENTER_CRITICAL(&s_rgb_recovery_lock);
    *output = s_rgb_recovery.stats;
    output->pendingReasons = s_rgb_recovery.pendingReasons;
    output->pending = s_rgb_recovery.pendingReasons != 0;
    portEXIT_CRITICAL(&s_rgb_recovery_lock);
    return output->active;
}
/* FASTBEE_RGB_RECOVERY_IMPLEMENTATION_END */
#endif
