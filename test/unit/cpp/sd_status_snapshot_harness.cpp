#include "peripherals/SDCardManager.h"
#include "core/SpiBusInitializer.h"
#include "network/SDCardStatusJson.h"
#include <SD.h>
#include <esp_timer.h>
#include <freertos/semphr.h>
#include <cassert>
#include <thread>
#include <string>

static void mount(SDCardManager& manager) {
    String error;
    assert(manager.configureAndMount("sd-a", 18, 23, 19, 5, 1000000, error));
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode(argv[1]);
    auto& manager = SDCardManager::getInstance();
    String error;
    if (mode == "cold") {
        spiMutexFailAllocation = true;
        for (int i=0; i<1000; ++i) {
            const auto snapshot = manager.getStatus();
            assert(snapshot.statusAvailable && !snapshot.configured && !snapshot.mounted && !snapshot.capacityValid);
        }
        assert(spiMutexCreates == 0 && spiMutexTakes == 0 && SPI.begins == 0 && SD.capacityReads == 0);
        return 0;
    }
    if (mode == "json") {
        JsonDocument doc;
        JsonObject data = doc.to<JsonObject>();
        data["id"] = "sd-a";
        SDCardStatus status;
        status.statusAvailable = status.configured = status.mounted = status.capacityValid = true;
        status.ownerId = "sd-a"; status.cardType = "SDHC";
        status.cardBytes = 8192; status.totalBytes = 4096; status.usedBytes = 128; status.capacityAgeMs = 42;
        status.lastError = "retained diagnostic text";
        FastBee::writeSdCardStatusJson(data, status, "sd-a");
        assert(data["mounted"] == true && data["capacityQuality"] == "last_operation_snapshot");
        assert(data["capacityAgeMs"] == 42 && data["usedBytes"] == 128);
        status.cardType = "overwritten source"; status.lastError = "changed";
        assert(data["cardType"] == "SDHC" && data["driverError"] == "retained diagnostic text");
        FastBee::writeSdCardStatusJson(data, status, "sd-b");
        assert(data["configured"] == false && data["mounted"] == false);
        assert(data["totalBytes"].isNull() && data["cardType"].isNull() && data["capacityAgeMs"].isNull());
        assert(data["capacityQuality"] == "unavailable");
        FastBee::writeSdCardStatusJson(data, SDCardStatus{}, "sd-a");
        assert(data["statusAvailable"] == false && data["mounted"].isNull() && data["configured"].isNull());
        assert(data["driverError"].isNull() && data["id"] == "sd-a");
        return 0;
    }
    if (mode == "age_zero") sdMonotonicUs = 0;
    if (mode == "clock_unavailable_at_sample") sdMonotonicUs = -1;
    if (mode == "invalid_capacity") SD.used = SD.total + 1;
    mount(manager);
    if (mode == "age_zero" || mode == "age_multi_wrap" || mode == "age_after_wrap" ||
        mode == "clock_rollback" || mode == "clock_unavailable" || mode == "clock_unavailable_at_sample" ||
        mode == "age_refresh") {
        const auto first = manager.getStatus();
        const int reads = SD.capacityReads, opens = SD.opens, begins = SD.begins, ends = SD.ends;
        const uint64_t longAge = 3ULL * (1ULL << 32) + 37;
        if (mode == "age_zero") assert(first.capacityValid && first.capacitySampledAtMs == 0 && first.capacityAgeMs == 0);
        if (mode == "age_multi_wrap" || mode == "age_refresh")
            sdMonotonicUs = static_cast<int64_t>((first.capacitySampledAtMs + longAge) * 1000);
        if (mode == "age_after_wrap") {
            sdMonotonicUs = static_cast<int64_t>(((1ULL << 32) + 123) * 1000);
            String content;
            assert(manager.readFile("/fastbee/a", content, error, 4096, "sd-a"));
            // Successful read does not resample FAT capacity; use explicit mount
            // after unmount to acquire a new sample at the long uptime.
            assert(manager.getStatus().capacitySampledAtMs == first.capacitySampledAtMs);
            assert(SD.capacityReads == reads);
            assert(manager.unmount("sd-a", &error, false));
            assert(manager.mount(error, "sd-a"));
            assert(manager.getStatus().capacitySampledAtMs == (1ULL << 32) + 123);
            sdMonotonicUs += 42000;
            assert(manager.getStatus().capacityAgeMs == 42);
            return 0;
        }
        if (mode == "clock_rollback") sdMonotonicUs = 0;
        if (mode == "clock_unavailable") sdMonotonicUs = -1;
        if (mode == "clock_unavailable_at_sample") sdMonotonicUs = 1000000;
        const auto later = manager.getStatus();
        assert(later.statusAvailable && later.mounted);
        const bool invalid = mode == "clock_rollback" || mode == "clock_unavailable" || mode == "clock_unavailable_at_sample";
        assert(later.capacityValid != invalid);
        if (mode == "age_multi_wrap" || mode == "age_refresh") assert(later.capacityAgeMs == longAge);
        JsonDocument doc; auto data = doc.to<JsonObject>();
        FastBee::writeSdCardStatusJson(data, later, "sd-a");
        if (invalid) assert(data["capacityAgeMs"].isNull() && data["totalBytes"].isNull());
        else assert(data["capacityAgeMs"].as<uint64_t>() == later.capacityAgeMs);
        assert(SD.capacityReads == reads && SD.opens == opens && SD.begins == begins && SD.ends == ends);
        if (mode == "age_refresh") {
            const uint8_t value[] = {1,2};
            assert(manager.writeFile("/fastbee/a", value, 2, false, error, "sd-a"));
            const auto refreshed = manager.getStatus();
            assert(refreshed.capacityValid && refreshed.capacityAgeMs == 0);
            assert(refreshed.capacitySampledAtMs == first.capacitySampledAtMs + longAge);
        }
        if (mode == "clock_rollback" || mode == "clock_unavailable") {
            sdMonotonicUs = static_cast<int64_t>((first.capacitySampledAtMs + 99) * 1000);
            const auto recovered = manager.getStatus();
            assert(recovered.capacityValid && recovered.capacityAgeMs == 99);
            assert(SD.capacityReads == reads && SD.opens == opens);
        }
        if (mode == "clock_unavailable_at_sample") {
            // A bad acquisition timestamp cannot be repaired by observation;
            // a new successful mount must acquire the first trusted sample.
            assert(manager.unmount("sd-a", &error, false));
            assert(manager.mount(error, "sd-a"));
            assert(manager.getStatus().capacityValid);
            assert(manager.getStatus().capacitySampledAtMs == 1000);
        }
        return 0;
    } else if (mode == "owner") {
        const int reads=SD.capacityReads, opens=SD.opens, begins=SD.begins, ends=SD.ends;
        const uint8_t bytes[]={1,2,3}; String content="keep"; std::vector<SDCardEntry> entries;
        assert(!manager.mount(error,"sd-b") && error.startsWith("SD_OWNER_MISMATCH"));
        assert(!manager.list("/fastbee",entries,error,"sd-b") && error.startsWith("SD_OWNER_MISMATCH"));
        assert(!manager.readFile("/fastbee/a",content,error,4096,"sd-b") && error.startsWith("SD_OWNER_MISMATCH"));
        assert(!manager.writeFile("/fastbee/a",bytes,3,false,error,"sd-b") && error.startsWith("SD_OWNER_MISMATCH"));
        assert(!manager.removeFile("/fastbee/a",error,"sd-b") && error.startsWith("SD_OWNER_MISMATCH"));
        assert(!manager.unmount("sd-b",&error,false) && error.startsWith("SD_OWNER_MISMATCH"));
        assert(SD.capacityReads==reads && SD.opens==opens && SD.begins==begins && SD.ends==ends);
        assert(content=="keep" && manager.getStatus().ownerId=="sd-a");
        assert(manager.readFile("/fastbee/a",content,error,4096,"sd-a") && content=="abc");
        assert(manager.writeFile("/fastbee/a",bytes,3,false,error,"sd-a"));
        assert(manager.list("/fastbee",entries,error,"sd-a"));
        assert(manager.removeFile("/fastbee/a",error,"sd-a"));
        assert(manager.unmount("sd-a",&error));
        assert(!manager.mount(error,"sd-a") && error.startsWith("SD_OWNER_MISMATCH"));
        mount(manager);
    } else if (mode == "snapshot" || mode == "invalid_capacity") {
        const int reads=SD.capacityReads, opens=SD.opens, begins=SD.begins, ends=SD.ends;
        const int allocations=spiMutexCreates, writes=sdPinWrites;
        const auto first=manager.getStatus();
        for (int i=0; i<1000; ++i) {
            const auto snapshot=manager.getStatus();
            assert(snapshot.statusAvailable && snapshot.mounted);
            assert(snapshot.capacityValid == (mode == "snapshot"));
            assert(snapshot.capacitySampledAtMs == first.capacitySampledAtMs);
            assert(snapshot.capacityAgeMs >= first.capacityAgeMs);
            assert(spiMutexLastRequest == 0);
        }
        assert(SD.capacityReads==reads && SD.opens==opens && SD.begins==begins && SD.ends==ends);
        assert(spiMutexCreates==allocations && sdPinWrites==writes);
    } else if (mode == "storage_busy") {
        blockSdRead=true;
        std::thread worker([&] { String content,e; assert(manager.readFile("/fastbee/a",content,e)); });
        while (!sdReadEntered.load()) std::this_thread::yield();
        const int reads=SD.capacityReads;
        for (int i=0; i<100; ++i) {
            const auto snapshot=manager.getStatus();
            assert(!snapshot.statusAvailable && !snapshot.capacityValid && snapshot.lastError.startsWith("SD_BUSY"));
            assert(spiMutexLastRequest==0 && SD.capacityReads==reads);
        }
        allowSdRead=true; worker.join();
        assert(manager.getStatus().statusAvailable);
    } else if (mode == "bus_busy") {
        std::atomic<bool> entered{false}, release{false};
        std::thread worker([&] {
            FastBee::SpiBusOperation operation;
            assert(operation.locked()); entered=true;
            while (!release.load()) std::this_thread::yield();
        });
        while (!entered.load()) std::this_thread::yield();
        const int reads=SD.capacityReads;
        const auto snapshot=manager.getStatus();
        assert(!snapshot.statusAvailable && !snapshot.capacityValid && snapshot.lastError.startsWith("SPI_BUSY"));
        assert(spiMutexLastRequest==0 && SD.capacityReads==reads);
        release=true; worker.join();
        assert(manager.getStatus().statusAvailable);
    } else if (mode == "lost_bus") {
        SPI.end();
        const int reads=SD.capacityReads, begins=SPI.begins;
        const auto snapshot=manager.getStatus();
        assert(!snapshot.statusAvailable && !snapshot.capacityValid && snapshot.lastError.startsWith("SD_SPI_LEASE_LOST"));
        assert(snapshot.configurationAvailable && snapshot.configured && snapshot.ownerId=="sd-a");
        assert(snapshot.recoverySuggested && snapshot.ioFailures==0 && !snapshot.mounted);
        JsonDocument doc;auto data=doc.to<JsonObject>();
        FastBee::writeSdCardStatusJson(data,snapshot,"sd-a");
        assert(data["configurationAvailable"]==true && data["configured"]==true);
        assert(data["mounted"].isNull() && data["totalBytes"].isNull() && data["recoverySuggested"]==true);
        FastBee::writeSdCardStatusJson(data,snapshot,"other");
        assert(data["recoverySuggested"].isNull() && data["ioFailures"].isNull() && data["configured"]==false);
        assert(SD.capacityReads==reads && SPI.begins==begins);
        assert(manager.unmount("sd-a",&error,false));
        assert(manager.mount(error) && manager.getStatus().capacityValid);
    } else if (mode == "partial_write") {
        const uint8_t bytes[]={1,2,3}; sdWriteLimit=1;
        assert(!manager.writeFile("/fastbee/a",bytes,3,false,error));
        auto snapshot=manager.getStatus();
        assert(snapshot.statusAvailable && snapshot.mounted && !snapshot.capacityValid);
        assert(snapshot.lastError.startsWith("SD_WRITE_TRUNCATED"));
        sdWriteLimit=3;
        assert(manager.writeFile("/fastbee/a",bytes,3,false,error));
        assert(manager.getStatus().capacityValid);
    } else if (mode == "unmount") {
        assert(manager.unmount("sd-a",&error,false));
        auto snapshot=manager.getStatus();
        assert(snapshot.statusAvailable && snapshot.configured && !snapshot.mounted && !snapshot.capacityValid);
        assert(manager.mount(error) && manager.getStatus().capacityValid);
        assert(manager.unmount("sd-a",&error));
        snapshot=manager.getStatus();
        assert(snapshot.statusAvailable && !snapshot.configured && !snapshot.capacityValid);
    } else { assert(false); }
    assert(manager.unmount("sd-a",&error));
}
