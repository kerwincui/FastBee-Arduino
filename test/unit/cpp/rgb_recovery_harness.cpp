#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include "screen/RgbRecovery.h"

#define IRAM_ATTR
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portENTER_CRITICAL_ISR(x) ((void)(x))
#define portEXIT_CRITICAL_ISR(x) ((void)(x))
#define portENTER_CRITICAL_SAFE(x) ((void)(x))
#define portEXIT_CRITICAL_SAFE(x) ((void)(x))
constexpr uint32_t GDMA_LL_EVENT_TX_L1_FIFO_UDF = 1U << 5;
constexpr uint32_t GDMA_LL_EVENT_TX_L3_FIFO_UDF = 1U << 7;
constexpr int GDMA_LL_AHB_PAIRS_PER_GROUP = 5;
using gdma_channel_handle_t = void*;
struct gdma_dev_t { uint32_t raw = 0; } hardware;
#define GDMA_LL_GET_HW(id) ((id) == 0 ? &hardware : nullptr)
struct esp_lcd_panel_t { bool needRestart = false; };
using esp_lcd_panel_handle_t = esp_lcd_panel_t*;
struct esp_rgb_panel_t {
    esp_lcd_panel_t base;
    gdma_channel_handle_t dma_chan;
    size_t bb_size;
    struct { bool stream_mode; } flags;
};
#define __containerof(ptr, type, member) \
    reinterpret_cast<type*>(reinterpret_cast<char*>(ptr) - offsetof(type, member))
struct esp_lcd_rgb_panel_event_data_t {};
using Callback = bool (*)(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*);
struct esp_lcd_rgb_panel_event_callbacks_t { Callback on_vsync; };
int dmaToken = 0;
esp_rgb_panel_t panel{{}, &dmaToken, 0, {true}};
Callback callback = nullptr;
esp_err_t requestResult = ESP_OK, resetResult = ESP_OK, startResult = ESP_OK;
esp_err_t registerResult = ESP_OK;
uint32_t hardwareResets = 0, hardwareStarts = 0;
uint32_t gdma_ll_tx_get_interrupt_status(gdma_dev_t* dev, int channel, bool raw) {
    assert(channel == 2 && raw); return dev->raw;
}
void gdma_ll_tx_clear_interrupt_status(gdma_dev_t* dev, int channel, uint32_t mask) {
    assert(channel == 2 && !(mask & ~uint32_t(0xA0))); dev->raw &= ~mask;
}
esp_err_t gdma_get_group_channel_id(gdma_channel_handle_t dma, int* group, int* channel) {
    assert(dma == &dmaToken); *group = 0; *channel = 2; return ESP_OK;
}
esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t,
        const esp_lcd_rgb_panel_event_callbacks_t* callbacks, void*) {
    if (registerResult == ESP_OK) callback = callbacks->on_vsync;
    return registerResult;
}
esp_err_t esp_lcd_rgb_panel_restart(esp_lcd_panel_handle_t value) {
    if (requestResult == ESP_OK) value->needRestart = true;
    return requestResult;
}
esp_err_t gdma_reset(gdma_channel_handle_t) { ++hardwareResets; return resetResult; }
esp_err_t gdma_start(gdma_channel_handle_t, intptr_t) { ++hardwareStarts; return startResult; }

#include "rgb_recovery_production.inc"

