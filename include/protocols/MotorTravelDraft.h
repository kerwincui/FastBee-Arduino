#pragma once
#include <ArduinoJson.h>
#include <cmath>

// Per-axis setup notes, never a motion reference or an admission override.
struct MotorTravelDraft {
    bool configured = false, valid = true;
    double forwardClearanceMm = 0, reverseClearanceMm = 0;
    double forwardMarginMm = 0, reverseMarginMm = 0, countsPerMm = 0;
};

inline const char* loadMotorTravelDraft(JsonObjectConst device, MotorTravelDraft& out) {
    out = MotorTravelDraft{};
    if (device["motorTravelDraft"].isUnbound()) return nullptr;
    out.configured = true;
    out.valid = false;
    JsonObjectConst draft = device["motorTravelDraft"].as<JsonObjectConst>();
    if (draft.isNull() || !draft["version"].is<unsigned>() || draft["version"].as<unsigned>() != 1)
        return "motorTravelDraft.version must be 1";
    const char* keys[] = {"forwardClearanceMm", "reverseClearanceMm", "forwardMarginMm", "reverseMarginMm", "countsPerMm"};
    double values[5];
    for (unsigned i = 0; i < 5; ++i) {
        JsonVariantConst value = draft[keys[i]];
        if (value.is<bool>() || value.is<const char*>() || !value.is<double>())
            return "motorTravelDraft values must be finite nonnegative numbers";
        values[i] = value.as<double>();
        if (!std::isfinite(values[i]) || values[i] < 0)
            return "motorTravelDraft values must be finite nonnegative numbers";
    }
    if (values[2] > values[0] || values[3] > values[1] ||
        (values[0] - values[2] <= 0 && values[1] - values[3] <= 0))
        return "motorTravelDraft margins leave no usable travel";
    out.forwardClearanceMm = values[0]; out.reverseClearanceMm = values[1];
    out.forwardMarginMm = values[2]; out.reverseMarginMm = values[3];
    out.countsPerMm = values[4];
    out.valid = true;
    return nullptr;
}

inline void saveMotorTravelDraft(JsonObject device, const MotorTravelDraft& draft) {
    if (!draft.configured) return;
    JsonObject out = device["motorTravelDraft"].to<JsonObject>();
    out["version"] = draft.valid ? 1 : 0;
    out["forwardClearanceMm"] = draft.forwardClearanceMm;
    out["reverseClearanceMm"] = draft.reverseClearanceMm;
    out["forwardMarginMm"] = draft.forwardMarginMm;
    out["reverseMarginMm"] = draft.reverseMarginMm;
    out["countsPerMm"] = draft.countsPerMm;
}
