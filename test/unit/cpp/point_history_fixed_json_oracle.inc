bool PointHistory::configure(bool enabled, const std::vector<String>& sourceAllowlist,
                             String& error, bool persist,
                             const uint32_t* expectedRevision) {
    error = "";
    if (!_initialized && !initialize()) {
        error = "Point history is unavailable";
        return false;
    }
    {
        HistoryLock lock(_mutex);
        if (!lock.locked() || _writesPaused) {
            error = "Point history writes are paused";
            return false;
        }
    }
    if (sourceAllowlist.size() > MAX_SOURCES) {
        error = String("History source limit exceeded (max ") + MAX_SOURCES + ")";
        return false;
    }
    for (size_t i = 0; i < sourceAllowlist.size(); ++i) {
        if (!validSourceRef(sourceAllowlist[i])) {
            error = "Invalid history sourceRef";
            return false;
        }
        for (size_t j = i + 1; j < sourceAllowlist.size(); ++j) {
            if (sourceAllowlist[i] == sourceAllowlist[j]) {
                error = "Duplicate history sourceRef";
                return false;
            }
        }
    }

    // Prepare allocations before committing bytes. Publishing the persisted
    // allowlist must not allocate and fail halfway through a revision change.
    std::vector<String> preparedAllowlist;
    preparedAllowlist.reserve(sourceAllowlist.size());
    for (const auto& source : sourceAllowlist) {
        preparedAllowlist.emplace_back();
        if (!copyCheckedText(preparedAllowlist.back(), source.c_str(), source.length(), MAX_SOURCE_REF_LENGTH)) {
            error = "Unable to prepare history sourceRef";
            return false;
        }
    }
    uint32_t nextRevision;
    {
        HistoryLock lock(_mutex);
        if (!lock.locked() || _writesPaused || _configWriteInProgress) {
            error = "Point history is busy";
            return false;
        }
        if (expectedRevision && *expectedRevision != _configRevision) {
            error = "History configuration revision conflict";
            return false;
        }
        if (_configRevision == UINT32_MAX) {
            error = "History configuration revision exhausted";
            return false;
        }
        nextRevision = _configRevision + 1;
        if (enabled && _series.capacity() < sourceAllowlist.size()) {
            _series.reserve(sourceAllowlist.size());
        }
        _configWriteInProgress = true;
    }

    // Reserve the configuration writer before flushing/staging. Samples and
    // other writers cannot change the old state until this commit completes.
    bool success = enabled || flushNow();
    if (!success) error = "Unable to flush point history before disabling";
    if (success && persist) {
        JsonDocument doc;
        doc["schemaVersion"] = 1;
        doc["revision"] = nextRevision;
        doc["enabled"] = enabled;
        JsonArray sources = doc["sourceAllowlist"].to<JsonArray>();
        for (const auto& source : sourceAllowlist) sources.add(source);
        success = atomicWriteJsonWithBackup(CONFIG_FILE, doc);
        if (!success) error = "Failed to persist history configuration";
    }
    {
        // A persisted commit must publish its matching runtime revision before
        // releasing the writer reservation, even under brief reader contention.
        HistoryLock lock(_mutex, portMAX_DELAY);
        if (!lock.locked()) { error = "Point history is busy"; return false; }
        _configWriteInProgress = false;
        if (!success) return false;
        _configRevision = nextRevision;
        _enabled = enabled;
        _sourceAllowlist.swap(preparedAllowlist);
        if (enabled) {
            // An empty allowlist means accept any source, not preallocate all
            // 24 series (each embeds ten samples). Keep existing history when
            // changing the allowlist, and reserve only the configured demand.
            // Capacity was prepared before the disk commit. Retaining bounded
            // spare capacity avoids a second allocation on the commit path.
        } else {
            std::vector<Series>().swap(_series);
            _restored = false;
            _dirty = false;
            _firstDirtyAt = 0;
            _postSnapshotDirtyAt = 0;
        }
    }

    return true;
}

