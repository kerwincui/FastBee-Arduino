#include <Arduino.h>
constexpr int SDA = 21;
constexpr int SCL = 22;
#include "peripherals/drivers/I2CSensorBus.h"
#include "peripherals/IoExpanderProvider.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK line %d: %s\n", __LINE__, #x); std::exit(86); } } while (false)
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string mode = argv[1];
    auto& manager = FastBee::I2CBusManager::instance();
    FastBee::I2CSensorBus first, second;
    int a = 0, b = 0;
    JsonDocument one, two;
    if (mode == "duplicate" || mode == "release") {
        CHECK(first.open(&a, "INA226", "{}", one));
        CHECK(!second.open(&b, "INA226", "{}", two));
        CHECK(manager.clientCount() == 1);
        CHECK(first.writeRegister16(0, 0x1234));
        if (mode == "release") {
            CHECK(first.close());
            CHECK(second.open(&b, "INA226", "{}", two));
        }
    } else if (mode == "alias") {
        CHECK(first.open(&a, "SHT30", "{}", one));
        CHECK(!second.open(&b, "SHT31", "{}", two));
        CHECK(manager.clientCount() == 1);
    } else if (mode == "different") {
        CHECK(first.open(&a, "INA226", "{\"addr\":64}", one));
        CHECK(second.open(&b, "INA226", "{\"addr\":65}", two));
        CHECK(manager.clientCount() == 2);
    } else if (mode == "concurrent") {
        std::atomic<unsigned> ready{0}, winners{0};
        std::atomic<bool> go{false};
        auto race = [&](FastBee::I2CSensorBus& bus, int& owner, JsonDocument& doc) {
            ++ready; while (!go.load()) std::this_thread::yield();
            if (bus.open(&owner, "INA226", "{}", doc)) ++winners;
        };
        std::thread x(race, std::ref(first), std::ref(a), std::ref(one));
        std::thread y(race, std::ref(second), std::ref(b), std::ref(two));
        while (ready.load() != 2) std::this_thread::yield();
        go = true; x.join(); y.join();
        CHECK(winners == 1 && manager.clientCount() == 1);
    } else if (mode == "claim_rules" || mode == "retire" || mode == "recovery") {
        String error;
        CHECK(!manager.claimAddress(&a, 0x48, error));
        CHECK(manager.acquire(&a, 21, 22, 100000, error));
        for (uint8_t address : {0, 7, 0x78, 0x7f}) CHECK(!manager.claimAddress(&a, address, error));
        CHECK(manager.claimAddress(&a, 0x48, error));
        CHECK(manager.claimAddress(&a, 0x48, error));
        CHECK(!manager.claimAddress(&a, 0x49, error));
        CHECK(manager.acquire(&b, 21, 22, 100000, error));
        CHECK(!manager.claimAddress(&b, 0x48, error));
        if (mode == "retire") {
            manager.deferRelease(&a);
            CHECK(manager.claimAddress(&b, 0x48, error));
            CHECK(!manager.isAcquired(&a));
        } else {
            if (mode == "recovery") CHECK(manager.recover(&a, error));
            CHECK(!manager.claimAddress(&b, 0x48, error));
            CHECK(manager.claimAddress(&b, 0x49, error));
        }
        CHECK(manager.release(&a) && manager.release(&b));
    } else if (mode == "provider_collision") {
        String error;
        CHECK(manager.acquire(&a, 21, 22, 100000, error));
        CHECK(manager.claimAddress(&a, 0x20, error));
        {
            FastBee::IoExpanderProvider provider(0x20);
            CHECK(!provider.acquire(21, 22, error));
            CHECK(Wire.transmissions == 0 && manager.clientCount() == 1);
            CHECK(manager.ownsAddress(&a, 0x20));
            CHECK(manager.release(&a));
            CHECK(provider.acquire(21, 22, error));
            CHECK(manager.acquire(&b, 21, 22, 100000, error));
            CHECK(!manager.claimAddress(&b, 0x20, error));
            CHECK(manager.release(&b));
        }
        CHECK(manager.clientCount() == 0);
    } else if (mode == "capacity") {
        int owners[FastBee::I2CBusManager::MAX_CLIENTS] = {};
        String error;
        for (size_t i = 0; i < FastBee::I2CBusManager::MAX_CLIENTS; ++i) {
            CHECK(manager.acquire(&owners[i], 21, 22, 100000, error));
            CHECK(manager.claimAddress(&owners[i], static_cast<uint8_t>(8 + i), error));
        }
        CHECK(!manager.acquire(&a, 21, 22, 100000, error));
        for (auto& owner : owners) manager.deferRelease(&owner);
        CHECK(manager.acquire(&a, 21, 22, 100000, error));
        CHECK(manager.claimAddress(&a, 8, error) && manager.clientCount() == 1);
        CHECK(manager.release(&a));
    } else { CHECK(false); }
    CHECK(first.close() && second.close());
    CHECK(manager.clientCount() == 0);
    std::printf("ownership %s passed\n", argv[1]);
}
