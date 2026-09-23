#ifndef SD_CARD_MANAGER_H
#define SD_CARD_MANAGER_H

#include <Arduino.h>
#include <vector>

struct SDCardStatus {
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
};

struct SDCardEntry {
    String path;
    bool directory = false;
    size_t size = 0;
};

class SDCardManager {
public:
    static constexpr size_t MAX_FILE_BYTES = 16384;
    static constexpr size_t MAX_LIST_ENTRIES = 32;

    static SDCardManager& getInstance();

    bool configureAndMount(const String& ownerId, uint8_t sckPin,
                           uint8_t mosiPin, uint8_t misoPin, uint8_t csPin,
                           uint32_t frequency, String& errorMsg);
    bool mount(String& errorMsg);
    bool unmount(const String& ownerId = "", String* errorMsg = nullptr,
                 bool releaseConfiguration = true);

    SDCardStatus getStatus() const;
    bool list(const String& path, std::vector<SDCardEntry>& entries,
              String& errorMsg);
    bool readFile(const String& path, String& content, String& errorMsg,
                  size_t maxBytes = MAX_FILE_BYTES);
    bool writeFile(const String& path, const uint8_t* data, size_t len,
                   bool append, String& errorMsg);
    bool removeFile(const String& path, String& errorMsg);

    static bool normalizeSandboxPath(const String& input, String& normalized,
                                     String& errorMsg, bool allowRoot = true);

private:
    SDCardManager() = default;
    SDCardManager(const SDCardManager&) = delete;
    SDCardManager& operator=(const SDCardManager&) = delete;

    bool ensureMutex() const;
    bool ensureMountedLocked(String& errorMsg);
    void updateCapacityLocked();
    void setErrorLocked(const String& errorMsg);

    mutable void* _mutex = nullptr;
    SDCardStatus _status;
};

#endif // SD_CARD_MANAGER_H
