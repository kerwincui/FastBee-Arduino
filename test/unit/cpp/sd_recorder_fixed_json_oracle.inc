// Frozen original serializers before the 28-assignment FixedJsonField rewrite.
// Preserve these bodies as the independent behavior oracle.
size_t sdRecordBatch(FastBee::SdRecorderRuntime& runtime, size_t& count) {
    size_t length = 0;
    count = 0;
    for (size_t i = 0; i < runtime.queue.size(); ++i) {
        const auto& sample = runtime.queue.at(i);
        JsonDocument doc;
        doc["schema"] = 1;
        doc["sourceRef"] = runtime.config.sourceRef;
        doc["sourceRevision"] = runtime.sourceRevision;
        doc["unit"] = runtime.unit;
        doc["value"] = sample.value;
        doc["quality"] = "good";
        doc["sampleId"] = sample.identity;
        doc["sampledAtMs"] = sample.sampledAtMs;
        doc["recordedAtMs"] = sample.recordedAtMs;
        doc["timeSource"] = sample.timeSource;
        doc["timeQuality"] = sample.timeQuality;
        doc["timeAuthenticated"] = sample.timeAuthenticated;
        doc["timeSequence"] = sample.timeSequence;
        if (sample.wallKnown) doc["wallEpochMs"] = sample.wallEpochMs;
        else doc["wallEpochMs"] = nullptr;
        if (doc.overflowed()) return 0;
        const size_t bytes = measureJson(doc);
        if (bytes + 1 > runtime.batch.size() - length) break;
        if (serializeJson(doc, runtime.batch.data() + length, runtime.batch.size() - length) != bytes) return 0;
        length += bytes;
        runtime.batch[length++] = '\n';
        ++count;
    }
    return length;
}
String sdRecordJson(const FastBee::SdRecorderRuntime& runtime) {
    const uint64_t now = sdRecordNow();
    JsonDocument doc;
    doc["ruleId"] = runtime.ruleId;
    doc["targetId"] = runtime.targetId;
    doc["sourceRef"] = runtime.config.sourceRef;
    doc["phase"] = runtime.queue.fault() ? runtime.queue.fault() :
        runtime.waitingFresh ? "WAITING_FRESH_SAMPLE" : "RECORDING";
    doc["pending"] = runtime.queue.size();
    doc["accepted"] = runtime.queue.accepted;
    doc["written"] = runtime.queue.written;
    doc["dropped"] = runtime.queue.dropped;
    doc["lastError"] = runtime.lastError;
    doc["checkAgeMs"] = now >= runtime.checkedAt ? now - runtime.checkedAt : UINT64_MAX;
    doc["writeQuality"] = "sdk_write_flush_only";
    doc["fileSlot"] = runtime.storage.slot;
    doc["fileCount"] = runtime.config.fileCount;
    if (doc.overflowed()) return "null";
    String json;
    const size_t expected = measureJson(doc);
    if (!json.reserve(expected) || serializeJson(doc, json) != expected || json.length() != expected) return "null";
    return json;
}
