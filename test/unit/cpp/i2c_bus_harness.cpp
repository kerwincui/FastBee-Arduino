#include "core/I2CBusManager.h"
#include <freertos/semphr.h>
#include <cassert>
#include <thread>
#include <future>
#include <cstring>

int main() {
    auto& manager = FastBee::I2CBusManager::instance();
    FastBee::I2CResourceSnapshot snapshot;
    manager.snapshotResources(snapshot);
    assert(snapshot.status == FastBee::ResourceObservation::Observed && snapshot.count == 0);
    assert(!snapshot.initialized && semaphoreCreates == 0 && Wire.begins == 0 && Wire.ends == 0);
    int owners[17] = {};
    bool first = false, second = false;
    std::thread a([&] { String error; first = manager.acquire(&owners[0], 4, 5, 100000, error); });
    std::thread b([&] { String error; second = manager.acquire(&owners[1], 4, 5, 100000, error); });
    a.join(); b.join();
    assert(first && second);
    assert(semaphoreCreates == 2 && semaphoreDeletes == 1);
    assert(manager.clientCount() == 2 && Wire.begins == 1);
    assert(Wire.clock == 100000 && Wire.timeout == 20);
    String error;
    assert(manager.acquire(&owners[0], 4, 5, 100000, error));
    assert(manager.clientCount() == 2);
    assert(!manager.acquire(&owners[2], 4, 6, 100000, error));
    assert(!manager.acquire(&owners[2], 4, 5, 400000, error));
    assert(manager.wire(&owners[0]) == &Wire);
    assert(manager.wire(&owners[2]) == nullptr);
    {
        FastBee::I2CBusLock outer;
        FastBee::I2CBusLock nested;
        assert(outer.locked() && nested.locked());
        assert(manager.wire(&owners[1]) == &Wire);
    }
    manager.release(&owners[0]);
    assert(Wire.ends == 0 && manager.clientCount() == 1);
    manager.release(&owners[1]);
    assert(Wire.ends == 1 && manager.clientCount() == 0);
    for (unsigned i = 0; i < 16; ++i) assert(manager.acquire(&owners[i], 4, 5, 100000, error));
    assert(!manager.acquire(&owners[16], 4, 5, 100000, error));
    assert(manager.clientCount() == 16 && Wire.begins == 2);
    manager.snapshotResources(snapshot);
    assert(snapshot.count == 16 && snapshot.initialized && semaphoreLastWait == 0);
    assert(!manager.recover(&owners[16], error));
    assert(manager.recover(&owners[0], error));
    assert(manager.clientCount() == 16 && Wire.ends == 2 && Wire.begins == 3);
    Wire.beginOk = false;
    assert(!manager.recover(&owners[0], error));
    assert(manager.clientCount() == 16 && manager.wire(&owners[1]) == nullptr);
    manager.snapshotResources(snapshot);
    assert(snapshot.count == 16 && !snapshot.initialized); // Failed recovery retains leases.
    assert(!manager.acquire(&owners[0], 4, 6, 100000, error));
    Wire.beginOk = true;
    assert(manager.acquire(&owners[0], 4, 5, 100000, error));
    assert(manager.clientCount() == 16 && manager.wire(&owners[1]) == &Wire);
    for (unsigned i = 0; i < 16; ++i) manager.release(&owners[i]);
    assert(manager.clientCount() == 0 && Wire.ends == 4);
    assert(manager.sda() == -1 && manager.scl() == -1 && manager.frequency() == 0);

    char consumer[] = "retained-sensor";
    assert(manager.acquire(&owners[0], 4, 5, 100000, error, FastBee::ResourceConsumerRole::Sensor, consumer));
    consumer[0] = 'X'; // The bus must retain its own copy through owner destruction.
    assert(!manager.acquire(&owners[0], 4, 5, 100000, error, FastBee::ResourceConsumerRole::Sensor, consumer));
    assert(manager.claimAddress(&owners[0], 0x20, error));
    manager.deferRelease(&owners[0]);
    const unsigned begins = Wire.begins, ends = Wire.ends;
    for (unsigned i = 0; i < 1000; ++i) {
        manager.snapshotResources(snapshot);
        assert(snapshot.count == 1 && snapshot.leases[0].retiring);
        assert(snapshot.leases[0].address == 0x20 && snapshot.sda == 4 && snapshot.scl == 5);
        assert(snapshot.frequency == 100000 && semaphoreLastWait == 0);
        assert(std::strcmp(snapshot.leases[0].consumer.id, "retained-sensor") == 0);
        assert(snapshot.leases[0].consumer.role == FastBee::ResourceConsumerRole::Sensor);
    }
    assert(Wire.begins == begins && Wire.ends == ends); // Observation never drains retirement.
    assert(manager.clientCount() == 0 && Wire.ends == ends + 1); // A normal operation does.
    assert(manager.acquire(&owners[1], 4, 5, 100000, error));
    std::promise<void> entered, release;
    auto ready = release.get_future();
    std::thread held([&] {
        assert(manager.lock()); entered.set_value(); ready.wait(); manager.unlock();
    });
    entered.get_future().wait();
    snapshot.count = 16;
    manager.snapshotResources(snapshot);
    assert(snapshot.status == FastBee::ResourceObservation::Busy && snapshot.count == 0);
    assert(semaphoreLastWait == 0); // No wall-clock timing assumption.
    release.set_value(); held.join();
    manager.snapshotResources(snapshot);
    assert(snapshot.status == FastBee::ResourceObservation::Observed && snapshot.count == 1);
    assert(manager.release(&owners[1]));
    const auto priorBegins = Wire.begins;
    assert(!manager.acquire(&owners[0], 4, 5, 100000, error, FastBee::ResourceConsumerRole::Sensor,
                            "01234567890123456789012345678901234567890"));
    assert(Wire.begins == priorBegins);
    assert(manager.acquire(&owners[0], 4, 5, 100000, error, FastBee::ResourceConsumerRole::Peripheral, "new-owner"));
    Wire.beginOk=false;
    assert(!manager.recover(&owners[0], error));
    manager.snapshotResources(snapshot);
    assert(snapshot.count==1 && !snapshot.initialized && std::strcmp(snapshot.leases[0].consumer.id,"new-owner")==0);
    assert(snapshot.leases[0].consumer.role==FastBee::ResourceConsumerRole::Peripheral);
    assert(manager.release(&owners[0]));
    manager.snapshotResources(snapshot); assert(snapshot.count==0);
}
