#include "peripherals/SDCardManager.h"
#include "core/SpiBusInitializer.h"
#include "peripherals/BoundedFileRead.h"

#include "systems/LoggerSystem.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
constexpr const char* SD_SANDBOX_ROOT = "/fastbee";
constexpr uint32_t SD_DEFAULT_FREQUENCY = 20000000UL;
constexpr uint32_t SD_MIN_FREQUENCY = 100000UL;
constexpr uint32_t SD_MAX_FREQUENCY = 40000000UL;

class SDMutexGuard {
public:
    explicit SDMutexGuard(void* mutex, uint32_t timeoutMs = 2000)
        : _mutex(static_cast<SemaphoreHandle_t>(mutex)),
          _locked(_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {}
    ~SDMutexGuard() {
        if (_locked) xSemaphoreGive(_mutex);
    }
    bool locked() const { return _locked; }

private:
    SemaphoreHandle_t _mutex;
    bool _locked;
};

String cardTypeName(uint8_t type) {
    switch (type) {
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SDSC";
        case CARD_SDHC: return "SDHC";
        case CARD_NONE: return "none";
        default: return "unknown";
    }
}
}

SDCardManager& SDCardManager::getInstance() {
    static SDCardManager instance;
    return instance;
}

bool SDCardManager::ensureMutex() const {
    if (mutexHandle()) return true;
    void* created = static_cast<void*>(xSemaphoreCreateMutex());
    if (!created) return mutexHandle() != nullptr;
    void* expected = nullptr;
    if (!_mutex.compare_exchange_strong(expected, created,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(created));
    }
    return true;
}

void SDCardManager::setErrorLocked(const String& errorMsg) {
    _status.lastError = errorMsg;
    if (!errorMsg.isEmpty()) LOG_WARNINGF("SD card: %s", errorMsg.c_str());
}

void SDCardManager::recordIoFailureLocked(const String& errorMsg) {
    _status.capacityValid = false;
    _status.recoverySuggested = true;
    if (_status.ioFailures != UINT32_MAX) ++_status.ioFailures;
    const int separator = errorMsg.indexOf(':');
    _status.lastFailureCode = separator > 0 ? errorMsg.substring(0, separator) : "SD_IO_FAILED";
    setErrorLocked(errorMsg);
}

