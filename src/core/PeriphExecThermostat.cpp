#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#include "core/PeripheralManager.h"
#include "core/PeripheralActionContract.h"
#include "core/PeriphExecExecutor.h"
#include "core/SensorConfigEpoch.h"
#include "core/DriverRegistry.h"
#include "core/SensorConfigSupport.h"
#include "core/FastBeeFramework.h"
#include "core/MemoryBudget.h"
#include "systems/HealthMonitor.h"
#include <esp_timer.h>
#include <cstdlib>
#include <cerrno>

namespace {
bool thermostatSourceScaling(JsonObjectConst parameters) {
    for (const char* key : {"scaleFactor", "offset"}) {
        // An omitted field keeps the sampler's identity defaults. Explicit
        // bad types must not silently become 1/0 through JsonVariant::operator|.
        if (!parameters.containsKey(key)) continue;
        const auto value = parameters[key];
        if (value.is<bool>() || !value.is<double>() ||
            !std::isfinite(value.as<double>()) || !std::isfinite(value.as<float>())) return false;
    }
    return true;
}
bool thermostatTemperatureSource(const PeripheralConfig& peripheral,
                                JsonObjectConst parameters, String& field) {
    if (!peripheral.enabled) return false;
    // The configured driver overrides the action's legacy category at the
    // actual sampling entry point. A Celsius label cannot turn its humidity,
    // pressure, motion or electrical channel into a temperature channel.
    const bool configured = peripheral.type == PeripheralType::SENSOR_GENERIC &&
        peripheral.params.sensor.driver[0];
    const char* rawCategory = configured ? peripheral.params.sensor.driver :
        (parameters["sensorCategory"] | "analog");
    String category = rawCategory;
    if (category.length() != std::strlen(rawCategory)) return false;
    category.trim(); category.toLowerCase();
    if (!configured) {
        if (category == "dht11" || category == "dht22") return field == "temperature";
        // Retain explicitly labelled analogue/linear temperature conversion
        // and dedicated temperature readers. Physical calibration is separate.
        if (category == "analog" || category == "voltage" || category == "current" ||
            category == "ntc" || category == "ds18b20") return true;
        if (category == "current_loop" || category == "4-20ma" || category == "4_20ma")
            return parameters["mapToEngineering"] | (parameters["engineeringMapping"] | false);
        if (peripheral.type != PeripheralType::I2C &&
            peripheral.type != PeripheralType::SENSOR_GENERIC) return false;
    }
    const auto* metadata = FastBee::DriverRegistry::getInstance().getMetadata(category.c_str());
    if (!metadata) return false;
    uint8_t mode = configured ? peripheral.params.sensor.mode : 0;
    if (!configured) {
        JsonDocument legacy;
        const auto supplied = parameters["driverParams"];
        if (supplied.is<const char*>()) {
            if (deserializeJson(legacy, supplied.as<const char*>()) || !legacy.is<JsonObject>()) return false;
        } else if (supplied.is<JsonObjectConst>()) {
            if (!legacy.set(supplied)) return false;
        } else if (!supplied.isNull()) return false;
        // Legacy sampling feeds driverParams through parseSensorConfig.
        // Share its accepted numeric/string aliases and strict rejection.
        if (!FastBee::parseSensorMode(legacy["mode"], 0, mode)) return false;
    }
    String resolved, error;
    const char* requested = parameters["channelField"] | "";
    if (*requested) {
        if (!FastBee::resolveSensorChannel(category.c_str(), mode, requested, resolved, error)) return false;
    } else {
        requested = parameters["dataField"] | "";
        if (!*requested || !FastBee::resolveSensorChannel(category.c_str(), mode, requested, resolved, error))
            if (!FastBee::resolveSensorChannel(category.c_str(), mode, "", resolved, error)) return false;
    }
    // Match the physical cache key, including the existing property-alias
    // fallback to the driver's default channel, before comparing sourceRef.
    field = resolved;
    if (field.isEmpty() || field != resolved) return false;
    for (uint8_t i = 0; i < metadata->activeChannelCount(mode); ++i) {
        const auto& channel = *metadata->getChannel(i, mode);
        if (field == channel.name)
            return !std::strcmp(channel.unit, "°C") || !std::strcmp(channel.unit, "℃") ||
                !std::strcmp(channel.unit, "C");
    }
    return false;
}
uint64_t thermostatNow() {
    const int64_t micros = esp_timer_get_time();
    return micros < 0 ? 0 : static_cast<uint64_t>(micros) / 1000;
}
bool temperatureNumber(const ObservationSnapshot& sample, double& value) {
    if (!sample.hasValue || !sample.bindingValid || sample.stale ||
        sample.quality != ObservationQuality::GOOD || sample.valueType != "number" ||
        sample.sourceRevision.isEmpty() || sample.sampledAtMs == UINT64_MAX ||
        (sample.unit != "°C" && sample.unit != "℃" && sample.unit != "C") ||
        sample.value.length() > 32 || sample.value.length() != std::strlen(sample.value.c_str())) return false;
    const char* start = sample.value.c_str();
    if (!*start || *start == ' ' || *start == '\t' || *start == '\n') return false;
    char* end = nullptr;
    errno = 0;
    value = std::strtod(start, &end);
    return errno != ERANGE && end != start && end && *end == '\0' && std::isfinite(value) &&
        value >= -55 && value <= 125;
}
}

