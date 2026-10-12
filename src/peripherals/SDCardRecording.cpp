#include "peripherals/SDCardManager.h"
#include "core/SpiBusInitializer.h"
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
class RecordingLock {
public:
    explicit RecordingLock(void* mutex) : mutex_(static_cast<SemaphoreHandle_t>(mutex)),
        held_(mutex_ && xSemaphoreTake(mutex_, 0) == pdTRUE) {}
    ~RecordingLock() { if (held_) xSemaphoreGive(mutex_); }
    bool held() const { return held_; }
private:
    SemaphoreHandle_t mutex_;
    bool held_;
};
uint32_t recordingOwnerHash(const String& owner) {
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < owner.length(); ++i) { hash ^= static_cast<uint8_t>(owner[i]); hash *= 16777619UL; }
    return hash;
}
}

bool SDCardManager::appendRecording(const String& ownerId, uint64_t generation,
        SDRecordingSession& session, const uint8_t* data, size_t length,
        uint16_t fileBytes, uint8_t fileCount, String& errorMsg, bool* ioAttempted) {
    if (ioAttempted) *ioAttempted = false;
    if (ownerId.isEmpty() || !generation || !session.token || !data || !length || length > 2048 ||
        fileBytes < 4096 || fileBytes > MAX_FILE_BYTES || fileCount < 2 || fileCount > 8 ||
        session.slot >= fileCount) {
        errorMsg = "SD_RECORD_ARGUMENT_INVALID"; return false;
    }
    if (!ensureMutex()) { errorMsg = "SD_MUTEX_UNAVAILABLE"; return false; }
    RecordingLock lock(mutexHandle());
    if (!lock.held()) { errorMsg = "SD_BUSY"; return false; }
    if (!checkOwnerLocked(ownerId, errorMsg)) return false;
    if (generation != _recordingGeneration || _recordingGeneration == UINT64_MAX) {
        errorMsg = "SD_RECORD_STORAGE_CHANGED"; return false;
    }
    FastBee::SpiBusOperation bus(0);
    if (!bus.locked()) { errorMsg = "SPI_BUSY"; return false; }
    if (!requireMountedLocked(errorMsg)) return false;

    uint8_t slot = session.slot;
    const size_t limit = fileBytes;
    const bool rotate = session.sizes[slot] > limit || length > limit - session.sizes[slot];
    if (rotate) slot = static_cast<uint8_t>((slot + 1) % fileCount);
    char pathBuffer[80];
    const int pathLength = std::snprintf(pathBuffer, sizeof(pathBuffer),
        "/fastbee/rec-%08lx-%08lx%08lx-%u.jsonl",
        static_cast<unsigned long>(recordingOwnerHash(ownerId)),
        static_cast<unsigned long>(session.token >> 32),
        static_cast<unsigned long>(session.token & 0xffffffffULL), static_cast<unsigned>(slot));
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(pathBuffer)) {
        errorMsg = "SD_RECORD_PATH_INVALID"; return false;
    }
    const String path = pathBuffer;
    if (path.length() != static_cast<size_t>(pathLength)) { errorMsg = "SD_RECORD_NO_MEMORY"; return false; }
    const bool created = (session.createdMask & (1U << slot)) != 0;
    const bool exists = SD.exists(path);
    if (created != exists) {
        errorMsg = created ? "SD_RECORD_FILE_DISAPPEARED" : "SD_RECORD_HISTORICAL_COLLISION";
        recordIoFailureLocked(errorMsg); return false;
    }
    if (exists) {
        File previous = SD.open(path, FILE_READ);
        const bool verified = previous && !previous.isDirectory() && previous.size() == session.sizes[slot];
        previous.close();
        if (!verified) { errorMsg = "SD_RECORD_FILE_CHANGED"; recordIoFailureLocked(errorMsg); return false; }
    }
    // The only deletion is a file created and size-tracked by this still-live
    // recorder session. Never glob, scan/delete old sessions or format media.
    if (rotate && created) {
        if (ioAttempted) *ioAttempted = true;
        _status.capacityValid = false;
        if (!SD.remove(path)) { errorMsg = "SD_RECORD_ROTATE_FAILED"; recordIoFailureLocked(errorMsg); return false; }
    }
    const size_t previousSize = rotate ? 0 : session.sizes[slot];
    if (ioAttempted) *ioAttempted = true;
    _status.capacityValid = false;
    // exists() can be false after a read/stat/allocation failure. New slots
    // must atomically reject any existing path, including a recreated slot
    // after rotation; "w" would truncate historical evidence in that window.
    File file = SD.open(path, (!rotate && created) ? FILE_APPEND : "wx", true);
    if (!file || file.isDirectory()) {
        file.close(); errorMsg = "SD_RECORD_OPEN_FAILED"; recordIoFailureLocked(errorMsg); return false;
    }
    const size_t written = file.write(data, length);
    file.flush();
    const bool accepted = written == length && !file.getWriteError() && file.size() == previousSize + length;
    file.close();
    if (!accepted) {
        // A short/failed write can already have altered media. Session state
        // is deliberately not advanced and callers must latch, never retry.
        errorMsg = "SD_RECORD_WRITE_UNCONFIRMED"; recordIoFailureLocked(errorMsg); return false;
    }
    session.sizes[slot] = static_cast<uint16_t>(previousSize + length);
    session.createdMask |= static_cast<uint8_t>(1U << slot);
    session.slot = slot;
    // FAT totals are not probed for every batch. A successful SDK write/flush
    // is not power-loss durability; capacity remains explicitly unknown.
    _status.lastError = "";
    errorMsg = "";
    return true;
}
