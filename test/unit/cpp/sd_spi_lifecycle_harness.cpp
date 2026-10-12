#include "peripherals/SDCardManager.h"
#include "core/SpiBusInitializer.h"
#include <SD.h>
#include <freertos/semphr.h>
#include <cassert>
#include <thread>

int main() {
    auto& storage = SDCardManager::getInstance();
    auto& bus = FastBee::SpiBusInitializer::instance();
    String error;
    spiMutexFailAllocation = true;
    assert(!storage.configureAndMount("sd",18,23,19,5,1000000,error));
    assert(error.startsWith("SD_MUTEX_UNAVAILABLE") && SPI.begins == 0);
    spiMutexFailAllocation = false;
    spiMutexRace = true;
    std::atomic<int> successes{0};
    auto configure = [&] { String e; if (storage.configureAndMount("sd",18,23,19,5,1000000,e)) ++successes; };
    std::thread first(configure), second(configure);
    first.join(); second.join(); spiMutexRace = false;
    assert(successes == 2 && spiMutexCreates - spiMutexDeletes == 2);
    assert(bus.isAcquired(&storage) && storage.getStatus().mounted);
    const int liveEnds = SPI.ends, mountEnds = SD.ends;
    assert(!storage.unmount("other", &error));
    assert(error.startsWith("SD_OWNER_MISMATCH") && SPI.ends == liveEnds && SD.ends == mountEnds);
    assert(!storage.configureAndMount("other",18,23,19,6,1000000,error));
    assert(error.startsWith("SD_RESOURCE_BUSY") && SPI.ends == liveEnds);
    // An unrelated owner blocks stopping the bus but not unmounting this card.
    int peer;
    assert(bus.acquire(&peer,18,19,23,6,error));
    assert(storage.unmount("sd", &error, false));
    assert(!bus.isAcquired(&storage) && SPI.active && bus.isAcquired(&peer));
    assert(storage.getStatus().configured && !storage.getStatus().mounted);
    assert(bus.release(&peer,error) && SPI.active);
    assert(bus.end(error) && !SPI.active);
    assert(storage.mount(error) && bus.isAcquired(&storage));
    assert(storage.unmount("sd", &error));
    assert(SPI.active && !storage.getStatus().configured);
    assert(storage.unmount("sd", &error));
    assert(bus.end(error) && !SPI.active);
    // Pin attachment failure never reaches SD.begin or changes CS.
    SPI.failAfterAllocation = true;
    const int beforeModes = sdPinModes, beforeMounts = SD.begins;
    assert(!storage.configureAndMount("sd",18,23,19,5,1000000,error));
    assert(error.startsWith("SPI_INIT_FAILED") && !SPI.active && !bus.isAcquired(&storage));
    assert(sdPinModes == beforeModes && SD.begins == beforeMounts);
    SPI.failAfterAllocation = false;
    // Every post-begin failure retires the real lease and closes the SD layer.
    for (int failure = 0; failure < 3; ++failure) {
        SD.mountOk = failure != 0;
        SD.card = failure == 1 ? CARD_NONE : CARD_SDHC;
        SD.directoryExists = failure != 2; SD.mkdirOk = failure != 2;
        const int beforeEnds = SD.ends;
        assert(!storage.mount(error));
        const char* expected[] = {"SD_MOUNT_FAILED","SD_CARD_MISSING","SD_SANDBOX_CREATE_FAILED"};
        assert(error.startsWith(expected[failure]));
        assert(SPI.active && !bus.isAcquired(&storage) && SD.ends == beforeEnds + 1);
        assert(!storage.getStatus().mounted && storage.getStatus().configured);
    }
    SD.mountOk = true; SD.card = CARD_SDHC; SD.directoryExists = true; SD.mkdirOk = true;
    // Legacy global-SPI users are retained when SD alone exits or fails.
    assert(bus.begin(18,19,23,6,error));
    SD.mountOk = false;
    assert(!storage.mount(error) && SPI.active && !bus.isAcquired(&storage));
    SD.mountOk = true;
    assert(storage.mount(error));
    assert(storage.unmount("sd", &error, false) && SPI.active);
    assert(bus.end(error) && !SPI.active);
    for (int cycle = 0; cycle < 1000; ++cycle) {
        assert(storage.mount(error));
        assert(storage.unmount("sd", &error, false));
        assert(SPI.active && !bus.isAcquired(&storage));
    }
    assert(storage.mount(error));
    String content;
    assert(storage.readFile("/fastbee/test.txt",content,error) && content == "abc");
    std::vector<SDCardEntry> entries;
    assert(storage.list("/fastbee",entries,error));
    const uint8_t bytes[] = {1,2,3};
    assert(storage.writeFile("/fastbee/test.txt",bytes,3,false,error));
    assert(storage.removeFile("/fastbee/test.txt",error));
    const int beforeInvalid = SD.opens;
    assert(!storage.readFile("../secret",content,error));
    assert(SD.opens == beforeInvalid);
    // A read retains the operation lock through file close, blocking bus end.
    blockSdRead = true;
    std::thread reading([&] { String c,e; assert(storage.readFile("/fastbee/test.txt",c,e)); });
    while (!sdReadEntered.load()) std::this_thread::yield();
    const int beforeBusy = SPI.ends;
    assert(!bus.end(error) && error.startsWith("SPI_BUSY"));
    assert(SPI.ends == beforeBusy && SPI.active);
    allowSdRead = true; reading.join(); blockSdRead = false;
    assert(storage.unmount("sd", &error, false) && SPI.active);
    assert(storage.mount(error));
    // Externally lost hardware is diagnosed, not silently repaired under leases.
    SPI.end();
    const int beforeLost = SD.opens, beforeCapacity = SD.capacityReads;
    assert(!storage.readFile("/fastbee/test.txt",content,error));
    assert(error.startsWith("SD_SPI_LEASE_LOST") && SD.opens == beforeLost);
    assert(storage.getStatus().lastError.startsWith("SD_SPI_LEASE_LOST"));
    assert(SD.capacityReads == beforeCapacity);
    assert(storage.unmount("sd", &error, false));
    assert(storage.mount(error));
    assert(storage.unmount("sd", &error));
    assert(SPI.active && !bus.isAcquired(&storage));
    assert(bus.end(error) && !SPI.active);
    assert(spiMutexCreates - spiMutexDeletes == 2);
}
