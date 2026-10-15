#include "screen/ScreenRuntime.h"

#include "core/FeatureBudget.h"
#include "core/FeatureFlags.h"
#include "core/RuntimeFeatureManager.h"
#include "core/ScreenPanelManager.h"
#include "core/MotorControlService.h"
#include "screen/ScreenDevice.h"
#include "screen/ScreenGeometry.h"
#include "screen/ScreenInteraction.h"
#include "screen/RgbRecovery.h"
#include "screen/StripeChangeCache.h"
#include "screen/WeatherStatusText.h"
#include "StripeRenderer.h"
#include "systems/LoggerSystem.h"

#ifndef FASTBEE_SCREEN_WORKER_ISOLATION
#define FASTBEE_SCREEN_WORKER_ISOLATION 0
#endif
#ifndef FASTBEE_SCREEN_PERF_TELEMETRY
#define FASTBEE_SCREEN_PERF_TELEMETRY 0
#endif

#if FASTBEE_SCREEN_WORKER_ISOLATION == 1
#include "screen/ScreenWorkerIsolation.h"
#endif

#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
#include <U8g2lib.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <new>
#include <time.h>

namespace FastBee {
namespace Screen {

#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)

namespace {

constexpr uint32_t SNAPSHOT_INTERVAL_MS = 500;
constexpr uint32_t MOTOR_PENDING_SNAPSHOT_INTERVAL_MS = 100;
constexpr uint32_t START_TIMEOUT_MS = 5000;
constexpr uint32_t STOP_TIMEOUT_MS = 2000;
constexpr uint32_t TOUCH_POLL_MS = 20;
constexpr uint32_t TAP_MAX_MS = 900;
constexpr uint32_t REPEAT_START_MS = 600;
constexpr uint32_t REPEAT_INTERVAL_MS = 150;
constexpr uint32_t REPEAT_FAST_AFTER_MS = 2000;
constexpr int REPEAT_FAST_MULTIPLIER = 10;
constexpr uint16_t TAP_MOVE_PX = 14;
constexpr uint16_t MIN_SWIPE_PX = 40;
constexpr uint32_t BUZZER_PULSE_MS = 80;
constexpr uint32_t BUZZER_OFF_RETRY_MS = 100;
constexpr uint32_t TASK_STACK_BYTES = 8192;
constexpr UBaseType_t TASK_PRIORITY = 2;
constexpr size_t SCREEN_INTERNAL_RESERVE_PSRAM = 20U * 1024U;
constexpr size_t SCREEN_INTERNAL_RESERVE_NO_PSRAM = 16U * 1024U;
constexpr size_t SCREEN_PSRAM_RESERVE = 8U * 1024U;

uint32_t snapshotIntervalMs(const ScreenPanelSnapshot& snapshot) {
    if (snapshot.activePageIndex >= snapshot.pageCount) return SNAPSHOT_INTERVAL_MS;
    const char* pageId = snapshot.pages[snapshot.activePageIndex].id;
    for (size_t index = 0; index < snapshot.cardCount; ++index) {
        const auto& card = snapshot.cards[index];
        if (card.pending && card.visualType == ScreenVisualType::MOTOR_CONTROL &&
            std::strcmp(card.pageId, pageId) == 0) return MOTOR_PENDING_SNAPSHOT_INTERVAL_MS;
    }
    return SNAPSHOT_INTERVAL_MS;
}

struct RuntimeState {
    SemaphoreHandle_t mutex = nullptr;
    std::atomic<SemaphoreHandle_t> observationMutex{nullptr};
    SemaphoreHandle_t started = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> refreshRequested{false};
    bool running = false;
    bool sleeping = false;
    bool startSucceeded = false;
    ScreenSystemAction pendingSystemAction = ScreenSystemAction::NONE;
    uint8_t pendingSystemValue = 0;
    ScreenSystemActionCapabilities systemActionCapabilities{};
    size_t allocated = 0;
    String lastError;
    std::atomic<bool> backlightChanging{true};
    BacklightResourceSnapshot backlightResources;
};

std::atomic<RuntimeState*> runtimeObserverState{nullptr};

RuntimeState& runtimeState() {
    static RuntimeState value;
    runtimeObserverState.store(&value, std::memory_order_release);
    return value;
}

bool ensureRuntimeSynchronization(RuntimeState& state) {
    // First-start callers must not publish different lifetime mutexes. Failed
    // allocations may be retried by a later business start, never by a query.
    static std::atomic_flag initializing = ATOMIC_FLAG_INIT;
    if (initializing.test_and_set(std::memory_order_acquire)) return false;
    if (!state.mutex) state.mutex = xSemaphoreCreateMutex();
    if (!state.started) state.started = xSemaphoreCreateBinary();
    const bool ready = state.mutex && state.started;
    if (ready) state.observationMutex.store(state.mutex, std::memory_order_release);
    initializing.clear(std::memory_order_release);
    return ready;
}

class StateLock {
public:
    explicit StateLock(TickType_t timeout = pdMS_TO_TICKS(100))
        : _state(runtimeState()),
          _locked(_state.mutex &&
                  xSemaphoreTake(_state.mutex, timeout) == pdTRUE) {}
    ~StateLock() {
        if (_locked) xSemaphoreGive(_state.mutex);
    }
    bool locked() const { return _locked; }
private:
    RuntimeState& _state;
    bool _locked;
};

void publishBacklightResources() {
    BacklightResourceSnapshot snapshot;
    ScreenDevice::instance().snapshotBacklightResource(snapshot); // Screen task only.
    auto& state = runtimeState();
    StateLock lock;
    if (!lock.locked()) return; // Keep changing=true instead of publishing stale data.
    snapshot.sampledAt = millis();
    state.backlightResources = snapshot;
    state.backlightChanging.store(false, std::memory_order_release);
}

bool deactivateScreenDeviceResources(String* error) {
    runtimeState().backlightChanging.store(true, std::memory_order_release);
    const bool stopped = ScreenDevice::instance().deactivate(error);
    publishBacklightResources();
    return stopped;
}

size_t screenFramebufferBytes(const DisplayConfigParams* config) {
    return config && strcasecmp(config->backend, "rgb") == 0
        ? static_cast<size_t>(config->width) * config->height * sizeof(uint16_t) : 0;
}

bool screenMemoryAvailable(const DisplayConfigParams* config, String& error) {
    if (!config) {
        error = "SCREEN_DISPLAY_NOT_CONFIGURED";
        return false;
    }
    const size_t stripeLines = std::max<size_t>(
        1, std::min<size_t>(config->bufferLines, config->height));
    const size_t stripeBytes = static_cast<size_t>(config->width) *
                               stripeLines * sizeof(uint16_t);
    const size_t snapshotBytes = sizeof(ScreenPanelSnapshot);
    const size_t framebufferBytes = screenFramebufferBytes(config);
    const size_t largestPayload = std::max(std::max(snapshotBytes, stripeBytes),
        framebufferBytes ? framebufferBytes + 4096U : 0U);
    const size_t internalFree = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internalLargest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const bool hasPsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;

    if (hasPsram) {
        const size_t psramFree = heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const size_t psramLargest = heap_caps_get_largest_free_block(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (internalFree >= SCREEN_INTERNAL_RESERVE_PSRAM + TASK_STACK_BYTES &&
            internalLargest >= TASK_STACK_BYTES &&
            psramFree >= framebufferBytes + snapshotBytes + stripeBytes + SCREEN_PSRAM_RESERVE &&
            psramLargest >= largestPayload) {
            error = "";
            return true;
        }
    }

    // Continuous RGB DMA requires a PSRAM framebuffer; do not admit it through
    // the SPI renderer's internal-memory fallback when PSRAM is absent/full.
    if (framebufferBytes) {
        error = hasPsram ? "SCREEN_PANEL_PSRAM_OR_DRAM_LOW" : "SCREEN_RGB_PSRAM_REQUIRED";
        return false;
    }
    const size_t requiredInternal = snapshotBytes + stripeBytes +
        TASK_STACK_BYTES + SCREEN_INTERNAL_RESERVE_NO_PSRAM;
    if (internalFree >= requiredInternal &&
        internalLargest >= largestPayload) {
        error = "";
        return true;
    }
    error = hasPsram ? "SCREEN_PANEL_PSRAM_OR_DRAM_LOW" :
                       "SCREEN_PANEL_DRAM_LOW";
    return false;
}

uint32_t hashBytes(uint32_t hash, const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619UL;
    }
    return hash;
}

uint32_t snapshotHash(const ScreenPanelSnapshot& snapshot,
                      const char* confirmationCardId) {
    uint32_t hash = 2166136261UL;
    hash = hashBytes(hash, &snapshot.revision, sizeof(snapshot.revision));
    hash = hashBytes(hash, &snapshot.activePageIndex,
                     sizeof(snapshot.activePageIndex));
    hash = hashBytes(hash, &snapshot.pageCount, sizeof(snapshot.pageCount));
    hash = hashBytes(hash, &snapshot.cardCount, sizeof(snapshot.cardCount));
    hash = hashBytes(hash, &snapshot.bottomNavCount, sizeof(snapshot.bottomNavCount));
    hash = hashBytes(hash, snapshot.bottomNavPageIds.data(),
                     sizeof(snapshot.bottomNavPageIds));
    hash = hashBytes(hash, snapshot.themeId, sizeof(snapshot.themeId));
    hash = hashBytes(hash, snapshot.backgroundPresetId,
                     sizeof(snapshot.backgroundPresetId));
    if (snapshot.pageCount > 0) {
        const uint8_t pageIndex = std::min<uint8_t>(
            snapshot.activePageIndex, snapshot.pageCount - 1U);
        const ScreenPanelPageSnapshot& page = snapshot.pages[pageIndex];
        hash = hashBytes(hash, &page, sizeof(page));
        for (uint8_t offset = 0; offset < page.cardCount; ++offset) {
            const size_t index = page.firstCard + offset;
            if (index >= snapshot.cardCount) break;
            const ScreenPanelCardSnapshot& card = snapshot.cards[index];
            // Receipt time alone is not visible. System sources update it on
            // every snapshot; hashing it needlessly redraws an unchanged page.
            const size_t timestampOffset = offsetof(ScreenPanelCardSnapshot, updatedMs);
            hash = hashBytes(hash, &card, timestampOffset);
            const size_t afterTimestamp = timestampOffset + sizeof(card.updatedMs);
            hash = hashBytes(hash, reinterpret_cast<const uint8_t*>(&card) + afterTimestamp,
                             sizeof(card) - afterTimestamp);
        }
    }
    if (confirmationCardId) {
        hash = hashBytes(hash, confirmationCardId,
                         strlen(confirmationCardId));
    }
    const time_t now = time(nullptr);
    const uint32_t minute = now > 0 ? static_cast<uint32_t>(now / 60) : 0;
    return hashBytes(hash, &minute, sizeof(minute));
}

void updateRuntimeState(bool running, bool sleeping, size_t allocated,
                        const String& error = "") {
    StateLock lock;
    if (!lock.locked()) return;
    RuntimeState& state = runtimeState();
    state.running = running;
    state.sleeping = sleeping;
    state.allocated = allocated;
    state.lastError = error;
}

ScreenSystemActionCapabilities readSystemActionCapabilities() {
    ScreenSystemActionCapabilities caps{};
    ScreenDevice& device = ScreenDevice::instance();
    IDisplayBackend* display = device.display();
    ITouchBackend* touch = device.touch();
    caps.displayReady = display && display->ready();
    caps.touchConfigured = device.config() &&
        strcasecmp(device.config()->touchBackend, "none") != 0;
    caps.touchReady = touch && touch->ready();
    strlcpy(caps.touchError, device.touchError().c_str(), sizeof(caps.touchError));
    if (caps.displayReady && caps.touchReady) {
        const DisplayCapabilities displayCaps = display->capabilities();
        caps.brightness = displayCaps.brightness;
        caps.brightnessContinuous = displayCaps.brightnessContinuous;
        caps.minVisibleBrightnessPercent = displayCaps.minVisibleBrightnessPercent;
        caps.sleep = displayCaps.sleep;
        caps.touchWake = true;
        caps.buzzer = displayCaps.buzzer;
        caps.restart = true;
    }
    return caps;
}

void signalStarted(bool success, const String& error) {
    LOG_INFOF("ScreenRuntime: start=%s error=%s internal=%u psram=%u",
              success ? "running" : "failed", error.c_str(),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    {
        StateLock lock;
        if (lock.locked()) {
            RuntimeState& state = runtimeState();
            state.startSucceeded = success;
            state.running = success;
            state.systemActionCapabilities = success ? readSystemActionCapabilities() :
                ScreenSystemActionCapabilities{};
            state.lastError = error;
        }
    }
    RuntimeState& state = runtimeState();
    if (state.started) xSemaphoreGive(state.started);
}

void finishTask() {
    // Never delete the task while its published handle still points to it.
    // A contended state lock is retryable, just like backend cleanup.
    for (;;) {
        {
            StateLock lock;
            if (lock.locked()) {
                RuntimeState& state = runtimeState();
                state.running = false;
                state.sleeping = false;
                state.pendingSystemAction = ScreenSystemAction::NONE;
                state.pendingSystemValue = 0;
                state.systemActionCapabilities = {};
                state.allocated = 0;
                state.task = nullptr;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(nullptr);
}

struct TouchGesture {
    bool down = false;
    bool suppressUntilRelease = false;
    bool tapCancelled = false;
    int tapTarget = 0;
    uint32_t revision = 0;
    uint8_t pageIndex = 0;
    int slider = -1;
    uint32_t sliderUpdatedMs = 0;
    uint16_t startX = 0;
    uint16_t startY = 0;
    uint16_t lastX = 0;
    uint16_t lastY = 0;
    uint32_t startedMs = 0;
    // Auto-repeat state for motor adjust and numeric ± controls only. Every
    // `gesture = TouchGesture{}` reset (release, invalidation, layout save,
    // controller recovery, sleep) also disarms a pending repeat.
    int repeatCard = -1;
    uint8_t repeatRow = 0;
    uint8_t repeatCol = 0;
    uint32_t repeatNextMs = 0;
    bool repeated = false;
};

bool dispatchCardAction(ScreenPanelCardSnapshot& card,
                        bool discardUnsavedRestartHistory = false,
                        uint32_t displayedRevision = 0) {
    if (card.visualType == ScreenVisualType::MOTOR_CONTROL) {
        const char* actions[] = {"", "forward", "reverse", "setSpeed", "setPulse"};
        const uint8_t selected = card.motorConfirmAction;
        card.motorConfirmAction = 0;
        if (!selected || selected > 4) return false;
        const char* blocked = selected <= 2 ? screenMotorStartBlockReason(card, selected == 1) :
            card.pending ? "请等待指令完成" : nullptr;
        if (blocked) {
            strlcpy(card.error, blocked, sizeof(card.error));
            screenRememberMotorFeedback(card, millis());
            return false;
        }
        card.motorUiFeedback[0] = '\0';
        String action(actions[selected]);
        if (selected > 2) action += ":" + String(card.motorConfirmValue);
        uint32_t operation = 0; String error;
        if (ScreenPanelManager::instance().dispatchAction(card.id, ScreenActionKind::MOTOR_COMMAND, action, operation, error, card.controlRef)) {
            card.pending = true; card.error[0] = '\0'; return true;
        }
        strlcpy(card.error, error.c_str(), sizeof(card.error));
        screenRememberMotorFeedback(card, millis());
        return false;
    }
    ScreenActionKind kind = ScreenActionKind::TRIGGER;
    bool target = true;
    if (card.kind == ScreenCardKind::SYSTEM) {
        kind = ScreenActionKind::SYSTEM_ACTION;
        target = !strcmp(card.systemActionId, "touch-beep")
            ? !(strcmp(card.value, "true") == 0 || strcmp(card.value, "1") == 0)
            : discardUnsavedRestartHistory;
    } else if (card.visualType == ScreenVisualType::SWITCH) {
        kind = ScreenActionKind::SET_BOOLEAN;
        target = !(card.value[0] &&
                   (strcasecmp(card.value, "true") == 0 ||
                    strcasecmp(card.value, "on") == 0 ||
                    strcmp(card.value, "1") == 0));
    } else if (card.visualType == ScreenVisualType::NUMBER_CONTROL) {
        char value[32];
        snprintf(value, sizeof(value), "%.*f", card.precision,
                 static_cast<double>(card.numericValue));
        uint32_t operationId = 0;
        String error;
        if (!ScreenPanelManager::instance().dispatchAction(
                card.id, ScreenActionKind::SET_NUMBER, String(value),
                operationId, error, card.controlRef, displayedRevision)) {
            LOG_WARNINGF("ScreenRuntime: numeric action '%s' rejected: %s",
                         card.id, error.c_str());
            strlcpy(card.error, error.c_str(), sizeof(card.error));
            return false;
        }
        return true;
    }
    uint32_t operationId = 0;
    String error;
    if (!ScreenPanelManager::instance().dispatchAction(
            card.id, kind, kind == ScreenActionKind::SET_BOOLEAN ? String(target ? "on" : "off") :
                kind == ScreenActionKind::SYSTEM_ACTION ? String(target ? "true" : "false") : String(),
            operationId, error, card.controlRef, displayedRevision)) {
        LOG_WARNINGF("ScreenRuntime: action '%s' rejected: %s",
                     card.id, error.c_str());
        strlcpy(card.error, error.c_str(), sizeof(card.error));
        return false;
    }
    return true;
}

bool handleContentTap(StripeRenderer& renderer, ScreenPanelSnapshot& snapshot,
                      ScreenPanelCardSnapshot& card, uint16_t x, uint16_t y,
                      char* confirmationCardId = nullptr, size_t confirmationCapacity = 0,
                      ScreenControlSession* controlSession = nullptr, uint32_t now = 0) {
    const PixelRect bounds = renderer.cardBounds(snapshot, card);
    if (card.visualType == ScreenVisualType::LINE) {
        // A still tap highlights the nearest plotted sample (detail row reads
        // that point). Swipe arbitration stays untouched: any drag past
        // TAP_MOVE_PX cancels the tap before this branch can run.
        const int nearest = renderer.historyTapIndex(card, bounds, x);
        if (nearest >= 0) card.selectedHistorySequence = card.history[nearest].sequence;
        return true;
    }
    // Data, chart, clock and weather surfaces stay read-only; a tap there
    // must not fall through to dispatchCardAction().
    if (card.kind == ScreenCardKind::DATA ||
        card.kind == ScreenCardKind::CHART ||
        card.kind == ScreenCardKind::CLOCK ||
        card.kind == ScreenCardKind::WEATHER) {
        return true;
    }
    if (card.visualType == ScreenVisualType::MOTOR_CONTROL) {
        const ScreenMotorLayout layout{bounds};
        if (!layout.valid()) return true;
        String action;
        if (screenRectContains(layout.stop(), x, y)) action = "stop";
        for (unsigned col = 0; col < 2; ++col) {
            if (screenRectContains(layout.direction(col), x, y)) {
                if (const char* blocked = screenMotorStartBlockReason(card, col == 0)) {
                    strlcpy(card.error, blocked, sizeof(card.error));
                    screenRememberMotorFeedback(card, now);
                    return true;
                }
                action = col ? "reverse" : "forward";
            }
        }
        if (action.isEmpty()) {
            if (card.pending) return true;
            for (unsigned row = 0; row < 2; ++row) for (unsigned col = 0; col < 3; ++col) {
                if (!screenRectContains(layout.adjust(row, col), x, y)) continue;
                uint16_t& value = row ? card.motorPulseInput : card.motorSpeedInput;
                bool& dirty = row ? card.motorPulseDirty : card.motorSpeedDirty;
                if (controlSession) controlSession->used(card.controlRef, now);
                if (col < 2) {
                    const int step = row ? card.motorPulseStep : card.motorSpeedStep;
                    value = std::max(0, std::min(65535, static_cast<int>(value) + (col ? step : -step)));
                    dirty = true; card.error[0] = '\0'; card.motorUiFeedback[0] = '\0'; return true;
                }
                action = String(row ? "setPulse:" : "setSpeed:") + String(value);
            }
        }
        if (action.isEmpty()) return true;
        card.motorUiFeedback[0] = '\0';
        const bool motion = action == "forward" || action == "reverse";
        if (motion && card.requiresConfirmation && (!controlSession || !controlSession->allows(card.controlRef))) {
            if (confirmationCardId && confirmationCapacity) {
                card.motorConfirmAction = action == "forward" ? 1 : action == "reverse" ? 2 : action.startsWith("setSpeed:") ? 3 : 4;
                card.motorConfirmValue = card.motorConfirmAction == 3 ? card.motorSpeedInput : card.motorPulseInput;
                strlcpy(confirmationCardId, card.id, confirmationCapacity);
            }
            return true;
        }
        uint32_t operation = 0; String error;
        if (!ScreenPanelManager::instance().dispatchAction(card.id, ScreenActionKind::MOTOR_COMMAND, action, operation, error, card.controlRef)) {
            strlcpy(card.error, error.c_str(), sizeof(card.error));
            screenRememberMotorFeedback(card, now);
        }
        else { card.pending = true; card.error[0] = '\0'; if (controlSession) controlSession->used(card.controlRef, now); }
        return true;
    }
    if (card.kind == ScreenCardKind::SYSTEM && !strcmp(card.systemActionId, "touch-beep")) {
        const bool enabled = !snapshot.touchBeepEnabled;
        String error;
        if (ScreenPanelManager::instance().queueSetting("touchBeepEnabled", enabled ? 1 : 0, error)) {
            snapshot.touchBeepEnabled = enabled;
            card.numericValue = enabled ? 1 : 0;
            strlcpy(card.value, enabled ? "true" : "false", sizeof(card.value));
            card.error[0] = '\0';
        } else strlcpy(card.error, error.c_str(), sizeof(card.error));
        return true;
    }
    if (card.visualType == ScreenVisualType::SLIDER) {
        // Brightness is changed only by the drag path in the worker.  Labels
        // and card padding are deliberately inert.
        return true;
    }
    if (card.visualType == ScreenVisualType::ACTION ||
        card.visualType == ScreenVisualType::SWITCH) {
        // Parent cards are display surfaces; only their explicit control
        // rectangle may fall through to dispatchCardAction().
        if (!renderer.actionHitTest(snapshot, card, x, y)) return true;
    }
    if (card.visualType == ScreenVisualType::NUMBER_CONTROL &&
        renderer.numericControlHitTest(snapshot, card, x, y) == NumericControlHit::NONE) {
        return true;
    }
    if (card.visualType == ScreenVisualType::SELECT) {
        const bool confirm = strcmp(card.systemActionId, "confirm-idle") == 0;
        const uint32_t sleepChoices[] = {0, 30, 60, 120, 300, 600};
        const uint32_t confirmChoices[] = {15, 30, 60, 120, 300, 600};
        const auto* choices = confirm ? confirmChoices : sleepChoices;
        const bool next = screenRectContains(screenSelectButton(bounds, true), x, y);
        if (!next && !screenRectContains(screenSelectButton(bounds, false), x, y)) return true;
        const uint32_t current = static_cast<uint32_t>(card.numericValue);
        uint32_t value = current;
        if (next) { for (size_t i = 0; i < 6; ++i) if (choices[i] > current) { value = choices[i]; break; } }
        else { for (int i = 5; i >= 0; --i) if (choices[i] < current) { value = choices[i]; break; } }
        if (value == current) return true;
        String error;
        if (ScreenPanelManager::instance().queueSetting(confirm ? "controlConfirmIdleSeconds" : "idleSleepSeconds", value, error)) {
            card.numericValue = value; card.error[0] = '\0';
            if (confirm) snapshot.controlConfirmIdleSeconds = value;
            else snapshot.idleSleepSeconds = value;
        }
        else strlcpy(card.error, error.c_str(), sizeof(card.error));
        return true;
    }
    return false;
}

void handleTap(StripeRenderer& renderer, ScreenPanelSnapshot& snapshot,
               uint16_t x, uint16_t y, char* confirmationCardId,
               size_t confirmationCapacity, ScreenControlSession* controlSession = nullptr,
               uint32_t now = 0) {
    if (controlSession) controlSession->observe(snapshot.revision, now, snapshot.controlConfirmIdleSeconds);
    const int pageTab = renderer.pageNavigationHit(snapshot, x, y);
    if (pageTab >= 0) {
        confirmationCardId[0] = '\0';
        const int pageIndex = renderer.navigationPageIndex(snapshot, pageTab);
        if (pageIndex == -2) renderer.toggleNavigation();
        else if (pageIndex >= 0 && pageIndex < snapshot.pageCount) {
            renderer.closeNavigation();
            ScreenPanelManager::instance().setActivePage(String(snapshot.pages[pageIndex].id));
        }
        return;
    }
    if (renderer.navigationOpen()) {
        const int pageIndex = renderer.pageMenuHit(snapshot, x, y);
        if (pageIndex >= 0) {
            renderer.closeNavigation();
            confirmationCardId[0] = '\0';
            ScreenPanelManager::instance().setActivePage(String(snapshot.pages[pageIndex].id));
        }
        return;
    }
    if (confirmationCardId[0]) {
        ScreenPanelCardSnapshot* selected = nullptr;
        for (uint8_t i = 0; i < snapshot.cardCount; ++i)
            if (!strcmp(snapshot.cards[i].id, confirmationCardId)) { selected = &snapshot.cards[i]; break; }
        const bool motor = selected && selected->visualType == ScreenVisualType::MOTOR_CONTROL;
        if (motor && renderer.confirmationStopHit(x, y)) {
            uint32_t operation = 0; String error;
            if (!ScreenPanelManager::instance().dispatchAction(selected->id, ScreenActionKind::MOTOR_COMMAND, String("stop"), operation, error, selected->controlRef))
                strlcpy(selected->error, error.c_str(), sizeof(selected->error));
            else { selected->pending = true; selected->error[0] = '\0'; }
            selected->motorConfirmAction = 0;
            confirmationCardId[0] = '\0';
            return;
        }
        bool confirmed = false;
        if (!renderer.confirmationChoice(x, y, confirmed, motor)) return;
        if (confirmed) {
            for (uint8_t i = 0; i < snapshot.cardCount; ++i) {
                if (strncmp(snapshot.cards[i].id, confirmationCardId,
                            confirmationCapacity) == 0) {
                    const bool discardHistory =
                        snapshot.cards[i].kind == ScreenCardKind::SYSTEM &&
                        strcmp(snapshot.cards[i].error,
                               "history_save_failed") == 0;
                    if (dispatchCardAction(snapshot.cards[i], discardHistory, snapshot.revision) && controlSession &&
                        snapshot.cards[i].visualType == ScreenVisualType::MOTOR_CONTROL)
                        controlSession->approve(snapshot.cards[i].controlRef, now);
                    break;
                }
            }
        }
        confirmationCardId[0] = '\0';
        return;
    }

    const int index = renderer.hitTest(snapshot, x, y);
    if (index < 0 || index >= snapshot.cardCount) return;
    ScreenPanelCardSnapshot& card = snapshot.cards[index];
    if (card.kind == ScreenCardKind::PAGE_LINK) {
        if (card.targetPageId[0]) {
            ScreenPanelManager::instance().setActivePage(String(card.targetPageId));
        }
        return;
    }
    if (handleContentTap(renderer, snapshot, card, x, y, confirmationCardId, confirmationCapacity, controlSession, now)) return;
    if (card.pending) return;
    if (card.visualType == ScreenVisualType::NUMBER_CONTROL) {
        const NumericControlHit hit = renderer.numericControlHitTest(
            snapshot, card, x, y);
        if (hit == NumericControlHit::DECREMENT ||
            hit == NumericControlHit::INCREMENT) {
            const double direction = hit == NumericControlHit::INCREMENT ? 1.0 : -1.0;
            const double next = std::max<double>(card.rangeMin,
                std::min<double>(card.rangeMax,
                    static_cast<double>(card.numericValue) +
                    direction * card.numericStep));
            const double scale = std::pow(10.0, card.precision);
            card.numericValue = static_cast<float>(std::round(next * scale) / scale);
            return;
        }
        if (hit != NumericControlHit::EXECUTE) return;
    }
    if (card.requiresConfirmation) {
        strlcpy(confirmationCardId, card.id, confirmationCapacity);
        return;
    }
    dispatchCardAction(card, false, snapshot.revision);
}

void changePage(ScreenPanelSnapshot& snapshot, int direction) {
    if (snapshot.pageCount < 2) return;
    int next = static_cast<int>(snapshot.activePageIndex) + direction;
    if (next < 0 || next >= snapshot.pageCount) return;
    ScreenPanelManager::instance().setActivePage(
        String(snapshot.pages[next].id));
}

void runScreenTask() {
    RuntimeState& state = runtimeState();
    ScreenDevice& device = ScreenDevice::instance();
    StripeRenderer renderer;
    ScreenPanelSnapshot* snapshot = nullptr;
    size_t snapshotBytes = 0;
    String error;

    if (!device.configured()) {
        signalStarted(false, "SCREEN_DISPLAY_NOT_CONFIGURED");
        return;
    }
    state.backlightChanging.store(true, std::memory_order_release);
    const bool activated = device.activate(error);
    publishBacklightResources();
    if (!activated) {
        signalStarted(false, error);
        return;
    }

    void* snapshotMemory = allocatePreferred(sizeof(ScreenPanelSnapshot));
    if (!snapshotMemory) {
        signalStarted(false, "SCREEN_SNAPSHOT_ALLOCATION_FAILED");
        return;
    }
    snapshot = new (snapshotMemory) ScreenPanelSnapshot();
    snapshotBytes = sizeof(ScreenPanelSnapshot);
    ScreenPanelSnapshotCache snapshotCache;
    if (!ScreenPanelManager::instance().getActiveSnapshot(*snapshot, snapshotCache)) {
        snapshot->~ScreenPanelSnapshot();
        heap_caps_free(snapshotMemory);
        signalStarted(false, "SCREEN_SNAPSHOT_UNAVAILABLE");
        return;
    }

    const DisplayConfigParams* config = device.config();
    if (!config || !renderer.begin(device.display(), *config, error)) {
        snapshot->~ScreenPanelSnapshot();
        heap_caps_free(snapshotMemory);
        signalStarted(false, error.isEmpty() ?
                      String("SCREEN_RENDER_INIT_FAILED") : error);
        return;
    }

    const size_t framebufferBytes = screenFramebufferBytes(config);
    signalStarted(true, "");
    updateRuntimeState(true, false, snapshotBytes + renderer.bufferBytes() + framebufferBytes);
    char confirmationCardId[41] = {};
    ScreenControlSession controlSession;
    uint32_t controlSessionRevision = snapshot->revision;
    TouchGesture gesture;
    uint32_t lastSnapshotMs = 0;
    uint32_t lastMotorReadMs = 0;
    uint32_t lastInteractionMs = millis();
    uint32_t previousHash = 0;
    uint8_t appliedBrightness = 255;
    uint32_t idleSleepSeconds = snapshot->idleSleepSeconds;
    bool sleeping = false;
    bool forceRender = true;
    bool touchWakePending = false;
    bool touchWakeContactDown = false;
    uint8_t touchWakeAttempts = 0;
    uint32_t touchWakeNextAttemptMs = 0;
    bool renderRetryPending = false;
    uint32_t lastRenderAttemptMs = 0;
    uint32_t renderFaultFirstMs = 0;
    uint8_t renderedFaultFlags = SCREEN_FAULT_NONE;
    bool brightnessOverridden = false;
    uint32_t brightnessOverrideRevision = 0;
    // Clear a retained latch before accepting feedback, including MCU-only
    // resets. A failed OFF stays pending through sleep and bus recovery.
    bool buzzerNeedsOff = device.display()->capabilities().buzzer;
    uint32_t buzzerOffAtMs = 0;
    uint32_t buzzerOffFailureLoggedAtMs = 0;
    uint32_t lastTouchBeepMs = 0;
    bool touchBeepStarted = false;
    const auto touchFeedback = [&](uint32_t now) {
        if (!snapshot->touchBeepEnabled || buzzerNeedsOff ||
            !device.display()->capabilities().buzzer ||
            (touchBeepStarted && now - lastTouchBeepMs < 100U)) return;
        // One short asynchronous pulse per touch. The common off/retry path
        // also clears a pulse whose I2C write failed part way through.
        const bool sounded = device.display()->setBuzzer(true);
        buzzerNeedsOff = true;
        const uint32_t duration = std::max<uint32_t>(20U,
            std::min<uint32_t>(150U, config->touchBuzzerDurationMs));
        buzzerOffAtMs = sounded ? now + duration : now;
        lastTouchBeepMs = now; touchBeepStarted = true;
    };
#if FASTBEE_SCREEN_PERF_TELEMETRY == 1
    uint32_t lastPerfReportMs = millis();
    uint32_t snapshotCount = 0, renderCount = 0;
    uint64_t snapshotMicros = 0, renderMicros = 0;
    uint32_t snapshotMaxMicros = 0, renderMaxMicros = 0;
#endif
#if FASTBEE_SCREEN_WORKER_ISOLATION == 1
    // Start the real-time deadline even if a snapshot/render fails. Diagnostic
    // errors or early task notifications must never leave touch frozen forever.
    ScreenWorkerIsolation isolation(millis());
    int announcedIsolationPhase = -1;
#endif

    while (!state.stopRequested.load(std::memory_order_acquire)) {
        const uint32_t now = millis();
        controlSession.observe(snapshot->revision, now, snapshot->controlConfirmIdleSeconds);
        if (confirmationCardId[0] && now - lastInteractionMs >= snapshot->controlConfirmIdleSeconds * 1000U)
            { confirmationCardId[0] = '\0'; forceRender = true; }
        // The driver latches underflow events in VSYNC. Admission and recovery
        // requests run here, outside the interrupt and without file logging.
        fastbee_rgb_recovery_service(now);
        if (device.display()) device.display()->service(now);
        if (device.serviceTouch(now)) {
            // Never complete a gesture or a pending confirmation across a
            // controller failure/reinitialization. Only the screen task owns I/O.
            gesture = TouchGesture{};
            gesture.suppressUntilRelease = true;
            renderer.setPressed(nullptr);
            confirmationCardId[0] = '\0';
            controlSession.reset();
            lastInteractionMs = now;
            if (sleeping && device.display()->setSleep(false)) {
                sleeping = false;
                renderer.invalidate();
                updateRuntimeState(true, false, snapshotBytes + renderer.bufferBytes() + framebufferBytes);
            }
            const auto caps = readSystemActionCapabilities();
            {
                StateLock lock;
                if (lock.locked()) state.systemActionCapabilities = caps;
            }
            forceRender = true;
        }

        // The active-high buzzer must always converge to off. A failed I2C
        // write remains pending and is retried without blocking rendering.
        if (buzzerNeedsOff &&
            static_cast<int32_t>(now - buzzerOffAtMs) >= 0) {
            if (device.display()->setBuzzer(false)) {
                buzzerNeedsOff = false;
            } else {
                buzzerOffAtMs = now + BUZZER_OFF_RETRY_MS;
                if (!buzzerOffFailureLoggedAtMs ||
                    now - buzzerOffFailureLoggedAtMs >= 1000U) {
                    LOG_WARNING("ScreenRuntime: buzzer off write failed; retrying");
                    buzzerOffFailureLoggedAtMs = now;
                }
            }
        }

        ScreenSystemAction pendingAction = ScreenSystemAction::NONE;
        uint8_t pendingValue = 0;
        {
            StateLock lock(0);
            if (lock.locked() &&
                state.pendingSystemAction != ScreenSystemAction::NONE) {
                pendingAction = state.pendingSystemAction;
                pendingValue = state.pendingSystemValue;
                state.pendingSystemAction = ScreenSystemAction::NONE;
                state.pendingSystemValue = 0;
            }
        }
        if (pendingAction != ScreenSystemAction::NONE) {
            const DisplayCapabilities caps = device.display()->capabilities();
            bool applied = false;
            if (pendingAction == ScreenSystemAction::SET_BRIGHTNESS &&
                caps.brightness) {
                if (sleeping) {
                    applied = device.display()->setSleep(false);
                    if (applied) sleeping = false;
                } else {
                    applied = true;
                }
                if (applied) applied = device.display()->setBrightness(pendingValue);
                if (applied) {
                    appliedBrightness = pendingValue;
                    renderer.invalidate();
                    forceRender = true;
                    brightnessOverridden = true;
                    brightnessOverrideRevision = snapshot->revision;
                    lastInteractionMs = now;
                    updateRuntimeState(true, sleeping,
                        snapshotBytes + renderer.bufferBytes() + framebufferBytes);
                }
            } else if (pendingAction == ScreenSystemAction::SLEEP &&
                       caps.sleep && device.touch() && device.touch()->ready()) {
                // A sleep tap is intentionally silent.  Clear any earlier
                // feedback pulse before blanking the backlight so a failed
                // or delayed I2C write cannot leave the buzzer latched while
                // the panel is asleep.
                if (caps.buzzer) {
                    buzzerNeedsOff = !device.display()->setBuzzer(false);
                    buzzerOffAtMs = now + BUZZER_OFF_RETRY_MS;
                    if (!buzzerNeedsOff) buzzerOffFailureLoggedAtMs = 0;
                }
                applied = device.display()->setSleep(true);
                if (applied) {
                    sleeping = true;
                    controlSession.reset();
                    confirmationCardId[0] = '\0';
                    gesture = TouchGesture{};
                    updateRuntimeState(true, true,
                        snapshotBytes + renderer.bufferBytes() + framebufferBytes);
                }
            } else if (pendingAction == ScreenSystemAction::BUZZER &&
                       caps.buzzer && !buzzerNeedsOff) {
                // Clear a stale latch before starting or extending the pulse.
                if (device.display()->setBuzzer(false))
                    applied = device.display()->setBuzzer(true);
                buzzerNeedsOff = true;
                buzzerOffAtMs = applied ? now + BUZZER_PULSE_MS : now;
                buzzerOffFailureLoggedAtMs = 0;
                lastInteractionMs = now;
            }
            if (!applied) {
                LOG_WARNINGF("ScreenRuntime: system action %u could not be applied",
                             static_cast<unsigned>(pendingAction));
            }
        }
#if FASTBEE_SCREEN_WORKER_ISOLATION == 1
        const auto isolationPhase = isolation.phase(now);
        const bool isolationPhaseFrame = isolation.needsPhaseFrame(isolationPhase);
        if (announcedIsolationPhase != static_cast<int>(isolationPhase)) {
            announcedIsolationPhase = static_cast<int>(isolationPhase);
            const char* title = ScreenWorkerIsolation::title(isolationPhase);
            LOG_INFOF("[SCRISO] %s elapsed=%lums",
                title ? title : "NORMAL: title and touch restored",
                static_cast<unsigned long>(isolation.elapsedMs(now)));
            gesture = TouchGesture{};
            confirmationCardId[0] = '\0';
            lastInteractionMs = now;
            forceRender = true;
        }
#endif
        ITouchBackend* touch = device.touch();
        TouchPoint point;
        if (
#if FASTBEE_SCREEN_WORKER_ISOLATION == 1
            ScreenWorkerIsolation::allowTouch(isolationPhase) &&
#endif
            touch && touch->ready() && touch->poll(point)) {
            lastInteractionMs = now;
            if (sleeping) {
                if (point.pressed && !touchWakeContactDown && !touchWakePending) {
                    touchWakePending = true;
                    touchWakeAttempts = 0;
                    touchWakeNextAttemptMs = now;
                    LOGGER.infof("ScreenRuntime: touch wake requested x=%u y=%u", point.x, point.y);
                }
                touchWakeContactDown = point.pressed;
            } else if (gesture.suppressUntilRelease) {
                if (!point.pressed) gesture.suppressUntilRelease = false;
            } else if (point.pressed) {
                if (!gesture.down) {
                    gesture.down = true;
                    gesture.startX = point.x;
                    gesture.startY = point.y;
                    gesture.startedMs = now;
                    gesture.revision = snapshot->revision;
                    gesture.pageIndex = snapshot->activePageIndex;
                    gesture.tapTarget = renderer.tapTarget(*snapshot, point.x, point.y, confirmationCardId);
                    const int hit = confirmationCardId[0] ? -1 : renderer.hitTest(*snapshot, point.x, point.y);
                    renderer.setPressed(hit >= 0 ? snapshot->cards[hit].id : nullptr, point.x, point.y);
                    if (hit >= 0) forceRender = true;
                    if (hit >= 0 && snapshot->cards[hit].visualType == ScreenVisualType::SLIDER &&
                        renderer.actionHitTest(*snapshot, snapshot->cards[hit], point.x, point.y) &&
                        device.display()->capabilities().brightnessContinuous) {
                        const auto bounds = renderer.cardBounds(*snapshot, snapshot->cards[hit]);
                        if (point.y >= bounds.y + bounds.h - 48) { gesture.slider = hit; }
                    }
                    // Arm auto-repeat only for the two ± style controls. The
                    // first tick fires REPEAT_START_MS after the press; every
                    // stop path below resets the whole gesture, so an armed
                    // repeat can never outlive its contact.
                    if (hit >= 0 && !confirmationCardId[0] && gesture.slider < 0) {
                        auto& card = snapshot->cards[hit];
                        if (card.visualType == ScreenVisualType::MOTOR_CONTROL) {
                            const ScreenMotorLayout motorLayout{renderer.cardBounds(*snapshot, card)};
                            if (motorLayout.valid()) {
                                for (unsigned row = 0; row < 2 && gesture.repeatCard < 0; ++row) {
                                    for (unsigned col = 0; col < 2; ++col) {
                                        if (!screenRectContains(motorLayout.adjust(row, col), point.x, point.y)) continue;
                                        gesture.repeatCard = hit;
                                        gesture.repeatRow = row;
                                        gesture.repeatCol = col;
                                    }
                                }
                            }
                        } else if (card.visualType == ScreenVisualType::NUMBER_CONTROL) {
                            const NumericControlHit numericHit = renderer.numericControlHitTest(
                                *snapshot, card, point.x, point.y);
                            if (numericHit == NumericControlHit::DECREMENT ||
                                numericHit == NumericControlHit::INCREMENT) {
                                gesture.repeatCard = hit;
                                gesture.repeatCol = numericHit == NumericControlHit::INCREMENT ? 1 : 0;
                            }
                        }
                        if (gesture.repeatCard >= 0) gesture.repeatNextMs = now + REPEAT_START_MS;
                    }
                }
                gesture.lastX = point.x;
                gesture.lastY = point.y;
                if (!gesture.tapCancelled &&
                    (abs(static_cast<int>(point.x) - gesture.startX) > TAP_MOVE_PX ||
                     abs(static_cast<int>(point.y) - gesture.startY) > TAP_MOVE_PX ||
                     renderer.tapTarget(*snapshot, point.x, point.y, confirmationCardId) != gesture.tapTarget)) {
                    gesture.tapCancelled = true;
                    renderer.setPressed(nullptr);
                    forceRender = true;
                }
                // Auto-repeat ticks mirror the single-tap ± logic. A repeat
                // never fires across a cancelled/invalidated gesture or a
                // pending layout refresh, and one tick per poll keeps the
                // 20ms cadence bounded. Wrap-safe signed deadline compare
                // matches the buzzer deadline pattern above.
                if (gesture.repeatCard >= 0 && !gesture.tapCancelled &&
                    gesture.revision == snapshot->revision &&
                    gesture.pageIndex == snapshot->activePageIndex &&
                    !state.refreshRequested.load(std::memory_order_acquire) &&
                    static_cast<int32_t>(now - gesture.repeatNextMs) >= 0) {
                    gesture.repeatNextMs += REPEAT_INTERVAL_MS;
                    gesture.repeated = true;
                    auto& card = snapshot->cards[gesture.repeatCard];
                    const int multiplier =
                        now - gesture.startedMs > REPEAT_FAST_AFTER_MS ? REPEAT_FAST_MULTIPLIER : 1;
                    if (card.visualType == ScreenVisualType::MOTOR_CONTROL) {
                        if (!card.pending) {
                            uint16_t& value = gesture.repeatRow ? card.motorPulseInput : card.motorSpeedInput;
                            bool& dirty = gesture.repeatRow ? card.motorPulseDirty : card.motorSpeedDirty;
                            const int step = static_cast<int>(gesture.repeatRow ? card.motorPulseStep : card.motorSpeedStep) * multiplier;
                            value = std::max(0, std::min(65535, static_cast<int>(value) + (gesture.repeatCol ? step : -step)));
                            dirty = true; card.error[0] = '\0'; card.motorUiFeedback[0] = '\0';
                            controlSession.used(card.controlRef, now);
                            forceRender = true;
                        }
                    } else if (card.visualType == ScreenVisualType::NUMBER_CONTROL) {
                        const double direction = gesture.repeatCol ? 1.0 : -1.0;
                        const double next = std::max<double>(card.rangeMin,
                            std::min<double>(card.rangeMax,
                                static_cast<double>(card.numericValue) +
                                direction * card.numericStep * multiplier));
                        const double scale = std::pow(10.0, card.precision);
                        card.numericValue = static_cast<float>(std::round(next * scale) / scale);
                        forceRender = true;
                    }
                }
                if (gesture.slider >= 0 && now - gesture.sliderUpdatedMs >= 100U) {
                    auto& card = snapshot->cards[gesture.slider];
                    const auto rect = renderer.cardBounds(*snapshot, card);
                    const uint8_t value = std::max(0, std::min(100, (static_cast<int>(point.x) - rect.x - 20) * 100 / std::max<int>(1, rect.w - 40)));
                    if (device.display()->setBrightness(value)) {
                        appliedBrightness = value; card.numericValue = value;
                        brightnessOverridden = true; brightnessOverrideRevision = snapshot->revision;
                        forceRender = true; state.refreshRequested.store(true, std::memory_order_release);
                    }
                    gesture.sliderUpdatedMs = now;
                }
            } else if (gesture.down) {
                const int dx = static_cast<int>(gesture.lastX) - gesture.startX;
                const int dy = static_cast<int>(gesture.lastY) - gesture.startY;
                const uint32_t duration = now - gesture.startedMs;
                const int swipeThreshold = std::max<int>(
                    MIN_SWIPE_PX, renderer.width() / 7);
                if (gesture.slider >= 0) {
                    auto& card = snapshot->cards[gesture.slider];
                    const auto rect = renderer.cardBounds(*snapshot, card);
                    const uint8_t value = std::max(0, std::min(100, (static_cast<int>(gesture.lastX) - rect.x - 20) * 100 / std::max<int>(1, rect.w - 40)));
                    if (device.display()->setBrightness(value)) {
                        appliedBrightness = value; card.numericValue = value;
                        brightnessOverridden = true; brightnessOverrideRevision = snapshot->revision;
                    }
                    String saveError;
                    if (!ScreenPanelManager::instance().queueSetting("brightnessPercent", appliedBrightness, saveError)) {
                        strlcpy(card.error, saveError.c_str(), sizeof(card.error));
                        ScreenPanelManager::instance().reportApplication(false, false, saveError);
                    }
                } else if (!state.refreshRequested.load(std::memory_order_acquire) &&
                           gesture.revision == snapshot->revision && gesture.pageIndex == snapshot->activePageIndex &&
                           abs(dx) >= swipeThreshold && abs(dx) > abs(dy) * 3 / 2) {
                    renderer.closeNavigation();
                    if (!confirmationCardId[0]) changePage(*snapshot, dx < 0 ? 1 : -1);
                    confirmationCardId[0] = '\0';
                } else if (!gesture.tapCancelled && !gesture.repeated && gesture.tapTarget &&
                           !state.refreshRequested.load(std::memory_order_acquire) &&
                           gesture.revision == snapshot->revision && gesture.pageIndex == snapshot->activePageIndex &&
                           renderer.tapTarget(*snapshot, gesture.lastX, gesture.lastY, confirmationCardId) == gesture.tapTarget &&
                           abs(dx) <= TAP_MOVE_PX && abs(dy) <= TAP_MOVE_PX && duration <= TAP_MAX_MS) {
                    if (confirmationCardId[0] || renderer.feedbackHitTest(*snapshot, gesture.lastX, gesture.lastY)) touchFeedback(now);
                    handleTap(renderer, *snapshot, gesture.lastX, gesture.lastY,
                              confirmationCardId, sizeof(confirmationCardId), &controlSession, now);
                }
                renderer.setPressed(nullptr);
                gesture = TouchGesture{};
                forceRender = true;
                state.refreshRequested.store(true, std::memory_order_release);
            }
        }

        // Retry a touch wake after a short tap has already ended. A held
        // contact cannot restart the bounded budget after a permanent failure.
        if (touchWakePending && sleeping &&
            static_cast<int32_t>(now - touchWakeNextAttemptMs) >= 0) {
            ++touchWakeAttempts;
            const bool brightnessSet = device.display()->setBrightness(
                brightnessOverridden ? appliedBrightness : snapshot->brightnessPercent);
            if (brightnessSet && device.display()->setSleep(false)) {
                sleeping = false;
                touchWakePending = false;
                controlSession.reset();
                confirmationCardId[0] = '\0';
                gesture = TouchGesture{};
                gesture.suppressUntilRelease = touchWakeContactDown;
                lastInteractionMs = now;
                renderer.invalidate();
                updateRuntimeState(true, false,
                                   snapshotBytes + renderer.bufferBytes() + framebufferBytes);
                forceRender = true;
                LOGGER.infof("ScreenRuntime: touch wake applied attempt=%u", touchWakeAttempts);
            } else {
                touchWakePending = touchWakeAttempts < 3;
                touchWakeNextAttemptMs = now + 100U;
                LOG_WARNINGF("ScreenRuntime: touch wake failed stage=%s attempt=%u retry=%u",
                             brightnessSet ? "backlight-on" : "brightness",
                             touchWakeAttempts, touchWakePending ? 1U : 0U);
            }
        }
        if (!sleeping) {
            touchWakePending = false;
            touchWakeContactDown = false;
        }

        const bool requestedRefresh = state.refreshRequested.exchange(
            false, std::memory_order_acq_rel);
        if (requestedRefresh && gesture.down && gesture.slider < 0) {
            // Release snapshot ownership immediately after a saved layout.
            // Consuming the remaining contact must never dispatch its old target.
            gesture = TouchGesture{};
            gesture.suppressUntilRelease = true;
            renderer.setPressed(nullptr);
            confirmationCardId[0] = '\0';
            controlSession.reset();
            forceRender = true;
        }
#if FASTBEE_SCREEN_WORKER_ISOLATION == 1
        if (isolation.allowSnapshot(isolationPhase) &&
            (isolationPhaseFrame || requestedRefresh || now - lastSnapshotMs >= snapshotIntervalMs(*snapshot))) {
#else
        if (forceRender || (!gesture.down && (requestedRefresh || now - lastSnapshotMs >= snapshotIntervalMs(*snapshot)))) {
#endif
            // A failed refresh keeps the last good screen and retries at the
            // same bounded interval, rather than parsing at the touch rate.
            if (!gesture.down) lastSnapshotMs = now;
#if FASTBEE_SCREEN_PERF_TELEMETRY == 1
            const int64_t snapshotStarted = esp_timer_get_time();
#endif
            // Draw the captured button immediately; background status reads and
            // snapshot replacement must not compete with an ongoing gesture.
            const bool captured = gesture.down || ScreenPanelManager::instance().getActiveSnapshot(
                *snapshot, snapshotCache);
#if FASTBEE_SCREEN_PERF_TELEMETRY == 1
            const uint32_t snapshotElapsed = static_cast<uint32_t>(
                esp_timer_get_time() - snapshotStarted);
            if (!gesture.down) {
                ++snapshotCount;
                snapshotMicros += snapshotElapsed;
                snapshotMaxMicros = std::max(snapshotMaxMicros, snapshotElapsed);
            }
#endif
            if (captured) {
                if (controlSessionRevision != snapshot->revision) {
                    controlSession.reset(); confirmationCardId[0] = '\0';
                    controlSessionRevision = snapshot->revision;
                    renderer.closeNavigation();
                    renderer.invalidate();
                    lastInteractionMs = now;
                    appliedBrightness = 255;
                    forceRender = true;
                    // A saved layout should be visible without a reboot or a
                    // second save; the wake contact may not operate its controls.
                    if (sleeping && device.display()->setBrightness(snapshot->brightnessPercent) &&
                        device.display()->setSleep(false)) {
                        sleeping = false;
                        gesture = TouchGesture{};
                        gesture.suppressUntilRelease = true;
                        updateRuntimeState(true, false,
                            snapshotBytes + renderer.bufferBytes() + framebufferBytes);
                    }
                }
                if (!sleeping && !gesture.down && now - lastMotorReadMs >= 5000U) {
                    lastMotorReadMs = now;
                    const auto& page = snapshot->pages[snapshot->activePageIndex];
                    for (size_t i = page.firstCard; i < page.firstCard + page.cardCount; ++i) {
                        const auto& card = snapshot->cards[i];
                        if (card.visualType != ScreenVisualType::MOTOR_CONTROL || card.pending) continue;
                        uint32_t operation; String readError;
                        MotorControlService::instance().submit(String(card.controlRef), "readStatus", 0, "", operation, readError);
                    }
                }
                for (auto& card : snapshot->cards) {
                    screenExpireMotorFeedback(card, now);
                    if (card.kind == ScreenCardKind::SYSTEM) {
                        const auto caps = device.display()->capabilities();
                        const bool noBuzzer = !strcmp(card.systemActionId, "touch-beep") && !caps.buzzer;
                        const bool noWake = (!strcmp(card.systemActionId, "sleep") || !strcmp(card.systemActionId, "sleep-time")) &&
                            (!caps.sleep || !device.touch() || !device.touch()->ready());
                        if (noBuzzer || noWake) {
                            card.interactive = false;
                            strlcpy(card.error, noBuzzer ? "当前设备不支持提示音" : "当前设备不支持触摸唤醒", sizeof(card.error));
                        }
                    }
                    if (card.visualType == ScreenVisualType::SLIDER) {
                        card.numericValue = brightnessOverridden ? appliedBrightness : snapshot->brightnessPercent;
                        if (!device.display()->capabilities().brightnessContinuous) strlcpy(card.error, "仅支持开关背光", sizeof(card.error));
                    }
                    if (card.visualType == ScreenVisualType::MOTOR_CONTROL && !card.pending && card.motorKnown) {
                        if (card.motorSpeed == card.motorSpeedInput) card.motorSpeedDirty = false;
                        if (card.motorPulse == card.motorPulseInput) card.motorPulseDirty = false;
                        if (card.motorKnown && !card.motorSpeedDirty) card.motorSpeedInput = card.motorSpeed;
                        if (card.motorKnown && !card.motorPulseDirty) card.motorPulseInput = card.motorPulse;
                    }
                }
                idleSleepSeconds = snapshot->idleSleepSeconds;
                if (brightnessOverridden &&
                    snapshot->revision != brightnessOverrideRevision) {
                    brightnessOverridden = false;
                }
                if (!sleeping && !brightnessOverridden &&
                    appliedBrightness != snapshot->brightnessPercent) {
                    if (device.display()->setBrightness(snapshot->brightnessPercent)) {
                        appliedBrightness = snapshot->brightnessPercent;
                    }
                }
                const uint32_t hash = snapshotHash(*snapshot, confirmationCardId);
                // A retry that keeps failing for a while, or a touch stack
                // that lost its controller, is surfaced on screen so the
                // panel cannot be mistaken for "no data".
                const uint8_t faultFlags = static_cast<uint8_t>(
                    (renderRetryPending && now - renderFaultFirstMs > 5000U
                        ? SCREEN_FAULT_RENDER_RETRY : 0) |
                    (device.touchError().length() > 0 ? SCREEN_FAULT_TOUCH : 0));
                if (faultFlags != renderedFaultFlags) forceRender = true;
#if FASTBEE_SCREEN_WORKER_ISOLATION == 1
                if (isolation.allowRender(isolationPhase) &&
                    (isolationPhaseFrame || forceRender || hash != previousHash) &&
                    (!renderRetryPending || now - lastRenderAttemptMs >= 250U)) {
                    const uint8_t pageIndex = snapshot->pageCount == 0 ? 0 :
                        std::min<uint8_t>(snapshot->activePageIndex, snapshot->pageCount - 1U);
                    ScreenIsolationTitle<sizeof(snapshot->pages[0].title)> isolationTitle(
                        snapshot->pageCount ? snapshot->pages[pageIndex].title : nullptr,
                        ScreenWorkerIsolation::title(isolationPhase));
#else
                if ((forceRender || hash != previousHash) &&
                    (!renderRetryPending || now - lastRenderAttemptMs >= 250U)) {
#endif
#if FASTBEE_SCREEN_PERF_TELEMETRY == 1
                    const int64_t renderStarted = esp_timer_get_time();
#endif
                    if (!renderer.render(*snapshot, confirmationCardId, error, faultFlags)) {
                        if (!renderRetryPending) renderFaultFirstMs = now;
                        renderRetryPending = true;
                        lastRenderAttemptMs = millis();
                        LOG_WARNINGF("ScreenRuntime: render failed: %s", error.c_str());
                        updateRuntimeState(true, sleeping,
                            snapshotBytes + renderer.bufferBytes() + framebufferBytes, error);
                    } else {
                        renderRetryPending = false;
                        renderFaultFirstMs = 0;
                        renderedFaultFlags = faultFlags;
                        previousHash = hash;
#if FASTBEE_SCREEN_WORKER_ISOLATION == 1
                        isolation.rendered(isolationPhase);
#endif
                        updateRuntimeState(true, sleeping,
                            snapshotBytes + renderer.bufferBytes() + framebufferBytes);
                    }
#if FASTBEE_SCREEN_PERF_TELEMETRY == 1
                    const uint32_t renderElapsed = static_cast<uint32_t>(
                        esp_timer_get_time() - renderStarted);
                    ++renderCount;
                    renderMicros += renderElapsed;
                    renderMaxMicros = std::max(renderMaxMicros, renderElapsed);
#endif
                    forceRender = renderRetryPending;
                }
            }
        }

        // A panel without touch has no local wake source. Keep it visible
        // instead of allowing the default idle timeout to black it permanently.
        const bool canWakeLocally = device.touch() && device.touch()->ready();
        if (
#if FASTBEE_SCREEN_WORKER_ISOLATION == 1
            ScreenWorkerIsolation::allowTouch(isolationPhase) &&
#endif
            canWakeLocally && !sleeping && idleSleepSeconds > 0 &&
            now - lastInteractionMs >= idleSleepSeconds * 1000UL) {
            if (device.display()->setSleep(true)) {
                sleeping = true;
                controlSession.reset(); confirmationCardId[0] = '\0';
                updateRuntimeState(true, true,
                                   snapshotBytes + renderer.bufferBytes() + framebufferBytes);
            }
        }
#if FASTBEE_SCREEN_PERF_TELEMETRY == 1
        if (millis() - lastPerfReportMs >= 60000U) {
            // Candidate-only counters bypass the file logger. Never emit per
            // frame, which would add storage work to the path being measured.
            // （D5 白名单：性能遥测刻意绕过文件日志，保留 Serial 直出）
            Serial.printf("[SCRPERF] snapshots=%lu avg_us=%lu max_us=%lu renders=%lu avg_us=%lu max_us=%lu layout_builds=%lu cards=%u stack_free=%u internal=%u\n",
                static_cast<unsigned long>(snapshotCount),
                static_cast<unsigned long>(snapshotCount ? snapshotMicros / snapshotCount : 0),
                static_cast<unsigned long>(snapshotMaxMicros),
                static_cast<unsigned long>(renderCount),
                static_cast<unsigned long>(renderCount ? renderMicros / renderCount : 0),
                static_cast<unsigned long>(renderMaxMicros),
                static_cast<unsigned long>(snapshotCache.rebuildCount()),
                static_cast<unsigned>(snapshotCache.refreshedCardCount()),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            FastBeeRgbRecoveryStats rgbStats{};
            if (fastbee_rgb_recovery_get_stats(&rgbStats)) {
                // D5 白名单：同上，性能遥测保留 Serial 直出。
                Serial.printf("[RGBREC] vsync=%lu udf=%lu l1=%lu l3=%lu requested=%lu applied=%lu errors=%lu/%lu/%lu suppressed=%lu startup=%lu pending=%u in_flight=%u reason=%lu\n",
                    static_cast<unsigned long>(rgbStats.vsyncs),
                    static_cast<unsigned long>(rgbStats.udfEvents),
                    static_cast<unsigned long>(rgbStats.l1),
                    static_cast<unsigned long>(rgbStats.l3),
                    static_cast<unsigned long>(rgbStats.requested),
                    static_cast<unsigned long>(rgbStats.applied),
                    static_cast<unsigned long>(rgbStats.requestErrors),
                    static_cast<unsigned long>(rgbStats.resetErrors),
                    static_cast<unsigned long>(rgbStats.startErrors),
                    static_cast<unsigned long>(rgbStats.suppressed),
                    static_cast<unsigned long>(rgbStats.startupRequests),
                    static_cast<unsigned>(rgbStats.pending),
                    static_cast<unsigned>(rgbStats.inFlight),
                    static_cast<unsigned long>(rgbStats.lastReason));
            }
            lastPerfReportMs = millis();
            snapshotCount = renderCount = snapshotMaxMicros = renderMaxMicros = 0;
            snapshotMicros = renderMicros = 0;
        }
#endif
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TOUCH_POLL_MS));
    }

    // Best-effort repeated shutdown plus backend end() prevents a successful
    // buzzer-on write from being left latched by a stop/reconfigure request.
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        if (device.display()->setBuzzer(false)) break;
        if (attempt == 2 && device.display()->capabilities().buzzer)
            LOG_WARNING("ScreenRuntime: buzzer OFF unconfirmed at shutdown; backend cleanup will retry");
        delay(2);
    }
    renderer.end();
    snapshot->~ScreenPanelSnapshot();
    heap_caps_free(snapshotMemory);
}

void screenTask(void*) {
    // FreeRTOS task deletion does not unwind C++ stack objects. Return from
    // the worker first so its cached JSON and error strings release memory on
    // startup failure and every stop/reconfigure cycle.
    runScreenTask();
    {
        // Keep the task handle and backend alive until checked cleanup succeeds.
        // stop() can time out truthfully; a busy shared bus never frees a lease
        // owner or its GPIO from underneath an in-flight transaction.
        String cleanupError;
        {
            StateLock lock;
            if (lock.locked()) {
                runtimeState().running = false;
                runtimeState().systemActionCapabilities = {};
            }
        }
        while (!deactivateScreenDeviceResources(&cleanupError)) {
            {
                StateLock lock;
                if (lock.locked()) runtimeState().lastError = cleanupError;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    } // FreeRTOS deletion does not unwind cleanupError.
    finishTask();
}

}  // namespace

#endif

ScreenRuntime& ScreenRuntime::instance() {
    static ScreenRuntime runtime;
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    (void)runtimeState(); // Owner path constructs state, never an observer query.
#endif
    static const FeatureInstanceObserver observer{"screenPanel", [](const void* context) {
        return static_cast<const ScreenRuntime*>(context)->observeFeatureInstance();
    }, &runtime};
    FeatureInstanceRegistry::instance().registerObserver(&observer);
    return runtime;
}

void ScreenRuntime::snapshotBacklightResources(BacklightResourceSnapshot& result) {
    result = {};
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    const auto* state = runtimeObserverState.load(std::memory_order_acquire);
    if (!state) return; // Never construct the runtime or synchronization to observe.
    const auto mutex = state->observationMutex.load(std::memory_order_acquire);
    if (!mutex) return;
    if (xSemaphoreTake(mutex, 0) != pdTRUE) { result.status = ResourceObservation::Busy; return; }
    if (state->backlightChanging.load(std::memory_order_acquire)) result.status = ResourceObservation::Busy;
    else result = state->backlightResources;
    xSemaphoreGive(mutex);
#endif
}

FeatureInstanceSnapshot ScreenRuntime::observeFeatureInstance() const {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    auto& state = runtimeState();
    const auto mutex = state.observationMutex.load(std::memory_order_acquire);
    if (!mutex) return FeatureInstanceSnapshot::unavailable(
        FeatureInstanceReason::SynchronizationUnavailable, FeatureInstanceScope::ServiceLifecycle);
    if (xSemaphoreTake(mutex, 0) != pdTRUE)
        return FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Busy);
    FeatureInstanceSnapshot snapshot;
    if (state.running && !state.task) snapshot = FeatureInstanceSnapshot::unavailable(
        FeatureInstanceReason::InvalidSnapshot, FeatureInstanceScope::ServiceLifecycle);
    else snapshot = FeatureInstanceSnapshot::lifecycle(
        state.task && state.stopRequested.load(std::memory_order_acquire) ? FeatureInstanceState::Retiring :
        state.running ? FeatureInstanceState::Running :
        (state.task || state.allocated) ? FeatureInstanceState::Allocated : FeatureInstanceState::Stopped);
    xSemaphoreGive(mutex);
    return snapshot;
#else
    return FeatureInstanceSnapshot::unavailable(
        FeatureInstanceReason::BackendUnavailable, FeatureInstanceScope::ServiceLifecycle);
#endif
}

bool ScreenRuntime::start(String& error) {
#if !FASTBEE_ENABLE_SCREEN_UI || !defined(ARDUINO_ARCH_ESP32)
    error = "SCREEN_UI_NOT_COMPILED";
    return false;
#else
    RuntimeState& state = runtimeState();
    if (!ensureRuntimeSynchronization(state)) {
        error = "SCREEN_RUNTIME_SYNC_ALLOCATION_FAILED";
        return false;
    }
    {
        StateLock lock;
        if (!lock.locked()) {
            error = "SCREEN_RUNTIME_BUSY";
            return false;
        }
        if (state.running) {
            error = "";
            return true;
        }
        if (state.task) {
            error = "SCREEN_RUNTIME_START_IN_PROGRESS";
            return false;
        }
        if (FeatureBudgetRegistry::instance().isDenied("screenPanel")) {
            error = "SCREEN_PANEL_BUDGET_DENIED";
            state.lastError = error;
            return false;
        }
        if (!RuntimeFeatureManager::instance().shouldEnable("screenPanel")) {
            error = "SCREEN_PANEL_DISABLED";
            state.lastError = error;
            return false;
        }
        if (!screenMemoryAvailable(ScreenDevice::instance().config(), error)) {
            state.lastError = error;
            LOG_WARNINGF("ScreenRuntime: admission rejected: %s internal=%u largest=%u",
                error.c_str(),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            return false;
        }
        while (xSemaphoreTake(state.started, 0) == pdTRUE) {}
        state.stopRequested.store(false, std::memory_order_release);
        state.refreshRequested.store(true, std::memory_order_release);
        state.pendingSystemAction = ScreenSystemAction::NONE;
        state.pendingSystemValue = 0;
        state.startSucceeded = false;
        state.lastError = "";
        if (xTaskCreate(screenTask, "screen-ui", TASK_STACK_BYTES, nullptr,
                        TASK_PRIORITY, &state.task) != pdPASS) {
            state.task = nullptr;
            error = "SCREEN_RUNTIME_TASK_ALLOCATION_FAILED";
            state.lastError = error;
            return false;
        }
    }
    if (xSemaphoreTake(state.started, pdMS_TO_TICKS(START_TIMEOUT_MS)) != pdTRUE) {
        error = "SCREEN_RUNTIME_START_TIMEOUT";
        stop();
        return false;
    }
    {
        StateLock lock;
        if (!lock.locked()) {
            error = "SCREEN_RUNTIME_BUSY";
            return false;
        }
        error = state.lastError;
        return state.startSucceeded;
    }
#endif
}

bool ScreenRuntime::stop(String* error) {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    RuntimeState& state = runtimeState();
    if (!state.mutex) {
        if (error) *error = "";
        return true;
    }
    TaskHandle_t task = nullptr;
    {
        StateLock lock;
        if (!lock.locked()) {
            if (error) *error = "SCREEN_RUNTIME_BUSY";
            return false;
        }
        state.stopRequested.store(true, std::memory_order_release);
        state.systemActionCapabilities = {};
        task = state.task;
        if (task) xTaskNotifyGive(task);
    }
    const uint32_t startedAt = millis();
    while (task && millis() - startedAt < STOP_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        StateLock lock;
        if (lock.locked()) task = state.task;
    }
    if (task) {
        StateLock lock;
        if (lock.locked()) state.lastError = "SCREEN_RUNTIME_STOP_TIMEOUT";
        if (error) *error = "SCREEN_RUNTIME_STOP_TIMEOUT";
        return false;
    }
    if (error) *error = "";
    return true;
#else
    if (error) *error = "";
    return true;
#endif
}

void ScreenRuntime::requestRefresh() {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    RuntimeState& state = runtimeState();
    state.refreshRequested.store(true, std::memory_order_release);
    StateLock lock;
    TaskHandle_t task = lock.locked() ? state.task : nullptr;
    if (task) xTaskNotifyGive(task);
#endif
}

bool ScreenRuntime::requestSystemAction(ScreenSystemAction action, uint8_t value,
                                        String& error) {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    if (action == ScreenSystemAction::NONE ||
        (action == ScreenSystemAction::SET_BRIGHTNESS && value > 100)) {
        error = "SCREEN_SYSTEM_ACTION_INVALID";
        return false;
    }
    RuntimeState& state = runtimeState();
    StateLock lock;
    if (!lock.locked()) {
        error = "SCREEN_RUNTIME_BUSY";
        return false;
    }
    if (!state.running || !state.task) {
        error = "SCREEN_RUNTIME_NOT_RUNNING";
        return false;
    }
    const DisplayCapabilities caps = ScreenDevice::instance().capabilities();
    if ((action == ScreenSystemAction::SET_BRIGHTNESS && !caps.brightness) ||
        (action == ScreenSystemAction::SLEEP && !caps.sleep) ||
        (action == ScreenSystemAction::BUZZER && !caps.buzzer)) {
        error = "SCREEN_SYSTEM_ACTION_UNSUPPORTED";
        return false;
    }
    if (action == ScreenSystemAction::SLEEP) {
        auto* touch = ScreenDevice::instance().touch();
        if (!touch || !touch->ready()) {
            error = "SCREEN_SLEEP_REQUIRES_TOUCH_WAKE";
            return false;
        }
    }
    if (state.pendingSystemAction != ScreenSystemAction::NONE) {
        error = "SCREEN_SYSTEM_ACTION_BUSY";
        return false;
    }
    state.pendingSystemValue = value;
    state.pendingSystemAction = action;
    xTaskNotifyGive(state.task);
    error = "";
    return true;
#else
    (void)action;
    (void)value;
    error = "SCREEN_UI_NOT_COMPILED";
    return false;
#endif
}

ScreenSystemActionCapabilities ScreenRuntime::systemActionCapabilities() const {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    StateLock lock;
    const RuntimeState& state = runtimeState();
    return lock.locked() && state.running && state.task
        ? state.systemActionCapabilities
        : ScreenSystemActionCapabilities{};
#else
    return {};
#endif
}

bool ScreenRuntime::running() const {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    StateLock lock;
    return lock.locked() && runtimeState().running;
#else
    return false;
#endif
}

bool ScreenRuntime::sleeping() const {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    StateLock lock;
    return lock.locked() && runtimeState().sleeping;
#else
    return false;
#endif
}

size_t ScreenRuntime::allocatedBytes() const {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    StateLock lock;
    return lock.locked() ? runtimeState().allocated : 0;
#else
    return 0;
#endif
}

void ScreenRuntime::getLastError(String& out) const {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    StateLock lock;
    out = lock.locked() ? runtimeState().lastError : "SCREEN_RUNTIME_BUSY";
#else
    out = "SCREEN_UI_NOT_COMPILED";
#endif
}

}  // namespace Screen
}  // namespace FastBee