bool PeriphExecManager::thermostatSourceBindingLocked(const String& sourceRef, String& binding) const {
    binding = "";
    bool found = false;
    for (const auto& entry : rules) {
        for (const auto& action : entry.second.actions) {
            if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) continue;
            JsonDocument doc;
            if (deserializeJson(doc, action.actionValue.c_str()) || !doc.is<JsonObject>()) return false;
            String id = doc["periphId"] | "";
            if (id.isEmpty()) id = action.targetPeriphId;
            const String channel = doc["channelField"] | "";
            String field = channel.isEmpty() ? PeriphExecExecutor::resolveChannelField(
                doc["sensorCategory"] | "analog", id, doc["dataField"] | "") : channel;
            PeripheralConfig source;
            const bool temperatureSource = PeripheralManager::getInstance().getPeripheralSnapshot(id, source) &&
                thermostatTemperatureSource(source, doc.as<JsonObjectConst>(), field);
            const String ref = id + "_" + field;
            if (ref != sourceRef) continue;
            const String unit = doc["unit"] | "";
            // Disabled rules may still be explicitly executed. A second saved
            // writer of this cache key is ambiguous even when its timer is off.
            if (found || !entry.second.enabled || (unit != "°C" && unit != "℃" && unit != "C") ||
                action.useReceivedValue || id != action.targetPeriphId || !temperatureSource ||
                !thermostatSourceScaling(doc.as<JsonObjectConst>())) return false;
            const size_t length = entry.first.length() + action.targetPeriphId.length() + action.actionValue.length() + 2;
            const String fingerprint = entry.first + ":" + action.targetPeriphId + ":" + action.actionValue;
            if (fingerprint.length() != length) return false;
            binding = fingerprint;
            if (binding != fingerprint) return false;
            found = true;
        }
    }
    return found;
}

void PeriphExecManager::retireThermostatsLocked() {
    for (auto& controller : _thermostats) {
        if (!controller) continue;
        const auto current = rules.find(controller->ruleId);
        String error;
        if (!_thermostatShutdown.load(std::memory_order_acquire) && !controller->retiring && current != rules.end() && current->second.enabled &&
            FastBee::hasThermostatAction(current->second) &&
            FastBee::validateThermostatRule(current->second, error) &&
            current->second.actions[0].targetPeriphId == controller->targetId &&
            current->second.actions[0].actionValue == controller->configurationText) {
            // Unrelated graph changes cannot rearm a manually latched fault or
            // reset a dwell timer. Keep the same output owner and controller.
            controller->ruleRevision = _ruleRuntimeRevision;
            String binding;
            if (!thermostatSourceBindingLocked(controller->config.sourceRef, binding) || binding != controller->sourceBinding) {
                const uint64_t now = thermostatNow();
                controller->controller.fault(now, FastBee::ThermostatPhase::SourceChanged, true);
                controller->outputConfirmed = controller->output.high();
                if (controller->outputConfirmed) controller->controller.confirm(false, now);
                else controller->controller.outputFailure(now);
            }
            continue;
        }
        controller->retiring = true;
        controller->outputConfirmed = controller->output.end();
        // Failed release retains the claim and evidence. Never report safe or
        // destroy this owner until the HIGH pad feedback has been confirmed.
        if (controller->outputConfirmed) controller.reset();
        else controller->controller.outputFailure(thermostatNow());
    }
}

bool PeriphExecManager::shutdownThermostats() {
    _thermostatShutdown.store(true, std::memory_order_release);
    if (!_rulesMutex) return true;
    MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(2000));
    if (!lock.isLocked()) return false;
    retireThermostatsLocked();
    retireSdRecorderLocked();
    for (const auto& controller : _thermostats) if (controller) return false;
    return true;
}

