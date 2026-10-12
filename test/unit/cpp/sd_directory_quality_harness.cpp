#include "peripherals/SDCardManager.h"
#include "core/SpiBusInitializer.h"
#include <SD.h>
#include <esp_timer.h>
#include <cassert>
#include <string>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    auto& storage = SDCardManager::getInstance();
    String error;
    assert(storage.configureAndMount("sd", 18, 23, 19, 5, 1000000, error));
    const auto initial = storage.getStatus();
    sdMonotonicUs = 9000000;
    size_t count = 2;
    if (mode == "empty") count = 0;
    if (mode == "exact") count = 32;
    if (mode == "overflow") count = 33;
    if (mode == "bounded") count = 1000;
    for (size_t i = 0; i < count; ++i)
        sdDirectoryPaths.push_back("/fastbee/file_" + std::to_string(i));
    if (mode == "outside") sdDirectoryPaths[1] = "/other/file";
    if (mode == "nested") sdDirectoryPaths[1] = "/fastbee/nested/file";
    if (mode == "invalid") sdDirectoryPaths[1] = "/fastbee/../file";
    if (mode == "missing") SD.openOk = false;
    if (mode == "failed_capacity") {
        const uint8_t bytes[] = {1, 2, 3};
        sdWriteLimit = 1;
        assert(!storage.writeFile("test.txt", bytes, 3, false, error, "sd"));
        assert(!storage.getStatus().capacityValid);
    }
    const int capacityReads = SD.capacityReads;
    std::vector<SDCardEntry> entries(1);
    entries[0].path = "old snapshot";
    bool truncated = true;
    const bool expected = mode != "outside" && mode != "nested" && mode != "invalid" && mode != "missing";
    const bool listed = storage.list("/fastbee", entries, error, "sd", &truncated);
    assert(listed == expected);
    assert(SD.capacityReads == capacityReads);
    assert(sdDirectoryReads <= 33);
    if (expected) {
        assert(entries.size() == std::min(count, size_t(32)));
        assert(truncated == (count > 32));
        assert(sdDirectoryChildCloses == std::min(count, size_t(33)));
        const auto after = storage.getStatus();
        assert(after.lastError.isEmpty());
        if (mode == "failed_capacity") {
            assert(!after.capacityValid && after.recoverySuggested && after.ioFailures == 1);
        } else {
            assert(after.capacityValid && after.capacitySampledAtMs == initial.capacitySampledAtMs);
            assert(after.capacityAgeMs == 8000);
        }
    } else {
        assert(entries.size() == 1 && entries[0].path == "old snapshot" && truncated);
        if (mode != "missing") {
            assert(error.startsWith("SD_DIRECTORY_ENTRY_INVALID"));
            assert(sdDirectoryChildCloses == 2);
            assert(!storage.getStatus().capacityValid && storage.getStatus().recoverySuggested);
        }
    }
    assert(storage.unmount("sd", &error));
    assert(FastBee::SpiBusInitializer::instance().end(error));
}
