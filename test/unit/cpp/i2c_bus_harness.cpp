#include "core/I2CBusManager.h"
#include <freertos/semphr.h>
#include <cassert>
#include <thread>

int main() {
    auto& manager = FastBee::I2CBusManager::instance();
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
    assert(!manager.recover(&owners[16], error));
    assert(manager.recover(&owners[0], error));
    assert(manager.clientCount() == 16 && Wire.ends == 2 && Wire.begins == 3);
    Wire.beginOk = false;
    assert(!manager.recover(&owners[0], error));
    assert(manager.clientCount() == 16 && manager.wire(&owners[1]) == nullptr);
    assert(!manager.acquire(&owners[0], 4, 6, 100000, error));
    Wire.beginOk = true;
    assert(manager.acquire(&owners[0], 4, 5, 100000, error));
    assert(manager.clientCount() == 16 && manager.wire(&owners[1]) == &Wire);
    for (unsigned i = 0; i < 16; ++i) manager.release(&owners[i]);
    assert(manager.clientCount() == 0 && Wire.ends == 4);
    assert(manager.sda() == -1 && manager.scl() == -1 && manager.frequency() == 0);
}
