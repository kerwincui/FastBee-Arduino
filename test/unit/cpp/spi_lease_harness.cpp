#include "core/SpiBusInitializer.h"
#include <SPI.h>
#include <freertos/semphr.h>
#include <cassert>
#include <thread>
#include <future>
#include <cstring>

int main() {
    auto& bus = FastBee::SpiBusInitializer::instance();
    FastBee::SpiResourceSnapshot snapshot;
    bus.snapshotResources(snapshot);
    assert(snapshot.status == FastBee::ResourceObservation::Observed && snapshot.count == 0);
    assert(!snapshot.initialized && spiMutexCreates == 0 && SPI.busReads == 0);
    int owners[FastBee::SpiBusInitializer::MAX_CLIENTS + 1]{};
    String error;
    assert(!bus.acquire(nullptr,18,19,23,5,error));
    assert(error.startsWith("SPI_OWNER_REQUIRED") && SPI.begins == 0);
    SPI.failAfterAllocation = true;
    assert(!bus.acquire(&owners[0],18,19,23,5,error));
    assert(!SPI.active && !bus.isAcquired(&owners[0]));
    SPI.failAfterAllocation = false;
    assert(bus.acquire(&owners[0],18,19,23,5,error));
    assert(bus.acquire(&owners[0],18,19,23,5,error));
    const int initialized = SPI.begins, ended = SPI.ends;
    assert(!bus.acquire(&owners[0],18,19,23,6,error));
    assert(error.startsWith("SPI_OWNER_CHANGED"));
    // Pointer owners must not silently change their MISO role while leased.
    assert(!bus.acquire(&owners[0],18,-1,23,5,error));
    assert(error.startsWith("SPI_OWNER_CHANGED"));
    assert(bus.isAcquired(&owners[0]) && SPI.begins == initialized && SPI.ends == ended);
    assert(bus.acquire(&owners[1],18,-1,23,6,error));
    assert(!bus.acquire(&owners[1],18,19,23,6,error));
    assert(error.startsWith("SPI_OWNER_CHANGED"));
    assert(bus.isAcquired(&owners[1]) && bus.isAcquired(&owners[0]));
    assert(bus.release(&owners[1],error));
    assert(!bus.acquire(&owners[1],18,19,23,5,error));
    assert(error.startsWith("SPI_CS_CONFLICT"));
    assert(!bus.begin(18,19,23,5,error));
    assert(!bus.end(error) && error.startsWith("SPI_IN_USE"));
    assert(SPI.begins == initialized && SPI.ends == ended);
    spiMutexFailTake = true;
    assert(!bus.release(&owners[0],error) && error.startsWith("SPI_BUSY"));
    snapshot.count = 16;
    bus.snapshotResources(snapshot);
    assert(snapshot.status == FastBee::ResourceObservation::Busy && snapshot.count == 0 && spiMutexLastRequest == 0);
    spiMutexFailTake = false;
    bus.snapshotResources(snapshot);
    assert(snapshot.count == 1); // Failed cleanup has not published a free CS.
    assert(bus.isAcquired(&owners[0]));
    assert(bus.acquire(&owners[1],18,19,23,6,error));
    assert(bus.release(&owners[0],error) && SPI.active);
    assert(bus.release(&owners[1],error) && SPI.active && SPI.ends == ended);
    bus.snapshotResources(snapshot);
    assert(snapshot.initialized && snapshot.count == 0); // Hardware retention is visible.
    assert(bus.end(error) && !SPI.active && SPI.ends == ended + 1);
    assert(bus.release(&owners[1],error) && SPI.ends == ended + 1);
    for (size_t i=0; i<FastBee::SpiBusInitializer::MAX_CLIENTS; ++i) {
        assert(bus.acquire(&owners[i],18,19,23,static_cast<int8_t>(40+i),error));
    }
    assert(!bus.acquire(&owners[16],18,19,23,60,error));
    bus.snapshotResources(snapshot);
    assert(snapshot.count == 16);
    assert(error.startsWith("SPI_CLIENT_LIMIT"));
    for (size_t i=0; i<FastBee::SpiBusInitializer::MAX_CLIENTS; ++i) assert(bus.release(&owners[i],error));
    assert(SPI.active && bus.end(error) && !SPI.active);
    for (int i=0;i<1000;++i) {
        assert(bus.acquire(&owners[0],18,19,23,5,error));
        assert(bus.release(&owners[0],error) && SPI.active && !bus.isAcquired(&owners[0]));
    }
    assert(bus.end(error));
    // Legacy clients must survive a migrated client's last release.
    assert(bus.begin(18,19,23,5,error));
    assert(bus.acquire(&owners[0],18,19,23,6,error));
    assert(bus.release(&owners[0],error) && SPI.active);
    assert(bus.end(error) && !SPI.active);
    // A missing hardware handle cannot silently recreate another owner's bus.
    assert(bus.acquire(&owners[0],18,19,23,5,error));
    SPI.end();
    assert(!bus.acquire(&owners[1],18,19,23,6,error));
    assert(error.startsWith("SPI_BUS_LOST"));
    assert(bus.isAcquired(&owners[0]) && !bus.isAcquired(&owners[1]));
    assert(bus.release(&owners[0],error));
    assert(bus.acquire(&owners[0],18,19,23,5,error));
    std::atomic<bool> finished{false};
    std::thread releaseThread;
    {
        FastBee::SpiBusOperation operation;
        assert(operation.locked());
        const int previousTakes = spiMutexTakes;
        releaseThread = std::thread([&] {
            String e;
            assert(bus.release(&owners[0],e));
            finished = true;
        });
        while (spiMutexTakes <= previousTakes) std::this_thread::yield();
        assert(!finished && SPI.active);
        assert(bus.isAcquired(&owners[0])); // recursive owner-side query
    }
    releaseThread.join();
    assert(finished && SPI.active && !bus.isAcquired(&owners[0]));
    assert(bus.end(error) && !SPI.active);
    assert(spiMutexCreates - spiMutexDeletes == 1);

    assert(bus.acquireNamed("named\"device",18,19,23,5,error));
    assert(bus.acquireNetwork(&owners[0],false,18,19,23,6,7,8,error));
    bus.snapshotResources(snapshot);
    assert(snapshot.count == 2 && snapshot.networkReserved && !snapshot.networkDedicatedHost);
    assert(std::strcmp(snapshot.leases[0].name,"named\"device") == 0);
    assert(snapshot.leases[1].networkOwner && snapshot.networkPins[3] == 6);
    assert(snapshot.leases[0].consumer.role==FastBee::ResourceConsumerRole::Peripheral);
    assert(snapshot.leases[1].consumer.role==FastBee::ResourceConsumerRole::Ethernet);
    const int reads = SPI.busReads, begins = SPI.begins, ends = SPI.ends;
    for (unsigned i = 0; i < 1000; ++i) bus.snapshotResources(snapshot);
    assert(SPI.busReads == reads && SPI.begins == begins && SPI.ends == ends && spiMutexLastRequest == 0);
    assert(bus.releaseNetwork(&owners[0],error));
    assert(bus.acquireNetwork(&owners[0],true,25,26,27,28,29,30,error));
    bus.snapshotResources(snapshot);
    assert(snapshot.count == 1 && snapshot.networkReserved && snapshot.networkDedicatedHost);
    std::promise<void> entered, release;
    auto ready = release.get_future();
    std::thread held([&] { assert(bus.lock()); entered.set_value(); ready.wait(); bus.unlock(); });
    entered.get_future().wait();
    bus.snapshotResources(snapshot);
    assert(snapshot.status == FastBee::ResourceObservation::Busy && snapshot.count == 0 && spiMutexLastRequest == 0);
    release.set_value(); held.join();
    assert(bus.releaseNamed("named\"device",error));
    assert(bus.releaseNetwork(&owners[0],error) && bus.end(error));
    using Role=FastBee::ResourceConsumerRole;
    char consumer[]="sd-owner";
    assert(bus.acquire(&owners[0],18,19,23,5,error,Role::SdCard,consumer));
    consumer[0]='X';
    assert(!bus.acquire(&owners[0],18,19,23,5,error,Role::SdCard,consumer));
    spiMutexFailTake=true; assert(!bus.release(&owners[0],error)); spiMutexFailTake=false;
    bus.snapshotResources(snapshot);
    assert(snapshot.count==1 && std::strcmp(snapshot.leases[0].consumer.id,"sd-owner")==0);
    assert(snapshot.leases[0].consumer.role==Role::SdCard);
    auto saved=snapshot;
    assert(bus.release(&owners[0],error));
    assert(bus.acquire(&owners[0],18,19,23,5,error,Role::Display));
    bus.snapshotResources(snapshot);
    assert(snapshot.count==1 && !snapshot.leases[0].consumer.id[0]);
    assert(snapshot.leases[0].consumer.role==Role::Display);
    assert(std::strcmp(saved.leases[0].consumer.id,"sd-owner")==0);
    assert(bus.release(&owners[0],error) && bus.end(error));
    const auto oldBegins=SPI.begins;
    assert(!bus.acquire(&owners[0],18,19,23,5,error,Role::Sensor,
                        "01234567890123456789012345678901234567890"));
    assert(SPI.begins==oldBegins);
}
