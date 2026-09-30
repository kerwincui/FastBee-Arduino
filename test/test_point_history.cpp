#include <unity.h>

#ifdef UNIT_TEST

#define FASTBEE_ENABLE_POINT_HISTORY 1

#include <Arduino.h>
#include <ArduinoJson.h>
#include <array>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define private public
#include "core/PointHistory.h"
#undef private

// PointHistory owns a process-lifetime singleton. Native tests reset its
// bounded state directly between cases and exercise the production methods.
#include "../src/core/PointHistory.cpp"

namespace {

FastBee::PointHistory& resetHistory(bool enabled = true) {
    auto& history = FastBee::PointHistory::instance();
    history._mutex = reinterpret_cast<SemaphoreHandle_t>(1);
    history._initialized = true;
    history._enabled = enabled;
    history._dirty = false;
    history._restored = false;
    history._writesPaused = false;
    history._flushInProgress = false;
    history._configWriteInProgress = false;
    history._lastFlushSucceeded = true;
    history._firstDirtyAt = 0;
    history._postSnapshotDirtyAt = 0;
    history._generation = 0;
    history._configRevision = 0;
    history._sequence = 0;
    history._bootId = "native-boot";
    history._sourceAllowlist.clear();
    history._series.clear();
    return history;
}

void test_point_history_keeps_last_ten_distinct_samples() {
    auto& history = resetHistory();
    for (uint32_t identity = 1; identity <= 12; ++identity) {
        TEST_ASSERT_TRUE(history.recordSample(
            "sensor.temp", 20.0, "C", "rev-1", identity, identity * 10));
    }
    FastBee::PointHistorySeriesSnapshot snapshot;
    TEST_ASSERT_TRUE(history.getSeries("sensor.temp", snapshot));
    TEST_ASSERT_EQUAL_UINT8(10, snapshot.count);
    TEST_ASSERT_EQUAL_UINT32(3, snapshot.samples[0].sampleIdentity);
    TEST_ASSERT_EQUAL_UINT32(12, snapshot.samples[9].sampleIdentity);
    // 20.0 is exactly representable; keep the double-value assertions without
    // requiring Unity's optional double-formatting support in native builds.
    TEST_ASSERT_TRUE_MESSAGE(snapshot.samples[0].value == 20.0,
                             "Oldest retained sample must keep its double value");
    TEST_ASSERT_TRUE_MESSAGE(snapshot.samples[9].value == 20.0,
                             "Newest retained sample must keep its double value");
}

void test_point_history_rejects_duplicate_identity_and_non_finite_values() {
    auto& history = resetHistory();
    TEST_ASSERT_TRUE(history.recordSample(
        "sensor.temp", 20.0, "C", "rev-1", 77, 100));
    TEST_ASSERT_FALSE(history.recordSample(
        "sensor.temp", 21.0, "C", "rev-1", 77, 200));
    TEST_ASSERT_FALSE(history.recordSample(
        "sensor.temp", NAN, "C", "rev-1", 78, 300));
    FastBee::PointHistorySeriesSnapshot snapshot;
    TEST_ASSERT_TRUE(history.getSeries("sensor.temp", snapshot));
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.count);
}

void test_point_history_marks_removed_source_stale_and_clears_on_revision_change() {
    auto& history = resetHistory();
    TEST_ASSERT_TRUE(history.recordSample(
        "remote.power", 42.0, "W", "mapping-a", 1, 100));
    TEST_ASSERT_TRUE(history.invalidateSource(
        "remote.power", "source_disabled"));
    FastBee::PointHistorySeriesSnapshot snapshot;
    TEST_ASSERT_TRUE(history.getSeries("remote.power", snapshot));
    TEST_ASSERT_TRUE(snapshot.stale);
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.count);

    TEST_ASSERT_TRUE(history.invalidateSource(
        "remote.power", "source_remapped", "mapping-b"));
    TEST_ASSERT_TRUE(history.getSeries("remote.power", snapshot));
    TEST_ASSERT_TRUE(snapshot.stale);
    TEST_ASSERT_EQUAL_UINT8(0, snapshot.count);
    TEST_ASSERT_EQUAL_STRING("mapping-b", snapshot.sourceRevision.c_str());

    TEST_ASSERT_TRUE(history.recordSample(
        "remote.power", 43.0, "W", "mapping-b", 2, 200));
    TEST_ASSERT_TRUE(history.getSeries("remote.power", snapshot));
    TEST_ASSERT_FALSE(snapshot.stale);
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.count);
}

