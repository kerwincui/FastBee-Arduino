#include "peripherals/SDCardManager.h"
#include "core/SpiBusInitializer.h"
#include <SD.h>
#include <freertos/semphr.h>
#include <cassert>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    auto& storage = SDCardManager::getInstance();
    String error;
    assert(storage.configureAndMount("sd", 18, 23, 19, 5, 1000000, error));
    const auto before = storage.getStatus();
    assert(before.recordingGeneration && !before.recordingRecovery);
    SDRecordingSession session; session.token = 0x1020304050607080ULL;
    const std::vector<uint8_t> batch(2048, 'a');
    auto append = [&](const String& owner = "sd", uint64_t generation = 0) {
        return storage.appendRecording(owner, generation ? generation : before.recordingGeneration,
            session, batch.data(), batch.size(), 4096, 2, error);
    };
    if (mode == "owner") {
        assert(!append("other") && recordingWrites == 0 && error.startsWith("SD_OWNER_MISMATCH"));
    } else if (mode == "generation") {
        assert(storage.unmount("sd", &error, false));
        assert(storage.mount(error, "sd"));
        assert(!append() && recordingWrites == 0 && error == "SD_RECORD_STORAGE_CHANGED");
    } else if (mode == "recovery") {
        assert(storage.recover("sd", error));
        const auto after = storage.getStatus();
        assert(after.recordingRecovery > before.recordingRecovery && after.recordingGeneration > before.recordingGeneration);
        assert(!append() && append("sd", after.recordingGeneration));
    } else if (mode == "lease") {
        SPI.end(); assert(!append() && recordingWrites == 0 && error.startsWith("SD_SPI_LEASE_LOST"));
    } else if (mode == "busy") {
        spiMutexFailTake = true;
        bool attempted = true;
        assert(!storage.appendRecording("sd", before.recordingGeneration, session, batch.data(),
            batch.size(), 4096, 2, error, &attempted) && !attempted && recordingWrites == 0);
        spiMutexFailTake = false;
    } else if (mode == "short" || mode == "flush") {
        if (mode == "short") recordingWriteLimit = 7; else recordingFlushError = true;
        assert(!append() && error == "SD_RECORD_WRITE_UNCONFIRMED");
        assert(session.createdMask == 0 && session.sizes[0] == 0);
        assert(!storage.getStatus().capacityValid && storage.getStatus().recoverySuggested);
    } else if (mode == "collision") {
        assert(append());
        SDRecordingSession collided; collided.token = session.token;
        const auto previousFiles = recordedFiles;
        assert(!storage.appendRecording("sd", before.recordingGeneration, collided, batch.data(), batch.size(), 4096, 2, error));
        assert(error == "SD_RECORD_HISTORICAL_COLLISION" && recordingWrites == 1 && recordingDeletes == 0);
        assert(previousFiles.size() == recordedFiles.size());
    } else if (mode == "exists-false") {
        assert(append());
        std::shared_ptr<RecordedFile> original;
        for (const auto& file : recordedFiles) if (!file.second->directory) original = file.second;
        assert(original);
        const uint64_t token = session.token;
        for (const std::string& evidence : {std::string("historical evidence"), std::string()}) {
            original->bytes = evidence;
            session = SDRecordingSession{}; session.token = token;
            recordingExistsFalse = true;
            bool attempted = false;
            assert(!storage.appendRecording("sd", before.recordingGeneration, session, batch.data(),
                batch.size(), 4096, 2, error, &attempted));
            assert(attempted && error == "SD_RECORD_OPEN_FAILED");
            assert(original->bytes == evidence && recordingWrites == 1 && recordingDeletes == 0);
            assert(session.createdMask == 0 && session.sizes[0] == 0);
        }
    } else if (mode == "create-race" || mode == "rotate-race") {
        if (mode == "rotate-race") for (unsigned i = 0; i < 4; ++i) assert(append());
        const SDRecordingSession previous = session;
        const unsigned priorWrites = recordingWrites, priorDeletes = recordingDeletes;
        recordingCreateBeforeOpen = true;
        bool attempted = false;
        assert(!storage.appendRecording("sd", before.recordingGeneration, session, batch.data(),
            batch.size(), 4096, 2, error, &attempted));
        assert(attempted && error == "SD_RECORD_OPEN_FAILED" && recordingWrites == priorWrites);
        assert(recordingDeletes == priorDeletes + (mode == "rotate-race" ? 1U : 0U));
        bool retained = false;
        for (const auto& file : recordedFiles) retained |= file.second->bytes == recordingRacedBytes;
        assert(retained && session.slot == previous.slot && session.createdMask == previous.createdMask);
        for (size_t i = 0; i < 8; ++i) assert(session.sizes[i] == previous.sizes[i]);
        assert(storage.getStatus().recoverySuggested && !storage.getStatus().capacityValid);
    } else if (mode == "changed") {
        assert(append());
        for (auto& file : recordedFiles) if (!file.second->directory) file.second->bytes += "external";
        assert(!append() && error == "SD_RECORD_FILE_CHANGED" && recordingWrites == 1);
    } else if (mode == "reserved") {
        for (const char* path : {"/fastbee/REC-USER.JSONL", "/fastbee/./ReC-USER.JSONL"}) {
            assert(!storage.writeFile(path, batch.data(), 5, false, error, "sd") && error == "SD_RECORD_PATH_RESERVED");
            assert(!storage.removeFile(path, error, "sd") && error == "SD_RECORD_PATH_RESERVED");
        }
        // The sandbox rejects '~' before managed-name checks. Short aliases
        // must remain denied without broadening the allowed path alphabet.
        assert(!storage.writeFile("/fastbee/REC-US~1.JSO", batch.data(), 5, false, error, "sd") && error.startsWith("SD_PATH_INVALID:"));
        assert(!storage.removeFile("/fastbee/REC-US~1.JSO", error, "sd") && error.startsWith("SD_PATH_INVALID:"));
        assert(recordingWrites == 0 && recordingDeletes == 0);
        assert(!storage.writeFile("rec-user.jsonl", batch.data(), 5, false, error, "sd"));
        assert(error == "SD_RECORD_PATH_RESERVED");
        assert(!storage.removeFile("/fastbee/rec-user.jsonl", error, "sd") && recordingDeletes == 0);
        assert(storage.writeFile("user.txt", batch.data(), 5, false, error, "sd"));
    } else if (mode == "rotation" || mode == "rotate-failure") {
        auto historical = std::make_shared<RecordedFile>(); historical->bytes = "preserve original evidence";
        recordedFiles["/fastbee/rec-old-session-0.jsonl"] = historical;
        for (int i = 0; i < 4; ++i) assert(append());
        assert(session.slot == 1 && session.sizes[0] == 4096 && session.sizes[1] == 4096);
        if (mode == "rotate-failure") {
            recordingDeleteOk = false;
            assert(!append() && error == "SD_RECORD_ROTATE_FAILED" && recordingWrites == 4);
        } else {
            for (int i = 0; i < 100; ++i) assert(append());
            assert(recordedFiles.size() == 4 && recordingDeletes == 50);
            assert(session.sizes[0] <= 4096 && session.sizes[1] <= 4096);
            String content;
            for (const auto& file : recordedFiles) if (file.second != historical && !file.second->directory)
                assert(storage.readFile(String(file.first), content, error, 4096, "sd") && content.length() == file.second->bytes.size());
        }
        assert(historical->bytes == "preserve original evidence");
    } else if (mode == "bounds") {
        for (unsigned files : {0U, 1U, 9U})
            assert(!storage.appendRecording("sd", before.recordingGeneration, session, batch.data(), 2048, 4096, files, error));
        assert(!storage.appendRecording("sd", before.recordingGeneration, session, batch.data(), 2049, 4096, 2, error));
        assert(recordingWrites == 0);
    } else { assert(false); }
    return 0;
}
