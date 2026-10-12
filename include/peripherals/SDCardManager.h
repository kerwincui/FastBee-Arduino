#ifndef SD_CARD_MANAGER_H
#define SD_CARD_MANAGER_H

#include <Arduino.h>
#include <atomic>
#include <vector>

struct SDCardStatus {
    // Passive diagnostic snapshot, not a fresh card-presence/capacity probe.
    bool statusAvailable = false;
    // Configuration/diagnostics can remain known when a lost SPI lease makes
    // mount state unavailable. Never inferred from an error string.
    bool configurationAvailable = false;
    bool capacityValid = false;
    // Boot-local 64-bit monotonic milliseconds, never wall time or millis32.
    // capacityValid, not a zero timestamp sentinel, determines availability.
    uint64_t capacitySampledAtMs = 0;
    uint64_t capacityAgeMs = 0;
    uint64_t recordingGeneration = 0, recordingRecovery = 0;
    bool configured = false;
    bool mounted = false;
    String ownerId;
    uint8_t sckPin = 255;
    uint8_t mosiPin = 255;
    uint8_t misoPin = 255;
    uint8_t csPin = 255;
    uint32_t frequency = 0;
    uint64_t cardBytes = 0;
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    String cardType;
    String lastError;
    // Diagnostics for this configuration session, retained across manual
    // unmount/remount and successful later I/O; not evidence of media health.
    bool recoverySuggested = false;
    uint32_t ioFailures = 0, recoveryAttempts = 0, recoveries = 0;
    String lastFailureCode;
};

struct SDCardEntry {
    String path;
    bool directory = false;
    size_t size = 0;
};

struct SDRecordingSession {
    uint64_t token = 0;
    uint16_t sizes[8] = {};
    uint8_t slot = 0, createdMask = 0;
};

class SDCardManager {
public:
    static constexpr size_t MAX_FILE_BYTES = 16384;
    static constexpr size_t MAX_LIST_ENTRIES = 32;

    static SDCardManager& getInstance();

    bool configureAndMount(const String& ownerId, uint8_t sckPin,
                           uint8_t mosiPin, uint8_t misoPin, uint8_t csPin,
                           uint32_t frequency, String& errorMsg);
    bool mount(String& errorMsg, const String& ownerId = "");
    // Explicit local teardown/remount only; never replays a failed file command.
    bool recover(const String& ownerId, String& errorMsg);
    bool unmount(const String& ownerId = "", String* errorMsg = nullptr,
                 bool releaseConfiguration = true);

    SDCardStatus getStatus() const;
    // Exact truncation comes from observing one additional entry, not from
    // equating a full-sized page with an incomplete directory. On failure the
    // caller's entries and truncation flag remain unchanged.
    bool list(const String& path, std::vector<SDCardEntry>& entries,
              String& errorMsg, const String& ownerId = "", bool* truncated = nullptr);
    bool readFile(const String& path, String& content, String& errorMsg,
                  size_t maxBytes = MAX_FILE_BYTES, const String& ownerId = "");
    bool writeFile(const String& path, const uint8_t* data, size_t len,
                   bool append, String& errorMsg, const String& ownerId = "");
    bool removeFile(const String& path, String& errorMsg, const String& ownerId = "");
    bool appendRecording(const String& ownerId, uint64_t generation,
        SDRecordingSession& session, const uint8_t* data, size_t length,
        uint16_t fileBytes, uint8_t fileCount, String& errorMsg, bool* ioAttempted = nullptr);
    // FAT names are case-insensitive; /./ and 8.3 REC-* aliases must not
    // bypass the managed leaf-name reservation. Input is already normalized.
    static bool isRecordingPath(const String& path) {
        size_t leaf = 0;
        for (size_t i = 0; i < path.length(); ++i) if (path[i] == '/') leaf = i + 1;
        if (path.length() - leaf < 4) return false;
        const char* prefix = "rec-";
        for (size_t i = 0; i < 4; ++i) {
            char c = path[leaf + i];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != prefix[i]) return false;
        }
        return true;
    }

    static bool normalizeSandboxPath(const String& input, String& normalized,
                                     String& errorMsg, bool allowRoot = true);

private:
    SDCardManager() = default;
    SDCardManager(const SDCardManager&) = delete;
    SDCardManager& operator=(const SDCardManager&) = delete;

    bool ensureMutex() const;
    void* mutexHandle() const { return _mutex.load(std::memory_order_acquire); }
    bool ensureMountedLocked(String& errorMsg);
    bool requireMountedLocked(String& errorMsg);
    bool checkOwnerLocked(const String& ownerId, String& errorMsg) const;
    void updateCapacityLocked();
    void setErrorLocked(const String& errorMsg);
    void recordIoFailureLocked(const String& errorMsg);

    mutable std::atomic<void*> _mutex{nullptr};
    SDCardStatus _status;
    uint64_t _recordingGeneration = 1, _recordingRecovery = 0;
};

#endif // SD_CARD_MANAGER_H