void test_point_history_pause_blocks_samples_and_reset_resume_clears_state() {
    auto& history = resetHistory();
    TEST_ASSERT_TRUE(history.recordSample(
        "sensor.temp", 20.0, "C", "rev-1", 1, 100));
    TEST_ASSERT_TRUE(history.pauseWrites(20));
    TEST_ASSERT_FALSE(history.recordSample(
        "sensor.temp", 21.0, "C", "rev-1", 2, 200));
    TEST_ASSERT_TRUE(history.resumeWrites(false));
    TEST_ASSERT_TRUE(history.recordSample(
        "sensor.temp", 21.0, "C", "rev-1", 2, 200));
    TEST_ASSERT_TRUE(history.pauseWrites(20));
    TEST_ASSERT_TRUE(history.resumeWrites(true));
    TEST_ASSERT_FALSE(history.isEnabled());
    TEST_ASSERT_EQUAL_UINT32(0, history.sourceCount());
}

void test_point_history_uninitialized_flush_is_a_noop_success() {
    auto& history = resetHistory();
    history._initialized = false;
    TEST_ASSERT_TRUE(history.flushNow());
}

void test_point_history_config_revision_rejects_stale_writer() {
    auto& history = resetHistory();
    String error;
    const uint32_t revision = 0;
    TEST_ASSERT_TRUE(history.configure(true, {"sensor.temp"}, error, true, &revision));
    TEST_ASSERT_EQUAL_UINT32(1, history._configRevision);
    TEST_ASSERT_TRUE(history.recordSample("sensor.temp", 21.0, "C", "v1", 1, 100));
    TEST_ASSERT_FALSE(history.configure(false, {}, error, true, &revision));
    TEST_ASSERT_EQUAL_STRING("History configuration revision conflict", error.c_str());
    TEST_ASSERT_TRUE(history.isEnabled());
    TEST_ASSERT_EQUAL_UINT32(1, history.sourceCount());
    JsonDocument saved;
    TEST_ASSERT_FALSE(deserializeJson(saved, g_mockFiles[FastBee::PointHistory::CONFIG_FILE]));
    TEST_ASSERT_EQUAL_UINT32(1, saved["revision"].as<uint32_t>());
    TEST_ASSERT_TRUE(saved["enabled"].as<bool>());
    history._configRevision = 0;
    TEST_ASSERT_TRUE(history.loadConfig());
    TEST_ASSERT_EQUAL_UINT32(1, history._configRevision);
}

void test_point_history_reserved_writer_blocks_competing_changes() {
    auto& history = resetHistory();
    history._configWriteInProgress = true;
    String error;
    TEST_ASSERT_FALSE(history.configure(false, {}, error, false));
    TEST_ASSERT_FALSE(history.recordSample("sensor.temp", 21.0, "C", "v1", 1, 100));
    TEST_ASSERT_TRUE(history.isEnabled());
    TEST_ASSERT_EQUAL_UINT32(0, history._configRevision);
    history._configWriteInProgress = false;
}

void test_point_history_legacy_config_defaults_revision() {
    auto& history = resetHistory();
    g_mockFiles[FastBee::PointHistory::CONFIG_FILE] =
        "{\"schemaVersion\":1,\"enabled\":true,\"sourceAllowlist\":[\"sensor.temp\"]}";
    TEST_ASSERT_TRUE(history.loadConfig());
    TEST_ASSERT_EQUAL_UINT32(0, history._configRevision);
    String error;
    const uint32_t revision = 0;
    TEST_ASSERT_TRUE(history.configure(true, {"sensor.temp", "sensor.humidity"}, error, false, &revision));
    TEST_ASSERT_EQUAL_UINT32(1, history._configRevision);
}

}  // namespace

#endif  // UNIT_TEST

void test_point_history_group() {
#ifdef UNIT_TEST
    RUN_TEST(test_point_history_keeps_last_ten_distinct_samples);
    RUN_TEST(test_point_history_rejects_duplicate_identity_and_non_finite_values);
    RUN_TEST(test_point_history_marks_removed_source_stale_and_clears_on_revision_change);
    RUN_TEST(test_point_history_pause_blocks_samples_and_reset_resume_clears_state);
    RUN_TEST(test_point_history_uninitialized_flush_is_a_noop_success);
    RUN_TEST(test_point_history_config_revision_rejects_stale_writer);
    RUN_TEST(test_point_history_reserved_writer_blocks_competing_changes);
    RUN_TEST(test_point_history_legacy_config_defaults_revision);
#endif
}