bool SDCardManager::normalizeSandboxPath(const String& input,
                                         String& normalized,
                                         String& errorMsg,
                                         bool allowRoot) {
    normalized = input;
    normalized.trim();
    normalized.replace('\\', '/');
    if (normalized.isEmpty() || normalized == "/") normalized = SD_SANDBOX_ROOT;
    if (!normalized.startsWith("/")) normalized = String(SD_SANDBOX_ROOT) + "/" + normalized;

    if (normalized.indexOf("..") >= 0 || normalized.indexOf("//") >= 0 ||
        normalized.indexOf(':') >= 0 || normalized.length() > 128) {
        errorMsg = "SD_PATH_INVALID: path is outside the /fastbee sandbox";
        return false;
    }
    if (normalized != SD_SANDBOX_ROOT && !normalized.startsWith(String(SD_SANDBOX_ROOT) + "/")) {
        errorMsg = "SD_PATH_FORBIDDEN: only /fastbee is accessible";
        return false;
    }
    if (!allowRoot && normalized == SD_SANDBOX_ROOT) {
        errorMsg = "SD_PATH_FORBIDDEN: the sandbox root cannot be modified";
        return false;
    }

    for (size_t i = 0; i < normalized.length(); ++i) {
        const char c = normalized[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '/' || c == '_' ||
                             c == '-' || c == '.';
        if (!allowed) {
            errorMsg = "SD_PATH_INVALID: path contains unsupported characters";
            return false;
        }
    }
    return true;
}

bool SDCardManager::configureAndMount(const String& ownerId, uint8_t sckPin,
                                      uint8_t mosiPin, uint8_t misoPin,
                                      uint8_t csPin, uint32_t frequency,
                                      String& errorMsg) {
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(mutexHandle());
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (ownerId.isEmpty() || sckPin == 255 || mosiPin == 255 ||
        misoPin == 255 || csPin == 255) {
        errorMsg = "SD_CONFIG_INVALID: owner and four SDSPI pins are required";
        setErrorLocked(errorMsg);
        return false;
    }
    if (_status.configured && _status.ownerId != ownerId) {
        errorMsg = String("SD_RESOURCE_BUSY: mounted by '") + _status.ownerId + "'";
        setErrorLocked(errorMsg);
        return false;
    }

    FastBee::SpiBusOperation busOperation;
    if (!busOperation.locked()) {
        errorMsg = "SPI_BUSY: SD configuration retained";
        return false;
    }
    if (_status.mounted) SD.end();
    _status.capacityValid = false;
    _status.mounted = false;
    if (_recordingGeneration == UINT64_MAX) { errorMsg = "SD_RECORD_GENERATION_EXHAUSTED"; return false; }
    ++_recordingGeneration;
    if (!FastBee::SpiBusInitializer::instance().release(this, errorMsg)) return false;
    _status = SDCardStatus();
    _status.configured = true;
    _status.ownerId = ownerId;
    _status.sckPin = sckPin;
    _status.mosiPin = mosiPin;
    _status.misoPin = misoPin;
    _status.csPin = csPin;
    _status.frequency = (frequency >= SD_MIN_FREQUENCY && frequency <= SD_MAX_FREQUENCY)
        ? frequency : SD_DEFAULT_FREQUENCY;
    return ensureMountedLocked(errorMsg);
}

bool SDCardManager::ensureMountedLocked(String& errorMsg) {
    errorMsg = "";
    FastBee::SpiBusOperation busOperation;
    if (!busOperation.locked()) {
        errorMsg = "SPI_BUSY: SD mount deferred";
        setErrorLocked(errorMsg);
        return false;
    }
    if (_status.mounted) {
        if (FastBee::SpiBusInitializer::instance().isAcquired(this) && SPI.bus()) return true;
        errorMsg = "SD_SPI_LEASE_LOST: unmount before recovering the bus";
        recordIoFailureLocked(errorMsg);
        return false;
    }
    if (!_status.configured) {
        errorMsg = "SD_NOT_CONFIGURED: no SDSPI peripheral is configured";
        setErrorLocked(errorMsg);
        return false;
    }

    if (!FastBee::SpiBusInitializer::instance().acquire(
            this, _status.sckPin, _status.misoPin, _status.mosiPin, _status.csPin, errorMsg,
            FastBee::ResourceConsumerRole::SdCard, _status.ownerId.c_str())) {
        setErrorLocked(errorMsg);
        return false;
    }
    const auto failMount = [&](const char* reason) {
        SD.end();
        String releaseError;
        const bool released = FastBee::SpiBusInitializer::instance().release(this, releaseError);
        errorMsg = reason;
        if (!released) errorMsg += String("; ") + releaseError;
        recordIoFailureLocked(errorMsg);
        return false;
    };
    pinMode(_status.csPin, OUTPUT);
    digitalWrite(_status.csPin, HIGH);
    if (!SD.begin(_status.csPin, SPI, _status.frequency, "/sd", 4, false)) {
        return failMount("SD_MOUNT_FAILED: no readable FAT card was detected");
    }
    if (SD.cardType() == CARD_NONE) {
        return failMount("SD_CARD_MISSING: card initialization returned no media");
    }
    if (!SD.exists(SD_SANDBOX_ROOT) && !SD.mkdir(SD_SANDBOX_ROOT)) {
        return failMount("SD_SANDBOX_CREATE_FAILED: unable to create /fastbee");
    }

    _status.mounted = true;
    _status.lastError = "";
    _status.recoverySuggested = false;
    updateCapacityLocked();
    LOG_INFOF("SD card: mounted owner=%s SCK=%u MOSI=%u MISO=%u CS=%u freq=%lu",
              _status.ownerId.c_str(), _status.sckPin, _status.mosiPin,
              _status.misoPin, _status.csPin,
              static_cast<unsigned long>(_status.frequency));
    return true;
}

bool SDCardManager::checkOwnerLocked(const String& ownerId, String& errorMsg) const {
    // Empty owner is retained for existing internal calls. API callers must
    // supply the requested peripheral ID; compare under the operation's lock.
    if (!ownerId.isEmpty() && (!_status.configured || _status.ownerId != ownerId)) {
        errorMsg = "SD_OWNER_MISMATCH: peripheral does not own storage";
        return false;
    }
    return true;
}

bool SDCardManager::requireMountedLocked(String& errorMsg) {
    // File operations (including HTTP GET) cannot initialize media or create
    // /fastbee. An explicit mount/recover is required after unmount/failure.
    if (!_status.configured || !_status.mounted) {
        errorMsg = "SD_NOT_MOUNTED: explicitly mount or recover before file access";
        return false;
    }
    if (!FastBee::SpiBusInitializer::instance().isAcquired(this) || !SPI.bus()) {
        errorMsg = "SD_SPI_LEASE_LOST: explicitly recover storage before file access";
        recordIoFailureLocked(errorMsg);
        return false;
    }
    errorMsg = "";
    return true;
}

bool SDCardManager::recover(const String& ownerId, String& errorMsg) {
    errorMsg = "";
    if (!ensureMutex()) { errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock"; return false; }
    SDMutexGuard guard(mutexHandle());
    if (!guard.locked()) { errorMsg = "SD_BUSY: storage is busy"; return false; }
    if (ownerId.isEmpty() || !_status.configured || !checkOwnerLocked(ownerId, errorMsg)) {
        if (errorMsg.isEmpty()) errorMsg = "SD_OWNER_MISMATCH: recovery requires the configured owner";
        return false;
    }
    FastBee::SpiBusOperation busOperation;
    if (!busOperation.locked()) { errorMsg = "SPI_BUSY: recovery did not start"; return false; }
    if (_status.recoveryAttempts != UINT32_MAX) ++_status.recoveryAttempts;
    if (_recordingGeneration == UINT64_MAX || _recordingRecovery == UINT64_MAX) {
        errorMsg = "SD_RECORD_GENERATION_EXHAUSTED"; return false;
    }
    ++_recordingGeneration;
    _status.recoverySuggested = true;
    _status.capacityValid = false;
    if (_status.mounted) SD.end();
    _status.mounted = false;
    if (!FastBee::SpiBusInitializer::instance().release(this, errorMsg)) {
        recordIoFailureLocked(errorMsg); return false;
    }
    if (!ensureMountedLocked(errorMsg)) return false;
    ++_recordingRecovery;
    if (_status.recoveries != UINT32_MAX) ++_status.recoveries;
    return true;
}

bool SDCardManager::mount(String& errorMsg, const String& ownerId) {
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(mutexHandle());
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    return checkOwnerLocked(ownerId, errorMsg) && ensureMountedLocked(errorMsg);
}

bool SDCardManager::unmount(const String& ownerId, String* errorMsg,
                            bool releaseConfiguration) {
    if (!ensureMutex()) return false;
    SDMutexGuard guard(mutexHandle());
    if (!guard.locked()) {
        if (errorMsg) *errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (!ownerId.isEmpty() && _status.configured && ownerId != _status.ownerId) {
        if (errorMsg) *errorMsg = "SD_OWNER_MISMATCH: peripheral does not own the mount";
        return false;
    }
    FastBee::SpiBusOperation busOperation;
    if (!busOperation.locked()) {
        if (errorMsg) *errorMsg = "SPI_BUSY: SD mount retained";
        return false;
    }
    if (_status.mounted) SD.end();
    _status.capacityValid = false;
    _status.mounted = false;
    String releaseError;
    if (_recordingGeneration != UINT64_MAX) ++_recordingGeneration;
    if (!FastBee::SpiBusInitializer::instance().release(this, releaseError)) {
        if (errorMsg) *errorMsg = releaseError;
        setErrorLocked(releaseError);
        return false;
    }
    if (releaseConfiguration) {
        _status = SDCardStatus();
    } else {
        _status.mounted = false;
        _status.cardBytes = 0;
        _status.totalBytes = 0;
        _status.usedBytes = 0;
        _status.cardType = "";
        _status.lastError = "";
    }
    return true;
}

void SDCardManager::updateCapacityLocked() {
    if (!_status.mounted) return;
    _status.cardType = cardTypeName(static_cast<uint8_t>(SD.cardType()));
    _status.cardBytes = SD.cardSize();
    _status.totalBytes = SD.totalBytes();
    _status.usedBytes = SD.usedBytes();
    const int64_t sampledAtUs = esp_timer_get_time();
    _status.capacitySampledAtMs = sampledAtUs >= 0 ? static_cast<uint64_t>(sampledAtUs) / 1000 : 0;
    _status.capacityAgeMs = 0;
    _status.capacityValid = sampledAtUs >= 0 && _status.totalBytes > 0 && _status.usedBytes <= _status.totalBytes;
}

SDCardStatus SDCardManager::getStatus() const {
    SDCardStatus snapshot;
    void* mutex = mutexHandle();
    if (!mutex) {
        snapshot.statusAvailable = true;
        return snapshot;
    }
    SDMutexGuard guard(mutex, 0);
    if (!guard.locked()) {
        snapshot.lastError = "SD_BUSY: storage status unavailable";
        return snapshot;
    }
    // Same client -> bus lock order as operations, but diagnostics never create
    // locks, wait for an operation, or query the filesystem/card.
    FastBee::SpiBusObservation busObservation;
    if (!busObservation.unused() && !busObservation.locked()) {
        snapshot.lastError = "SPI_BUSY: storage status unavailable";
        return snapshot;
    }
    if (_status.mounted &&
        (!FastBee::SpiBusInitializer::instance().isAcquired(this, 0) || !SPI.bus())) {
        snapshot = _status;
        snapshot.recordingGeneration = _recordingGeneration;
        snapshot.recordingRecovery = _recordingRecovery;
        snapshot.configurationAvailable = true;
        snapshot.statusAvailable = false;
        snapshot.mounted = false;
        snapshot.capacityValid = false;
        snapshot.recoverySuggested = true;
        snapshot.lastError = "SD_SPI_LEASE_LOST: capacity snapshot not refreshed";
        return snapshot;
    }
    snapshot = _status;
    snapshot.recordingGeneration = _recordingGeneration;
    snapshot.recordingRecovery = _recordingRecovery;
    snapshot.configurationAvailable = true;
    snapshot.statusAvailable = true;
    if (snapshot.capacityValid) {
        const int64_t nowUs = esp_timer_get_time();
        const uint64_t nowMs = nowUs >= 0 ? static_cast<uint64_t>(nowUs) / 1000 : 0;
        // Never turn clock rollback/unavailability into a fresh zero or an
        // unsigned huge age. A later valid clock observation can expose the
        // original snapshot again; no card query or remount is performed here.
        if (nowUs < 0 || nowMs < snapshot.capacitySampledAtMs) snapshot.capacityValid = false;
        else snapshot.capacityAgeMs = nowMs - snapshot.capacitySampledAtMs;
    }
    return snapshot;
}

bool SDCardManager::list(const String& path, std::vector<SDCardEntry>& entries,
                         String& errorMsg, const String& ownerId, bool* truncated) {
    String normalized;
    if (!normalizeSandboxPath(path, normalized, errorMsg)) return false;
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(mutexHandle());
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (!checkOwnerLocked(ownerId, errorMsg)) return false;
    FastBee::SpiBusOperation busOperation;
    if (!busOperation.locked()) {
        errorMsg = "SPI_BUSY: SD file operation deferred";
        return false;
    }
    if (!requireMountedLocked(errorMsg)) return false;

    File dir = SD.open(normalized, FILE_READ);
    if (!dir || !dir.isDirectory()) {
        dir.close();
        errorMsg = "SD_DIRECTORY_NOT_FOUND: directory does not exist";
        setErrorLocked(errorMsg);
        return false;
    }
    std::vector<SDCardEntry> observed;
    File file = dir.openNextFile();
    while (file && observed.size() < MAX_LIST_ENTRIES) {
        SDCardEntry entry;
        entry.path = file.path();
        String verified, pathError;
        const String prefix = normalized.endsWith("/") ? normalized : normalized + "/";
        if (!normalizeSandboxPath(entry.path, verified, pathError, false) ||
            verified != entry.path || !entry.path.startsWith(prefix) || entry.path.length() <= prefix.length() ||
            entry.path.substring(prefix.length()).indexOf('/') >= 0) {
            file.close();
            dir.close();
            errorMsg = "SD_DIRECTORY_ENTRY_INVALID: entry is outside the supported directory contract";
            recordIoFailureLocked(errorMsg);
            return false;
        }
        entry.directory = file.isDirectory();
        entry.size = entry.directory ? 0 : file.size();
        observed.push_back(entry);
        file.close();
        file = dir.openNextFile();
    }
    const bool hasMore = static_cast<bool>(file);
    file.close();
    dir.close();
    entries.swap(observed);
    if (truncated) *truncated = hasMore;
    // Listing is a filesystem read, not a fresh capacity measurement. It must
    // not hide an earlier invalid sample or make an old one appear recent.
    _status.lastError = "";
    return true;
}

bool SDCardManager::readFile(const String& path, String& content,
                             String& errorMsg, size_t maxBytes, const String& ownerId) {
    String normalized;
    if (!normalizeSandboxPath(path, normalized, errorMsg, false)) return false;
    if (maxBytes == 0 || maxBytes > MAX_FILE_BYTES) maxBytes = MAX_FILE_BYTES;
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(mutexHandle());
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (!checkOwnerLocked(ownerId, errorMsg)) return false;
    FastBee::SpiBusOperation busOperation;
    if (!busOperation.locked()) {
        errorMsg = "SPI_BUSY: SD file operation deferred";
        return false;
    }
    if (!requireMountedLocked(errorMsg)) return false;
    File file = SD.open(normalized, FILE_READ);
    if (!file || file.isDirectory()) {
        errorMsg = "SD_FILE_NOT_FOUND: file does not exist";
        setErrorLocked(errorMsg);
        return false;
    }
    const auto result = FastBee::readWholeFile(file, maxBytes, content);
    file.close();
    if (result != FastBee::FileReadResult::Ok) {
        switch (result) {
            case FastBee::FileReadResult::TooLarge:
                errorMsg = "SD_FILE_TOO_LARGE: file exceeds the bounded read limit"; break;
            case FastBee::FileReadResult::NoMemory:
                errorMsg = "SD_READ_NO_MEMORY: complete file buffer unavailable"; break;
            case FastBee::FileReadResult::Changed:
                errorMsg = "SD_READ_CHANGED: file size or end changed during read"; break;
            default:
                errorMsg = "SD_READ_INCOMPLETE: card did not provide the complete file"; break;
        }
        if (result == FastBee::FileReadResult::Incomplete || result == FastBee::FileReadResult::Changed)
            recordIoFailureLocked(errorMsg);
        else setErrorLocked(errorMsg);
        return false;
    }
    _status.lastError = "";
    return true;
}

bool SDCardManager::writeFile(const String& path, const uint8_t* data,
                              size_t len, bool append, String& errorMsg, const String& ownerId) {
    String normalized;
    if (!normalizeSandboxPath(path, normalized, errorMsg, false)) return false;
    if (isRecordingPath(normalized)) { errorMsg = "SD_RECORD_PATH_RESERVED"; return false; }
    if (!data || len == 0 || len > MAX_FILE_BYTES) {
        errorMsg = "SD_WRITE_SIZE_INVALID: payload must be 1-16384 bytes";
        return false;
    }
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(mutexHandle());
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (!checkOwnerLocked(ownerId, errorMsg)) return false;
    FastBee::SpiBusOperation busOperation;
    if (!busOperation.locked()) {
        errorMsg = "SPI_BUSY: SD file operation deferred";
        return false;
    }
    if (!requireMountedLocked(errorMsg)) return false;
    if (append && SD.exists(normalized)) {
        File existing = SD.open(normalized, FILE_READ);
        if (!existing || existing.isDirectory()) {
            existing.close();
            errorMsg = "SD_APPEND_SIZE_UNCONFIRMED: existing file could not be checked";
            recordIoFailureLocked(errorMsg);
            return false;
        }
        const size_t existingSize = existing.size();
        existing.close();
        if (existingSize > MAX_FILE_BYTES || len > MAX_FILE_BYTES - existingSize) {
            errorMsg = "SD_FILE_TOO_LARGE: append would exceed 16384 bytes";
            setErrorLocked(errorMsg);
            return false;
        }
    }
    _status.capacityValid = false; // A failed/partial write may already have changed storage.
    File file = SD.open(normalized, append ? FILE_APPEND : FILE_WRITE, true);
    if (!file) {
        errorMsg = "SD_OPEN_FAILED: unable to open file for writing";
        recordIoFailureLocked(errorMsg);
        return false;
    }
    const size_t written = file.write(data, len);
    file.flush();
    file.close();
    if (written != len) {
        errorMsg = "SD_WRITE_TRUNCATED: card accepted only part of the payload";
        recordIoFailureLocked(errorMsg);
        return false;
    }
    updateCapacityLocked();
    _status.lastError = "";
    return true;
}

bool SDCardManager::removeFile(const String& path, String& errorMsg, const String& ownerId) {
    String normalized;
    if (!normalizeSandboxPath(path, normalized, errorMsg, false)) return false;
    if (isRecordingPath(normalized)) { errorMsg = "SD_RECORD_PATH_RESERVED"; return false; }
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(mutexHandle());
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (!checkOwnerLocked(ownerId, errorMsg)) return false;
    FastBee::SpiBusOperation busOperation;
    if (!busOperation.locked()) {
        errorMsg = "SPI_BUSY: SD file operation deferred";
        return false;
    }
    if (!requireMountedLocked(errorMsg)) return false;
    if (!SD.exists(normalized)) {
        errorMsg = "SD_FILE_NOT_FOUND: file does not exist";
        setErrorLocked(errorMsg);
        return false;
    }
    File target = SD.open(normalized, FILE_READ);
    if (!target) {
        errorMsg = "SD_DELETE_TARGET_UNCONFIRMED: unable to inspect the target";
        recordIoFailureLocked(errorMsg);
        return false;
    }
    const bool directory = target && target.isDirectory();
    target.close();
    _status.capacityValid = false;
    const bool removed = directory ? SD.rmdir(normalized) : SD.remove(normalized);
    if (!removed) {
        errorMsg = "SD_DELETE_FAILED: file or empty directory could not be removed";
        recordIoFailureLocked(errorMsg);
        return false;
    }
    updateCapacityLocked();
    _status.lastError = "";
    return true;
}
