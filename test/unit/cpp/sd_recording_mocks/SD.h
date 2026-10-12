#pragma once
#include <FS.h>
#include <SPI.h>
#include <cassert>
constexpr int CARD_NONE = 0, CARD_MMC = 1, CARD_SD = 2, CARD_SDHC = 3;
constexpr int OUTPUT = 1;
inline void pinMode(uint8_t, int) {}
inline void digitalWrite(uint8_t, int) {}
inline bool recordingExistsFalse = false, recordingCreateBeforeOpen = false;
inline const char* recordingRacedBytes = "preserve concurrent historical evidence";
class MockSD {
public:
    bool begin(uint8_t, SPIClass&, uint32_t, const char*, int, bool format) {
        assert(SPI.active && !format); mounted = true; return true;
    }
    void end() { mounted = false; }
    int cardType() { return CARD_SDHC; }
    bool exists(const String& path) {
        assert(SPI.active);
        if (recordingExistsFalse && path.startsWith("/fastbee/rec-")) return false;
        return recordedFiles.count(std::string(path));
    }
    bool mkdir(const String& path) {
        auto data = std::make_shared<RecordedFile>(); data->directory = true;
        recordedFiles[std::string(path)] = data; return true;
    }
    bool rmdir(const String& path) { return remove(path); }
    bool remove(const String& path) {
        assert(SPI.active); ++recordingDeletes;
        return recordingDeleteOk && recordedFiles.erase(std::string(path)) != 0;
    }
    uint64_t cardSize() { return 1024 * 1024; }
    uint64_t totalBytes() { return 1024 * 1024; }
    uint64_t usedBytes() { return 128; }
    File open(const String& path, const char* mode, bool = false) {
        assert(SPI.active && mounted);
        if (!recordingOpenOk) return {};
        // Fault seam: a file appears after the driver's existence observation,
        // or is recreated after its current-session rotation removal.
        if (*mode != 'r' && recordingCreateBeforeOpen) {
            recordingCreateBeforeOpen = false;
            auto concurrent = std::make_shared<RecordedFile>();
            concurrent->bytes = recordingRacedBytes;
            recordedFiles[std::string(path)] = concurrent;
        }
        auto found = recordedFiles.find(std::string(path));
        if (*mode == 'r') return found == recordedFiles.end() ? File{} : File(found->second, std::string(path));
        // Confirmed against installed Core3 -> newlib O_EXCL -> FAT CREATE_NEW.
        // Reject before any mutation; this is the filesystem/SDK boundary.
        if (std::strchr(mode, 'x') && found != recordedFiles.end()) return {};
        if (found == recordedFiles.end()) {
            recordedFiles[std::string(path)] = std::make_shared<RecordedFile>();
            found = recordedFiles.find(std::string(path));
        }
        if (*mode == 'w') found->second->bytes.clear();
        return File(found->second, std::string(path), *mode == 'a');
    }
    bool mounted = false;
};
inline MockSD SD;
