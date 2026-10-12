#include "core/SpiBusInitializer.h"
#include <SPI.h>
#include <freertos/semphr.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <new>
#include <thread>

static std::atomic<unsigned> allocations{0};
static std::atomic<bool> failAllocation{false};
static void* allocateStorage(std::size_t size) noexcept {
    ++allocations;
    if (failAllocation) return nullptr;
    try { return ::operator new(size); }
    catch (const std::bad_alloc&) { return nullptr; }
}
#ifdef SPI_TEST_INTERNAL_HEAP
#include <esp_heap_caps.h>
void* heap_caps_malloc(std::size_t size, unsigned caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return allocateStorage(size);
}
void heap_caps_free(void* memory) { ::operator delete(memory); }
#else
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return allocateStorage(size);
}
#endif

int main() {
    using FastBee::SpiBusInitializer;
    auto& bus = SpiBusInitializer::instance();
    int owners[SpiBusInitializer::MAX_CLIENTS + 1]{};
    String error;
    FastBee::SpiResourceSnapshot snapshot;
    bus.snapshotResources(snapshot);
    assert(!bus.hasMutex() && snapshot.count == 0 && allocations == 0);
    assert(!bus.isAcquired(&owners[0]));
    assert(bus.release(&owners[0], error));
    assert(!bus.acquire(nullptr,18,19,23,5,error));
    assert(!bus.acquire(&owners[0],18,19,18,5,error));
    assert(error.startsWith("SPI_PINS_INVALID") && allocations == 0);
    assert(!bus.acquireNamed("",18,19,23,5,error));
    assert(!bus.acquireNamed("named",18,19,23,19,error));
    assert(!bus.checkNamed("missing",18,19,23,5,error));
    assert(bus.releaseNamed("missing",error));
    assert(!bus.isChipSelectAcquired(5));
    assert(allocations == 0 && SPI.begins == 0);

    // A dedicated W5500 host needs only its inline reservation, not 16 slots.
    assert(bus.acquireNetwork(&owners[0],true,25,26,27,28,29,30,error));
    bus.snapshotResources(snapshot);
    assert(snapshot.networkReserved && snapshot.networkDedicatedHost && snapshot.count == 0);
    assert(!bus.acquireNamed("conflict",25,26,27,28,error));
    assert(error.startsWith("SPI_NETWORK_PIN_CONFLICT") && allocations == 0);
    assert(bus.releaseNetwork(&owners[0],error) && bus.end(error));

    // An unmanaged Arduino bus cannot cause table allocation or be destroyed.
    SPI.begin(12,13,14,15);
    const int foreignEnds = SPI.ends;
    assert(!bus.acquire(&owners[0],18,19,23,5,error));
    assert(error.startsWith("SPI_BUS_UNMANAGED") && allocations == 0);
    assert(!bus.end(error) && SPI.ends == foreignEnds);
    SPI.end();

    // All three managed entry points report OOM before any SDK begin.
    failAllocation = true;
    const int beforeOomBegins = SPI.begins;
    assert(!bus.acquire(&owners[0],18,19,23,5,error));
    assert(error.startsWith("SPI_CLIENT_STORAGE_UNAVAILABLE"));
    assert(!bus.acquireNamed("named",18,19,23,5,error));
    assert(error.startsWith("SPI_CLIENT_STORAGE_UNAVAILABLE"));
    assert(!bus.acquireNetwork(&owners[0],false,18,19,23,5,6,7,error));
    assert(error.startsWith("SPI_CLIENT_STORAGE_UNAVAILABLE"));
    assert(allocations == 3 && SPI.begins == beforeOomBegins);
    bus.snapshotResources(snapshot);
    assert(!snapshot.networkReserved && snapshot.count == 0 && !snapshot.initialized);

    // Legacy begin does not need a table. A failed managed join retains it.
    assert(bus.begin(18,19,23,5,error));
    const int legacyEnds = SPI.ends;
    assert(!bus.acquire(&owners[0],18,19,23,6,error));
    assert(SPI.active && SPI.ends == legacyEnds);
    assert(bus.end(error));

    // SDK failure publishes no lease; an allocated empty table can be retired.
    failAllocation = false;
    SPI.failAfterAllocation = true;
    assert(!bus.acquireNamed("named",18,19,23,5,error));
    assert(error.startsWith("SPI_INIT_FAILED") && !SPI.active);
    const unsigned failedSdkAllocations = allocations;
    failAllocation = true;
    assert(!bus.acquire(&owners[0],18,19,23,6,error));
    assert(error.startsWith("SPI_INIT_FAILED") && allocations == failedSdkAllocations);
    assert(!bus.isAcquired(&owners[0]));
    assert(bus.end(error));
    SPI.failAfterAllocation = false;
    assert(!bus.acquireNamed("named",18,19,23,5,error));
    assert(error.startsWith("SPI_CLIENT_STORAGE_UNAVAILABLE") && allocations == failedSdkAllocations + 1);

    // Concurrent first users allocate once; the same stable named token remains
    // valid through later joins, failed release/end, and a lost hardware handle.
    failAllocation = false;
    const unsigned beforeConcurrent = allocations;
    std::thread a([&] { String e; assert(bus.acquireNamed("named",18,19,23,5,e)); });
    std::thread b([&] { String e; assert(bus.acquire(&owners[0],18,19,23,6,e)); });
    a.join(); b.join();
    assert(allocations == beforeConcurrent + 1);
    failAllocation = true;
    assert(bus.acquireNamed("named",18,19,23,5,error));
    assert(bus.checkNamed("named",18,19,23,5,error));
    assert(!bus.end(error) && error.startsWith("SPI_IN_USE"));
    spiMutexFailTake = true;
    assert(!bus.releaseNamed("named",error));
    spiMutexFailTake = false;
    assert(bus.checkNamed("named",18,19,23,5,error));
    SPI.end();
    assert(!bus.acquire(&owners[1],18,19,23,7,error));
    assert(error.startsWith("SPI_BUS_LOST"));
    bus.snapshotResources(snapshot);
    assert(snapshot.count == 2 && bus.isAcquired(&owners[0]));
    assert(bus.releaseNamed("named",error));
    assert(bus.release(&owners[0],error) && bus.end(error));

    // Dedicated ownership blocks table retirement even after all global users
    // release. Explicit end after releasing that reservation permits recovery.
    failAllocation = false;
    assert(bus.acquireNamed("named",18,19,23,5,error));
    assert(bus.acquireNetwork(&owners[0],true,25,26,27,28,29,30,error));
    assert(bus.releaseNamed("named",error) && bus.end(error));
    const unsigned retained = allocations;
    failAllocation = true;
    assert(bus.acquireNamed("named",18,19,23,5,error));
    assert(allocations == retained);
    assert(bus.releaseNamed("named",error) && bus.releaseNetwork(&owners[0],error));
    assert(bus.end(error));
    assert(!bus.acquireNamed("named",18,19,23,5,error));
    assert(error.startsWith("SPI_CLIENT_STORAGE_UNAVAILABLE") && allocations == retained + 1);

    // Capacity and all slot identities are unchanged after repeated retirement.
    failAllocation = false;
    for (size_t i=0;i<SpiBusInitializer::MAX_CLIENTS;++i)
        assert(bus.acquire(&owners[i],18,19,23,static_cast<int8_t>(40+i),error));
    assert(!bus.acquire(&owners[16],18,19,23,60,error));
    assert(error.startsWith("SPI_CLIENT_LIMIT"));
    bus.snapshotResources(snapshot);
    assert(snapshot.count == SpiBusInitializer::MAX_CLIENTS);
    for (size_t i=0;i<SpiBusInitializer::MAX_CLIENTS;++i)
        assert(bus.release(&owners[i],error));
    assert(bus.end(error));
    const unsigned afterEnd = allocations;
    for (unsigned i=0;i<50;++i) {
        bus.snapshotResources(snapshot);
        assert(snapshot.count == 0 && !snapshot.networkReserved);
        assert(!bus.isAcquired(&owners[0]) && !bus.isChipSelectAcquired(5));
    }
    assert(allocations == afterEnd);
}
