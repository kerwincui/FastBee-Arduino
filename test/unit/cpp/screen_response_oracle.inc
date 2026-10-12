void appendPanelClock(JsonObject clock) {
    const time_t now = time(nullptr);
    struct tm local {};
    bool valid = now >= 1609459200;
    if (valid) {
#if defined(_WIN32)
        valid = localtime_s(&local, &now) == 0;
#else
        valid = localtime_r(&now, &local) != nullptr;
#endif
    }
    // A valid device wall clock may come from NTP or another existing time
    // source. Do not claim a fresh NTP exchange or substitute browser time.
    clock["valid"] = valid;
    clock["quality"] = valid ? "good" : "unknown";
    if (!valid) return;
    char timeText[8];
    char dateText[16];
    strftime(timeText, sizeof(timeText), "%H:%M", &local);
    strftime(dateText, sizeof(dateText), "%Y-%m-%d", &local);
    clock["time"] = JsonString(timeText);
    clock["date"] = JsonString(dateText);
}

void ScreenPanelManager::appendCapabilities(JsonObject out) const {
    FastBee::Screen::ScreenSystemActionCapabilities actionCapabilities{};
#if FASTBEE_ENABLE_SCREEN_UI
    actionCapabilities =
        FastBee::Screen::ScreenRuntime::instance().systemActionCapabilities();
#endif
    out["schemaVersion"] = 1;
    out["maxPages"] = ScreenPanelSnapshot::MAX_PAGES;
    out["maxCards"] = ScreenPanelSnapshot::MAX_CARDS;
    out["maxHistorySamples"] = ScreenPanelSnapshot::MAX_HISTORY;
    out["maxSourcesPerCard"] = 4;
    out["templates"] = true;
    out["canvas"] = true;
    out["motorControls"] = FASTBEE_ENABLE_MODBUS != 0;
    out["touchSettings"] = true;
    out["charts"] = true;
    out["systemRestart"] = actionCapabilities.restart;
    out["brightness"] = actionCapabilities.brightness;
    out["brightnessContinuous"] = actionCapabilities.brightnessContinuous;
    out["minVisibleBrightnessPercent"] = actionCapabilities.minVisibleBrightnessPercent;
    out["sleep"] = actionCapabilities.sleep;
    out["touchWake"] = actionCapabilities.touchWake;
    out["buzzer"] = actionCapabilities.buzzer;
    out["touchBeep"] = actionCapabilities.buzzer;
    out["displayReady"] = actionCapabilities.displayReady;
    out["touchConfigured"] = actionCapabilities.touchConfigured;
    out["touchReady"] = actionCapabilities.touchReady;
    out["touchError"] = actionCapabilities.touchError;
    out["controlConfirmIdle"] = FASTBEE_ENABLE_SCREEN_UI != 0;
    out["maxBottomNavPages"] = ScreenPanelSnapshot::MAX_BOTTOM_NAV_PAGES;
    JsonObject disabledReasons = out["disabledReasons"].to<JsonObject>();
    String interactionReason;
    if (!FASTBEE_ENABLE_SCREEN_UI) interactionReason = "当前固件未启用屏幕运行时";
    else if (!actionCapabilities.displayReady) interactionReason = "显示设备尚未就绪，请检查显示外设";
    else if (!actionCapabilities.touchConfigured) interactionReason = "未配置触摸控制器，请检查显示外设";
    else if (!actionCapabilities.touchReady) {
        interactionReason = "触摸控制器未就绪，正在重试";
        if (actionCapabilities.touchError[0]) interactionReason += String("（") + actionCapabilities.touchError + "）";
    }
    if (!actionCapabilities.brightness) disabledReasons["brightness"] = interactionReason.isEmpty() ? String("当前硬件不支持亮度调节") : interactionReason;
    if (!actionCapabilities.brightnessContinuous) disabledReasons["brightnessContinuous"] = disabledReasons["brightness"];
    if (actionCapabilities.brightness && !actionCapabilities.brightnessContinuous) disabledReasons["brightnessContinuous"] = "仅支持背光开关";
    if (!actionCapabilities.sleep) disabledReasons["sleep"] = interactionReason.isEmpty() ? String("当前硬件不支持自动息屏") : interactionReason;
    if (!actionCapabilities.touchWake) disabledReasons["touchWake"] = interactionReason;
    if (!actionCapabilities.buzzer) disabledReasons["buzzer"] = interactionReason.isEmpty() ? String("显示外设未配置可用的蜂鸣器") : interactionReason;
    if (!actionCapabilities.restart) disabledReasons["systemRestart"] = interactionReason;
    if (!(FASTBEE_ENABLE_MODBUS != 0)) disabledReasons["motorControls"] = "当前固件不支持电机控制";
    JsonArray systemActions = out["systemActions"].to<JsonArray>();
    if (actionCapabilities.brightness) systemActions.add("brightness");
    if (actionCapabilities.sleep) { systemActions.add("sleep"); systemActions.add("sleep-time"); }
    if (actionCapabilities.buzzer) systemActions.add("touch-beep");
    if (FASTBEE_ENABLE_SCREEN_UI) systemActions.add("confirm-idle");
    if (actionCapabilities.buzzer) systemActions.add("buzzer");
    if (actionCapabilities.restart) systemActions.add("restart");
    out["requiresUniqueCardIds"] = true;
    out["allowsOverlap"] = false;
}

