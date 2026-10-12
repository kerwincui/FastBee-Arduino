#include "core/SpiBusInitializer.h"
#include <SPI.h>
#include <freertos/semphr.h>
#include <cassert>
#include <thread>
#include <vector>

int main() {
    auto& initializer = FastBee::SpiBusInitializer::instance();
    String error;
    spiMutexFailAllocation = true;
    assert(!initializer.begin(18, 19, 23, 5, error));
    assert(error.startsWith("SPI_MUTEX_UNAVAILABLE") && SPI.begins == 0);
    assert(!initializer.end(error) && SPI.ends == 0);
    spiMutexFailAllocation = false;
    spiMutexRace = true;
    std::atomic<int> successes{0};
    std::thread a([&] { String e; if (initializer.begin(18, 19, 23, 5, e)) ++successes; });
    std::thread b([&] { String e; if (initializer.begin(18, 19, 23, 6, e)) ++successes; });
    a.join(); b.join();
    spiMutexRace = false;
    assert(successes == 2 && SPI.begins == 1);
    assert(spiMutexCreates == 2 && spiMutexDeletes == 1 && spiMutexLastWait == 1000);
    assert(initializer.begin(18, -1, 23, 7, error)); // output-only client needs no MISO
    assert(!initializer.begin(18, 22, 23, 7, error));
    assert(error.startsWith("SPI_PINS_CONFLICT") && SPI.begins == 1 && SPI.ends == 0);
    assert(!initializer.begin(21, 19, 23, 7, error));
    assert(!initializer.begin(18, 19, 22, 7, error));
    for (const auto& pins : std::vector<std::vector<int8_t>>{
            {-1,19,23,5}, {18,-2,23,5}, {18,19,-1,5}, {18,19,23,-1},
            {18,18,23,5}, {18,23,23,5}, {18,19,18,5}, {18,19,23,19}}) {
        assert(!initializer.begin(pins[0],pins[1],pins[2],pins[3],error));
        assert(error.startsWith("SPI_PINS_INVALID"));
    }
    spiMutexFailTake = true;
    assert(!initializer.begin(18, 19, 23, 5, error));
    assert(error.startsWith("SPI_BUSY"));
    assert(!initializer.end(error) && SPI.ends == 0 && SPI.active);
    spiMutexFailTake = false;
    assert(initializer.end(error) && SPI.ends == 1);
    assert(initializer.end(error) && SPI.ends == 1);
    assert(initializer.begin(18, -1, 23, 5, error));
    assert(!initializer.begin(18, 19, 23, 6, error)); // never silently omit input
    assert(error.startsWith("SPI_PINS_CONFLICT") && SPI.lastMiso == -1);
    assert(initializer.end(error));
    const int beforeFailure = SPI.ends;
    SPI.failAfterAllocation = true;
    assert(!initializer.begin(18, 19, 23, 5, error));
    assert(error.startsWith("SPI_INIT_FAILED") && !SPI.active && SPI.ends == beforeFailure + 1);
    assert(!initializer.begin(18, 19, 23, 5, error)); // sticky failed handle was cleared
    SPI.failAfterAllocation = false;
    SPI.failBeforeAllocation = true;
    assert(!initializer.begin(18, 19, 23, 5, error) && !SPI.active);
    SPI.failBeforeAllocation = false;
    assert(initializer.begin(18, 19, 23, 5, error));
    assert(initializer.end(error));
    SPI.begin(12, 13, 14, 15); // foreign bus must not be adopted or destroyed
    const int unmanagedEnds = SPI.ends;
    assert(!initializer.begin(18, 19, 23, 5, error));
    assert(error.startsWith("SPI_BUS_UNMANAGED"));
    assert(!initializer.end(error) && SPI.active && SPI.ends == unmanagedEnds);
    SPI.end();
    for (int i = 0; i < 1000; ++i) {
        assert(initializer.begin(18, 19, 23, 5, error));
        assert(initializer.begin(18, 19, 23, 6, error));
        assert(initializer.end(error));
    }
    assert(spiMutexCreates - spiMutexDeletes == 1 && !SPI.active);
    // Concurrent begin/end is bounded and serialized; this does not test I/O leases.
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        for (int j = 0; j < 100; ++j) {
            String e;
            assert(initializer.begin(18, 19, 23, 5, e));
            assert(initializer.end(e));
        }
    });
    for (auto& worker : workers) worker.join();
    assert(!SPI.active);
}