void PeriphExecManager::checkThermostats() {
    if (!_rulesMutex) return;
    const uint32_t tick = millis();
    if (static_cast<uint32_t>(tick - _lastThermostatCheck) < 250) return;
    MutexGuard rulesLock(_rulesMutex, 0);
    if (!rulesLock.isLocked()) return;
    _lastThermostatCheck = tick;
    const uint64_t now = thermostatNow();
    auto* framework = FastBeeFramework::getInstance();
    auto* health = framework ? framework->getHealthMonitor() : nullptr;
    const bool memoryUnsafe = (health && health->getMemoryGuardLevel() >= MemoryGuardLevel::SEVERE) ||
        FastBee::MemoryBudget::dram8Free() < FastBee::MemoryBudget::PERIPH_EXEC_MIN_DRAM;
    const bool configurationUnsafe = _configurationLoadFailed || _configurationRestartPending ||
        _ruleRuntimeRevision == UINT64_MAX || _thermostatShutdown.load(std::memory_order_acquire);
    auto& peripherals = PeripheralManager::getInstance();
    for (auto& slot : _thermostats) {
        if (!slot) continue;
        const auto rule = rules.find(slot->ruleId);
        if (_thermostatShutdown.load(std::memory_order_acquire) || slot->retiring || rule == rules.end() || !rule->second.enabled ||
            rule->second.runtimeRevision != slot->ruleRevision) {
            slot->retiring = true;
            slot->outputConfirmed = slot->output.end();
            if (slot->outputConfirmed) slot.reset();
            else slot->controller.outputFailure(now);
        }
    }
    // No allocation/claim retry under memory pressure or for an already failed
    // admission. A later explicit graph mutation is needed to try a new session.
    if (!memoryUnsafe && !configurationUnsafe) for (const auto& entry : rules) {
        const auto& rule = entry.second;
        if (!rule.enabled || !FastBee::hasThermostatAction(rule)) continue;
        bool found = false;
        std::unique_ptr<FastBee::ThermostatRuntime>* freeSlot = nullptr;
        for (auto& slot : _thermostats) {
            if (slot && slot->ruleId == rule.id) found = true;
            if (!slot && !freeSlot) freeSlot = &slot;
        }
        if (found || !freeSlot) continue;
        String error;
        if (!FastBee::validateThermostatRule(rule, error)) continue;
        std::unique_ptr<FastBee::ThermostatRuntime> candidate(new (std::nothrow) FastBee::ThermostatRuntime());
        if (!candidate) continue;
        candidate->ruleId = rule.id;
        candidate->targetId = rule.actions[0].targetPeriphId;
        candidate->configurationText = rule.actions[0].actionValue;
        candidate->ruleRevision = rule.runtimeRevision;
        candidate->checkedAtMs = now;
        if (candidate->ruleId != rule.id || candidate->targetId != rule.actions[0].targetPeriphId ||
            candidate->configurationText != rule.actions[0].actionValue ||
            !FastBee::parseThermostatConfig(rule.actions[0].actionValue, candidate->config, error)) continue;
        candidate->controller.begin(now);
        RecursiveMutexGuard admissionLock(peripherals.getMutex(), 0);
        if (!admissionLock.isLocked() || _thermostatShutdown.load(std::memory_order_acquire)) continue;
        candidate->sourceConfigEpoch = FastBee::SensorConfigEpoch::current();
        const bool sourceBound = thermostatSourceBindingLocked(candidate->config.sourceRef, candidate->sourceBinding);
        candidate->admitted = candidate->output.begin(rule.actions[0].targetPeriphId);
        candidate->outputConfirmed = candidate->admitted;
        if (!candidate->admitted) candidate->controller.outputFailure(now);
        else if (!sourceBound) candidate->controller.fault(now, FastBee::ThermostatPhase::SourceChanged, true);
        *freeSlot = std::move(candidate);
    }
    for (auto& slot : _thermostats) {
        if (!slot || slot->retiring) continue;
        if (!slot->admitted) {
            // A Claim may reserve the pin before its initial HIGH readback
            // fails. Retire only that partial claim, never re-Claim or drive
            // LOW. Keep the failed session so success cannot auto-rearm it.
            if (slot->output.pin() >= 0) {
                slot->checkedAtMs = now;
                slot->outputConfirmed = slot->output.end();
                if (slot->outputConfirmed) slot->controller.confirm(false, now);
            }
            continue;
        }
        // Keep configuration, sensor binding checks and GPIO write in one
        // zero-wait peripheral lock. All claim commands independently recheck
        // type, lifecycle, pin, ownership and GPIO feedback too.
        RecursiveMutexGuard configLock(peripherals.getMutex(), 0);
        if (!configLock.isLocked()) {
            slot->controller.outputFailure(now);
            slot->outputConfirmed = false;
            continue;
        }
        slot->checkedAtMs = now;
        if (!FastBee::SensorConfigEpoch::matches(slot->sourceConfigEpoch))
            slot->controller.fault(now, FastBee::ThermostatPhase::SourceChanged, true);
        if (!slot->output.owned() || slot->output.isHigh() == slot->controller.on())
            slot->controller.outputFailure(now);
        FastBee::ThermostatSample input;
        ObservationSnapshot observation;
        if (getObservation(slot->config.sourceRef, observation)) {
            input.sampledAtMs = observation.sampledAtMs;
            input.valid = temperatureNumber(observation, input.value);
            if (input.valid) {
                if (slot->sourceRevision.isEmpty()) {
                    slot->sourceRevision = observation.sourceRevision;
                    if (slot->sourceRevision != observation.sourceRevision) slot->controller.outputFailure(now);
                } else if (slot->sourceRevision != observation.sourceRevision) {
                    slot->controller.fault(now, FastBee::ThermostatPhase::SourceChanged, true);
                }
            }
        }
        if (memoryUnsafe) slot->controller.fault(now, FastBee::ThermostatPhase::MemorySuspended);
        if (configurationUnsafe) slot->controller.fault(now, FastBee::ThermostatPhase::SourceChanged, true);
        const bool wanted = memoryUnsafe || configurationUnsafe || _thermostatShutdown.load(std::memory_order_acquire)
            ? false : slot->controller.decide(slot->config, input, now);
        if (wanted != slot->controller.on() || !slot->outputConfirmed || slot->controller.faulted()) {
            const bool confirmed = wanted ? slot->output.low() : slot->output.high();
            slot->outputConfirmed = confirmed;
            if (confirmed) slot->controller.confirm(wanted, now);
            else {
                slot->controller.outputFailure(now);
                // One bounded inactive-level recovery attempt. Original fault
                // remains latched even if this attempt succeeds.
                slot->outputConfirmed = slot->output.high();
                if (slot->outputConfirmed) slot->controller.confirm(false, now);
            }
        }
    }
}

