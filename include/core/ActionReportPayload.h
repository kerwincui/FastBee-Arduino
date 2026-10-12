#pragma once
#include <ArduinoJson.h>
#include "core/ActionReportIdentity.h"
#include "core/SensorSampleMetadata.h"

namespace FastBee {
constexpr size_t kActionReportPayloadBytes = 512;

enum class CapturedReportRow : uint8_t { Legacy, Added, Invalid };
template<class Result>
CapturedReportRow appendCapturedReportRow(JsonArray rows, const Result& result) {
    if (result.reportIdentity == ActionReportIdentity::Legacy) return CapturedReportRow::Invalid;
    const String* id = capturedReportId(result);
    if (!id) return CapturedReportRow::Invalid;
    JsonObject row = rows.add<JsonObject>();
    const char* remark = result.remark.isEmpty() ? (result.success ? "success" : "failed") : result.remark.c_str();
    // Failed/unpublished observations must not expose a prior or partial value
    // as current data. Failure reason remains available on the original ID.
    if (row.isNull() || !row["id"].set(*id) ||
        !row["value"].set(result.success ? result.actualValue.c_str() : "") ||
        !row["remark"].set(remark)) return CapturedReportRow::Invalid;
    if (result.hasSensorSample) {
        if (result.sampleUnit.length() > 32 || result.sampleUnit.length() != result.sampleUnitBytes ||
            strlen(result.sampleUnit.c_str()) != result.sampleUnit.length())
            return CapturedReportRow::Invalid;
        const auto& sample = result.sensorSample;
        const bool known = sample.quality != SensorSampleQuality::Unknown;
        if (known && (!sample.maxAgeMs || sample.capturedAtMs < sample.sampledAtMs))
            return CapturedReportRow::Invalid;
        JsonObject observation = row["sample"].to<JsonObject>();
        const char* quality = sample.quality == SensorSampleQuality::Unknown ? "unknown" :
            sample.quality == SensorSampleQuality::Stale ? "stale" : result.success ? "good" : "error";
        // These fields are immutable when queued. A receiver must never treat
        // the broker receipt timestamp as a fresh physical acquisition.
        if (observation.isNull() || !observation["unit"].set(result.sampleUnit) ||
            !observation["qualityAtCapture"].set(quality) ||
            !observation["clock"].set("boot_monotonic_ms") ||
            !observation["freshnessAtReceipt"].set("unverified")) return CapturedReportRow::Invalid;
        if (known && (!observation["sampledAtMs"].set(sample.sampledAtMs) ||
            !observation["capturedAtMs"].set(sample.capturedAtMs) ||
            !observation["maxAgeMs"].set(sample.maxAgeMs))) return CapturedReportRow::Invalid;
    }
    return CapturedReportRow::Added;
}

// Keep the existing array / {part,total,data} wire formats. Count exact escaped
// UTF-8 JSON bytes, including the envelope; never split an individual value.
// emitted counts callback invocations, NOT MQTT delivery/acceptance. A later
// serialization failure can follow earlier callbacks: callers must not replay
// the whole batch automatically.
template<class Emit>
const char* emitActionReportPayloads(JsonArrayConst rows, Emit emit, size_t& emitted) {
    emitted = 0;
    if (rows.isNull() || rows.size() == 0) return "REPORT_EMPTY";
    for (JsonVariantConst row : rows)
        if (!row.is<JsonObjectConst>()) return "REPORT_ROW_INVALID";
    const auto serialize = [&](JsonVariantConst value) {
        const size_t bytes = measureJson(value);
        if (bytes > kActionReportPayloadBytes) return false;
        String payload;
        if (!payload.reserve(bytes) || serializeJson(value, payload) != bytes || payload.length() != bytes)
            return false;
        emit(payload);
        ++emitted;
        return true;
    };
    if (measureJson(rows) <= kActionReportPayloadBytes)
        return serialize(rows) ? nullptr : "REPORT_SERIALIZATION_FAILED";

    JsonDocument chunk;
    // At most one part per row. Largest possible part/total digits form a
    // conservative bound for every actual envelope without a magic allowance.
    chunk["part"] = rows.size();
    chunk["total"] = rows.size();
    chunk["data"].to<JsonArray>();
    if (chunk.overflowed()) return "REPORT_SERIALIZATION_FAILED";
    const size_t overhead = measureJson(chunk);
    if (overhead >= kActionReportPayloadBytes) return "REPORT_ENVELOPE_TOO_LARGE";
    const size_t capacity = kActionReportPayloadBytes - overhead;
    size_t parts = 1, used = 0;
    // Complete size validation BEFORE the first callback. An oversized row
    // rejects this batch with an explicit error, not a truncated success value.
    for (JsonVariantConst row : rows) {
        const size_t bytes = measureJson(row);
        if (bytes > capacity) return "REPORT_VALUE_TOO_LARGE";
        if (used && bytes + 1 > capacity - used) { ++parts; used = 0; }
        used += bytes + (used ? 1 : 0);
    }
    size_t next = 0;
    for (size_t part = 1; part <= parts; ++part) {
        chunk.clear();
        chunk["part"] = part;
        chunk["total"] = parts;
        JsonArray data = chunk["data"].to<JsonArray>();
        used = 0;
        while (next < rows.size()) {
            const size_t bytes = measureJson(rows[next]);
            if (used && bytes + 1 > capacity - used) break;
            if (!data.add(rows[next]) || chunk.overflowed()) return "REPORT_SERIALIZATION_FAILED";
            used += bytes + (used ? 1 : 0);
            ++next;
        }
        if (chunk.overflowed() || !serialize(chunk.as<JsonVariantConst>())) return "REPORT_SERIALIZATION_FAILED";
    }
    return nullptr;
}
} // namespace FastBee