void ScreenPanelManager::appendApplication(JsonObject out) const {
    {
        PanelLock lock(_mutex);
        if (!lock.locked()) return;
        out["persisted"] = _persisted;
        out["applied"] = _applied;
        out["restartRequired"] = _restartRequired;
        out["revision"] = _revision;
        out["status"] = !_persisted ? "draft" : _applied ? "applied" :
                        _applicationError.isEmpty() ? "saved" : "apply_failed";
        if (!_applicationError.isEmpty()) out["error"] = _applicationError;
    }
#if FASTBEE_ENABLE_SCREEN_UI
    auto& runtime = FastBee::Screen::ScreenRuntime::instance();
    JsonObject state = out["runtime"].to<JsonObject>();
    state["running"] = runtime.running();
    state["sleeping"] = runtime.sleeping();
    state["allocatedBytes"] = runtime.allocatedBytes();
    String runtimeError;
    runtime.getLastError(runtimeError);
    if (!runtimeError.isEmpty()) state["error"] = runtimeError;
    state["internalFreeBytes"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    state["internalLargestBytes"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    state["psramFreeBytes"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

void ScreenPanelManager::appendState(JsonObject out) const {
    std::unique_ptr<ScreenPanelSnapshot> snapshot(
        new (std::nothrow) ScreenPanelSnapshot());
    if (!snapshot || !getSnapshot(*snapshot)) {
        out["available"] = false;
        return;
    }
    out["available"] = true;
    out["revision"] = snapshot->revision;
    out["activePageIndex"] = snapshot->activePageIndex;
    out["brightnessPercent"] = snapshot->brightnessPercent;
    out["idleSleepSeconds"] = snapshot->idleSleepSeconds;
    out["touchBeepEnabled"] = snapshot->touchBeepEnabled;
    out["controlConfirmIdleSeconds"] = snapshot->controlConfirmIdleSeconds;
    appendPanelClock(out["clock"].to<JsonObject>());
    JsonObject items = out["items"].to<JsonObject>();
    JsonArray cards = out["cards"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot->cardCount; ++i) {
        const auto& card = snapshot->cards[i];
        JsonObject item = cards.add<JsonObject>();
        // const char[N] is treated as a static literal by ArduinoJson 7.4.
        // This snapshot dies before the response is serialized, so copy text.
        item["id"] = JsonString(card.id);
        item["sourceRef"] = JsonString(card.sourceRef);
        item["controlRef"] = JsonString(card.controlRef);
        item["value"] = JsonString(card.value);
        item["unit"] = JsonString(card.unit);
        item["quality"] = qualityName(card.quality);
        item["error"] = JsonString(card.error);
        item["updatedMs"] = card.updatedMs;
        item["pending"] = card.pending;
        item["operationId"] = card.operationId;
        item["motorSpeed"] = card.motorSpeed; item["motorPulse"] = card.motorPulse;
        item["motorPrecision"] = card.motorPrecision;
        item["motorKnown"] = card.motorKnown; item["systemActionId"] = JsonString(card.systemActionId);
        if (card.visualType == ScreenVisualType::MOTOR_CONTROL) {
            item["motorCommandKnown"] = card.motorCommandKnown; item["motorCommandSuccess"] = card.motorCommandSuccess;
            item["motorCommandUpdatedMs"] = card.motorCommandUpdatedMs;
            item["motorReadError"] = JsonString(card.motorReadError);
            item["motorSoftLimitEnabled"] = card.motorSoftLimitEnabled;
            item["motorReferenceKnown"] = card.motorReferenceKnown;
            item["motorStationaryConfirmed"] = card.motorStationaryConfirmed;
            item["motorForwardReady"] = card.motorForwardReady; item["motorReverseReady"] = card.motorReverseReady;
        }
        item["historyCount"] = card.historyCount;
        item["historyStale"] = card.historyStale;
        item["historyError"] = JsonString(card.historyError);
        item["seriesCount"] = card.seriesCount;

        const char* stateRef = card.controlRef[0] ? card.controlRef : card.sourceRef;
        if (!stateRef[0]) continue;
        JsonObject state = items[stateRef].to<JsonObject>();
        state["cardId"] = JsonString(card.id);
        state["motorSpeed"] = card.motorSpeed; state["motorPulse"] = card.motorPulse;
        state["motorPrecision"] = card.motorPrecision;
        state["motorKnown"] = card.motorKnown;
        if (card.visualType == ScreenVisualType::MOTOR_CONTROL) {
            state["motorCommandKnown"] = card.motorCommandKnown; state["motorCommandSuccess"] = card.motorCommandSuccess;
            state["motorCommandUpdatedMs"] = card.motorCommandUpdatedMs;
            state["motorReadError"] = JsonString(card.motorReadError);
            state["motorSoftLimitEnabled"] = card.motorSoftLimitEnabled;
            state["motorReferenceKnown"] = card.motorReferenceKnown;
            state["motorStationaryConfirmed"] = card.motorStationaryConfirmed;
            state["motorForwardReady"] = card.motorForwardReady; state["motorReverseReady"] = card.motorReverseReady;
        }
        state["value"] = JsonString(card.value);
        state["unit"] = JsonString(card.unit);
        state["quality"] = qualityName(card.quality);
        state["error"] = JsonString(card.error);
        state["updatedMs"] = card.updatedMs;
        state["pending"] = card.pending;
        state["operationId"] = card.operationId;
        state["historyStale"] = card.historyStale;
        if (card.historyError[0]) state["historyError"] = JsonString(card.historyError);
        if (card.kind == ScreenCardKind::WEATHER) state["reason"] = JsonString(card.weatherReason);
        if (card.weatherCondition[0]) {
            state["condition"] = JsonString(card.weatherCondition);
            state["city"] = JsonString(card.weatherCity);
            if (card.hasWeatherTemperature) {
                state["temperature"] = card.weatherTemperature;
            }
        }
        if (card.seriesCount) {
            JsonArray series = state["series"].to<JsonArray>();
            for (uint8_t seriesIndex = 0; seriesIndex < card.seriesCount;
                 ++seriesIndex) {
                const auto& source = card.series[seriesIndex];
                JsonObject seriesItem = series.add<JsonObject>();
                seriesItem["sourceRef"] = JsonString(source.sourceRef);
                seriesItem["label"] = JsonString(source.label);
                seriesItem["unit"] = JsonString(source.unit);
                seriesItem["value"] = JsonString(source.value);
                seriesItem["quality"] = qualityName(source.quality);
                seriesItem["updatedMs"] = source.updatedMs;
            }
            // Per-card series remain distinct when two charts share a primary
            // source but compare different additional sources.
            item["series"] = series;
        }
    }
}