bool PeriphExecManager::getThermostatStatus(const PeriphExecRule& rule, ActionExecResult& result) {
    result.targetPeriphId = rule.actions.empty() ? "" : rule.actions[0].targetPeriphId;
    MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(20));
    if (!lock.isLocked()) { result.remark = "THERMOSTAT_STATUS_BUSY"; return false; }
    const auto current = rules.find(rule.id);
    if (current == rules.end() || !current->second.enabled || current->second.runtimeRevision != rule.runtimeRevision) {
        result.remark = "THERMOSTAT_RULE_CHANGED_OR_DISABLED"; return false;
    }
    for (const auto& slot : _thermostats) {
        if (!slot || slot->ruleId != rule.id || slot->ruleRevision != rule.runtimeRevision) continue;
        result.remark = slot->retiring ? "THERMOSTAT_RETIRING_OUTPUT_UNCONFIRMED" :
            FastBee::thermostatPhaseName(slot->controller.phase());
        result.actualValue = slot->outputConfirmed ? (slot->controller.on() ? "ON_PAD_LOW" : "OFF_PAD_HIGH") : "UNKNOWN";
        result.success = slot->admitted && slot->outputConfirmed && !slot->controller.faulted() && !slot->retiring;
        return result.success;
    }
    result.remark = "THERMOSTAT_NOT_ADMITTED";
    return false;
}

String PeriphExecManager::getThermostatDiagnosticsJson() const {
    MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(20));
    if (!lock.isLocked()) return "null";
    const uint64_t now = thermostatNow();
    JsonDocument doc;
    JsonArray list = doc.to<JsonArray>();
    for (const auto& slot : _thermostats) {
        if (!slot) continue;
        const auto current = rules.find(slot->ruleId);
        JsonObject item = list.add<JsonObject>();
        if (item.isNull() || !item["ruleId"].set(slot->ruleId) || !item["targetId"].set(slot->targetId) ||
            !item["phase"].set(slot->retiring ? "RETIRING_OUTPUT_UNCONFIRMED" :
                FastBee::thermostatPhaseName(slot->controller.phase())) ||
            !item["enabled"].set(current != rules.end() && current->second.enabled) ||
            !item["admitted"].set(slot->admitted) || !item["padConfirmed"].set(slot->outputConfirmed) ||
            !item["outputOn"].set(slot->controller.on()) || !item["retiring"].set(slot->retiring) ||
            !item["checkAgeMs"].set(now >= slot->checkedAtMs ? now - slot->checkedAtMs : UINT64_MAX) ||
            !item["physicalFeedback"].set("NOT_OBSERVED")) return "null";
    }
    if (doc.overflowed()) return "null";
    String json;
    const size_t expected = measureJson(doc);
    if (!json.reserve(expected + 1) || serializeJson(doc, json) != expected || json.length() != expected) return "null";
    return json;
}
#endif
