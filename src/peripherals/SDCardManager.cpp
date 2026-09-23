#include "peripherals/SDCardManager.h"

#include "systems/LoggerSystem.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
constexpr const char* SD_SANDBOX_ROOT = "/fastbee";
constexpr uint32_t SD_DEFAULT_FREQUENCY = 20000000UL;
constexpr uint32_t SD_MIN_FREQUENCY = 100000UL;
constexpr uint32_t SD_MAX_FREQUENCY = 40000000UL;

class SDMutexGuard {
public:
    explicit SDMutexGuard(void* mutex)
        : _mutex(static_cast<SemaphoreHandle_t>(mutex)),
          _locked(_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {}
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
    if (_mutex) return true;
    _mutex = static_cast<void*>(xSemaphoreCreateMutex());
    return _mutex != nullptr;
}

void SDCardManager::setErrorLocked(const String& errorMsg) {
    _status.lastError = errorMsg;
    if (!errorMsg.isEmpty()) LOG_WARNINGF("SD card: %s", errorMsg.c_str());
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
    SDMutexGuard guard(_mutex);
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

    if (_status.mounted) SD.end();
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
    if (_status.mounted) return true;
    if (!_status.configured) {
        errorMsg = "SD_NOT_CONFIGURED: no SDSPI peripheral is configured";
        setErrorLocked(errorMsg);
        return false;
    }

    pinMode(_status.csPin, OUTPUT);
    digitalWrite(_status.csPin, HIGH);
    SPI.begin(_status.sckPin, _status.misoPin, _status.mosiPin, _status.csPin);
    if (!SD.begin(_status.csPin, SPI, _status.frequency, "/sd", 4, false)) {
        errorMsg = "SD_MOUNT_FAILED: no readable FAT card was detected";
        setErrorLocked(errorMsg);
        return false;
    }
    if (SD.cardType() == CARD_NONE) {
        SD.end();
        errorMsg = "SD_CARD_MISSING: card initialization returned no media";
        setErrorLocked(errorMsg);
        return false;
    }
    if (!SD.exists(SD_SANDBOX_ROOT) && !SD.mkdir(SD_SANDBOX_ROOT)) {
        SD.end();
        errorMsg = "SD_SANDBOX_CREATE_FAILED: unable to create /fastbee";
        setErrorLocked(errorMsg);
        return false;
    }

    _status.mounted = true;
    _status.lastError = "";
    updateCapacityLocked();
    LOG_INFOF("SD card: mounted owner=%s SCK=%u MOSI=%u MISO=%u CS=%u freq=%lu",
              _status.ownerId.c_str(), _status.sckPin, _status.mosiPin,
              _status.misoPin, _status.csPin,
              static_cast<unsigned long>(_status.frequency));
    return true;
}

bool SDCardManager::mount(String& errorMsg) {
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(_mutex);
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    return ensureMountedLocked(errorMsg);
}

bool SDCardManager::unmount(const String& ownerId, String* errorMsg,
                            bool releaseConfiguration) {
    if (!ensureMutex()) return false;
    SDMutexGuard guard(_mutex);
    if (!guard.locked()) {
        if (errorMsg) *errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (!ownerId.isEmpty() && _status.configured && ownerId != _status.ownerId) {
        if (errorMsg) *errorMsg = "SD_OWNER_MISMATCH: peripheral does not own the mount";
        return false;
    }
    if (_status.mounted) SD.end();
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
}

SDCardStatus SDCardManager::getStatus() const {
    if (!ensureMutex()) return SDCardStatus();
    SDMutexGuard guard(_mutex);
    if (!guard.locked()) return SDCardStatus();
    const_cast<SDCardManager*>(this)->updateCapacityLocked();
    return _status;
}

bool SDCardManager::list(const String& path, std::vector<SDCardEntry>& entries,
                         String& errorMsg) {
    String normalized;
    if (!normalizeSandboxPath(path, normalized, errorMsg)) return false;
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(_mutex);
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (!ensureMountedLocked(errorMsg)) return false;

    File dir = SD.open(normalized, FILE_READ);
    if (!dir || !dir.isDirectory()) {
        errorMsg = "SD_DIRECTORY_NOT_FOUND: directory does not exist";
        setErrorLocked(errorMsg);
        return false;
    }
    entries.clear();
    File file = dir.openNextFile();
    while (file && entries.size() < MAX_LIST_ENTRIES) {
        SDCardEntry entry;
        entry.path = file.path();
        entry.directory = file.isDirectory();
        entry.size = entry.directory ? 0 : file.size();
        entries.push_back(entry);
        file.close();
        file = dir.openNextFile();
    }
    dir.close();
    updateCapacityLocked();
    return true;
}

bool SDCardManager::readFile(const String& path, String& content,
                             String& errorMsg, size_t maxBytes) {
    String normalized;
    if (!normalizeSandboxPath(path, normalized, errorMsg, false)) return false;
    if (maxBytes == 0 || maxBytes > MAX_FILE_BYTES) maxBytes = MAX_FILE_BYTES;
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(_mutex);
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (!ensureMountedLocked(errorMsg)) return false;
    File file = SD.open(normalized, FILE_READ);
    if (!file || file.isDirectory()) {
        errorMsg = "SD_FILE_NOT_FOUND: file does not exist";
        setErrorLocked(errorMsg);
        return false;
    }
    if (file.size() > maxBytes) {
        file.close();
        errorMsg = "SD_FILE_TOO_LARGE: file exceeds the bounded read limit";
        setErrorLocked(errorMsg);
        return false;
    }
    content = "";
    content.reserve(file.size() + 1);
    while (file.available()) content += static_cast<char>(file.read());
    file.close();
    return true;
}

bool SDCardManager::writeFile(const String& path, const uint8_t* data,
                              size_t len, bool append, String& errorMsg) {
    String normalized;
    if (!normalizeSandboxPath(path, normalized, errorMsg, false)) return false;
    if (!data || len == 0 || len > MAX_FILE_BYTES) {
        errorMsg = "SD_WRITE_SIZE_INVALID: payload must be 1-16384 bytes";
        return false;
    }
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(_mutex);
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (!ensureMountedLocked(errorMsg)) return false;
    if (append && SD.exists(normalized)) {
        File existing = SD.open(normalized, FILE_READ);
        const size_t combined = existing ? existing.size() + len : len;
        existing.close();
        if (combined > MAX_FILE_BYTES) {
            errorMsg = "SD_FILE_TOO_LARGE: append would exceed 16384 bytes";
            setErrorLocked(errorMsg);
            return false;
        }
    }
    File file = SD.open(normalized, append ? FILE_APPEND : FILE_WRITE, true);
    if (!file) {
        errorMsg = "SD_OPEN_FAILED: unable to open file for writing";
        setErrorLocked(errorMsg);
        return false;
    }
    const size_t written = file.write(data, len);
    file.flush();
    file.close();
    if (written != len) {
        errorMsg = "SD_WRITE_TRUNCATED: card accepted only part of the payload";
        setErrorLocked(errorMsg);
        return false;
    }
    updateCapacityLocked();
    _status.lastError = "";
    return true;
}

bool SDCardManager::removeFile(const String& path, String& errorMsg) {
    String normalized;
    if (!normalizeSandboxPath(path, normalized, errorMsg, false)) return false;
    if (!ensureMutex()) {
        errorMsg = "SD_MUTEX_UNAVAILABLE: unable to create storage lock";
        return false;
    }
    SDMutexGuard guard(_mutex);
    if (!guard.locked()) {
        errorMsg = "SD_BUSY: storage is busy";
        return false;
    }
    if (!ensureMountedLocked(errorMsg)) return false;
    if (!SD.exists(normalized)) {
        errorMsg = "SD_FILE_NOT_FOUND: file does not exist";
        setErrorLocked(errorMsg);
        return false;
    }
    File target = SD.open(normalized, FILE_READ);
    const bool directory = target && target.isDirectory();
    target.close();
    const bool removed = directory ? SD.rmdir(normalized) : SD.remove(normalized);
    if (!removed) {
        errorMsg = "SD_DELETE_FAILED: file or empty directory could not be removed";
        setErrorLocked(errorMsg);
        return false;
    }
    updateCapacityLocked();
    _status.lastError = "";
    return true;
}