static FastBeeRgbRecoveryStats stats() {
    FastBeeRgbRecoveryStats value{};
    fastbee_rgb_recovery_get_stats(&value);
    return value;
}
static void vsync() {
    assert(callback);
    callback(&panel.base, nullptr, nullptr);
    if (panel.base.needRestart) {
        panel.base.needRestart = false;
        fastbee_rgb_gdma_reset(panel.dma_chan);
        fastbee_rgb_gdma_start(panel.dma_chan, 0x1234);
    }
}
static void attach(uint32_t at = 0) {
    assert(fastbee_rgb_recovery_attach(&panel.base, at));
    assert(stats().active);
}
static void startup() {
    attach(); fastbee_rgb_recovery_service(2000); vsync();
    assert(stats().applied == 1);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "one_startup_not_periodic") {
        attach(); fastbee_rgb_recovery_service(1999); vsync();
        assert(stats().requested == 0);
        fastbee_rgb_recovery_service(2000);
        assert(stats().requested == 1 && stats().applied == 0 && stats().inFlight);
        vsync();
        assert(stats().applied == 1 && stats().lastReason == FASTBEE_RGB_RECOVERY_STARTUP);
        for (uint32_t now = 3000; now < 30000; now += 50) {
            fastbee_rgb_recovery_service(now); vsync();
        }
        assert(stats().requested == 1 && stats().applied == 1 && stats().startupRequests == 1);
    } else if (scenario == "latched_cooldown") {
        startup(); hardware.raw = GDMA_LL_EVENT_TX_L1_FIFO_UDF | 2U; vsync();
        assert(hardware.raw == 2 && stats().l1 == 1 && stats().l3 == 0);
        fastbee_rgb_recovery_service(2100); fastbee_rgb_recovery_service(2999);
        assert(stats().pending && stats().suppressed == 1 && stats().requested == 1);
        fastbee_rgb_recovery_service(3000);
        assert(stats().requested == 2 && stats().applied == 1 && !stats().pending);
        vsync(); assert(stats().applied == 2 && hardware.raw == 2);
        assert(stats().lastReason == FASTBEE_RGB_RECOVERY_UNDERFLOW);
    } else if (scenario == "persistent_events_bounded") {
        startup();
        for (uint32_t now = 2020; now <= 4500; now += 20) {
            hardware.raw |= GDMA_LL_EVENT_TX_L3_FIFO_UDF; vsync();
            fastbee_rgb_recovery_service(now);
        }
        vsync();
        assert(stats().requested == 3 && stats().applied == 3 && stats().pending);
        assert(stats().l3 > 100 && stats().udfEvents == stats().l3);
    } else if (scenario == "request_error_retry") {
        attach(); requestResult = ESP_FAIL;
        fastbee_rgb_recovery_service(2000);
        assert(stats().requestErrors == 1 && stats().pending && !stats().inFlight && !stats().applied);
        fastbee_rgb_recovery_service(2999); assert(stats().requested == 1);
        requestResult = ESP_OK; fastbee_rgb_recovery_service(3000); vsync();
        assert(stats().applied == 1 && stats().requested == 2);
    } else if (scenario == "reset_error_not_applied") {
        attach(); resetResult = ESP_FAIL; fastbee_rgb_recovery_service(2000); vsync();
        assert(stats().resetErrors == 1 && stats().applied == 0 && stats().pending);
        resetResult = ESP_OK; fastbee_rgb_recovery_service(3000); vsync();
        assert(stats().applied == 1 && !stats().pending);
    } else if (scenario == "start_error_not_applied") {
        attach(); startResult = ESP_FAIL; fastbee_rgb_recovery_service(2000); vsync();
        assert(stats().startErrors == 1 && stats().applied == 0 && stats().pending);
        startResult = ESP_OK; fastbee_rgb_recovery_service(3000); vsync();
        assert(stats().applied == 1);
    } else if (scenario == "manual_bypasses_grace_cooldown") {
        attach(); assert(fastbee_rgb_recovery_request());
        fastbee_rgb_recovery_service(50); vsync();
        assert(stats().applied == 1 && stats().lastReason == FASTBEE_RGB_RECOVERY_MANUAL);
        assert(fastbee_rgb_recovery_request()); fastbee_rgb_recovery_service(60); vsync();
        assert(stats().applied == 2);
    } else if (scenario == "timer_wrap") {
        const uint32_t at = UINT32_MAX - 1000U; attach(at);
        fastbee_rgb_recovery_service(at + 1999U); assert(!stats().requested);
        fastbee_rgb_recovery_service(at + 2000U); vsync(); assert(stats().applied == 1);
    } else if (scenario == "unrelated_dma_ignored") {
        attach(); fastbee_rgb_recovery_service(2000);
        int other = 0; fastbee_rgb_gdma_reset(&other); fastbee_rgb_gdma_start(&other, 5);
        assert(!stats().applied && stats().inFlight); vsync(); assert(stats().applied == 1);
    } else if (scenario == "detach_and_failed_attach") {
        startup(); const auto old = stats();
        fastbee_rgb_recovery_detach(&panel.base);
        assert(!callback && !stats().active && !fastbee_rgb_recovery_request());
        hardware.raw = 0xA0; fastbee_rgb_recovery_vsync(&panel.base, nullptr, nullptr);
        fastbee_rgb_recovery_service(20000); assert(stats().requested == old.requested && hardware.raw == 0xA0);
        registerResult = ESP_FAIL; assert(!fastbee_rgb_recovery_attach(&panel.base, 30000));
        assert(!stats().active);
    } else if (scenario == "events_while_request_in_flight") {
        attach(); fastbee_rgb_recovery_service(2000);
        hardware.raw = GDMA_LL_EVENT_TX_L1_FIFO_UDF | GDMA_LL_EVENT_TX_L3_FIFO_UDF;
        vsync(); assert(stats().applied == 1 && !stats().pending);
        fastbee_rgb_recovery_service(3000); vsync();
        assert(stats().applied == 1 && !stats().pending && stats().l1 == 1 && stats().l3 == 1);
    } else if (scenario == "failed_manual_retry_is_bounded") {
        startup(); requestResult = ESP_FAIL;
        assert(fastbee_rgb_recovery_request()); fastbee_rgb_recovery_service(2100);
        assert(stats().requestErrors == 1 && stats().requested == 2);
        fastbee_rgb_recovery_service(2120); fastbee_rgb_recovery_service(3099);
        assert(stats().requested == 2 && stats().pending);
        requestResult = ESP_OK; fastbee_rgb_recovery_service(3100); vsync();
        assert(stats().applied == 2);
    } else {
        assert(false && "unknown scenario");
    }
    static_assert(sizeof(FastBeeRgbRecoveryState) <= 256, "recovery must stay small");
    std::cout << scenario << " PASS state_bytes=" << sizeof(FastBeeRgbRecoveryState) << '\n';
}