void PointHistory::appendSeriesJson(const String& sourceRef, JsonObject out) const {
    PointHistorySeriesSnapshot snapshot;
    out["sourceRef"] = sourceRef;
    JsonArray points = out["points"].to<JsonArray>();
    if (!getSeries(sourceRef, snapshot)) {
        out["found"] = false;
        out["count"] = 0;
        return;
    }
    out["found"] = true;
    out["sourceRevision"] = snapshot.sourceRevision;
    out["unit"] = snapshot.unit;
    out["count"] = snapshot.count;
    out["containsRestoredSamples"] = snapshot.containsRestoredSamples;
    out["stale"] = snapshot.stale;
    if (!snapshot.staleReason.isEmpty()) out["staleReason"] = snapshot.staleReason;
    for (uint8_t i = 0; i < snapshot.count; ++i) {
        const auto& sample = snapshot.samples[i];
        JsonObject item = points.add<JsonObject>();
        item["value"] = sample.value;
        item["seq"] = sample.sequence;
        item["sampleIdentity"] = sample.sampleIdentity;
        item["bootId"] = sample.bootId;
        item["uptimeMs"] = sample.uptimeMs;
        if (sample.epochMs) item["epochMs"] = sample.epochMs;
        else item["epochMs"] = nullptr;
        item["restored"] = sample.restored;
    }
}

void PointHistory::appendConfigJson(JsonObject out) const {
    HistoryLock lock(_mutex);
    if (!lock.locked()) return;
    out["enabled"] = _enabled;
    out["revision"] = _configRevision;
    out["writesPaused"] = _writesPaused;
    out["maxSources"] = MAX_SOURCES;
    out["maxSamplesPerSource"] = MAX_SAMPLES_PER_SOURCE;
    JsonArray allowlist = out["sourceAllowlist"].to<JsonArray>();
    for (const auto& source : _sourceAllowlist) allowlist.add(source);
}

void PointHistory::appendStatusJson(JsonObject out) const {
    HistoryLock lock(_mutex);
    if (!lock.locked()) return;
    out["enabled"] = _enabled;
    out["dirty"] = _dirty;
    out["restored"] = _restored;
    out["persisted"] = !_dirty && _lastFlushSucceeded;
    out["lastFlushSucceeded"] = _lastFlushSucceeded;
    out["sourceCount"] = _series.size();
    out["firstDirtyAt"] = _firstDirtyAt;
    out["flushDeadlineMs"] = FLUSH_DEADLINE_MS;
}

bool PointHistory::writeHistorySnapshot(const std::vector<Series>& snapshot,
                                        uint32_t sequence) const {
    JsonDocument doc;
    doc["schemaVersion"] = 1;
    doc["sequence"] = sequence;
    JsonArray allSeries = doc["series"].to<JsonArray>();
    for (const auto& series : snapshot) {
        JsonObject source = allSeries.add<JsonObject>();
        source["sourceRef"] = series.sourceRef;
        source["sourceRevision"] = series.sourceRevision;
        source["unit"] = series.unit;
        source["stale"] = series.stale;
        if (!series.staleReason.isEmpty()) {
            source["staleReason"] = series.staleReason;
        }
        JsonArray samples = source["samples"].to<JsonArray>();
        for (uint8_t i = 0; i < series.count; ++i) {
            const auto& sample = series.samples[(series.head + i) % MAX_SAMPLES_PER_SOURCE];
            JsonObject item = samples.add<JsonObject>();
            item["value"] = sample.value;
            item["seq"] = sample.sequence;
            item["sampleIdentity"] = sample.sampleIdentity;
            item["bootId"] = sample.bootId;
            item["uptimeMs"] = sample.uptimeMs;
            if (sample.epochMs) item["epochMs"] = sample.epochMs;
            else item["epochMs"] = nullptr;
        }
    }
    if (doc.overflowed() || measureJson(doc) > MAX_FILE_BYTES) return false;
    return atomicWriteJsonWithBackup(DATA_FILE, doc);
}
