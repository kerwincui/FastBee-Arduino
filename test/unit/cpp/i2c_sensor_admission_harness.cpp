#include <Arduino.h>
constexpr int SDA = 21;
constexpr int SCL = 22;
#include "peripherals/drivers/I2CSensorBus.h"
#include <freertos/semphr.h>
#include <cassert>
#include <chrono>
#include <thread>
#include <array>
#include <memory>

int main() {
    auto& manager = FastBee::I2CBusManager::instance();
    FastBee::I2CSensorBus sensor;
    int originalOwner = 0, replacementOwner = 0;
    JsonDocument initial;
    assert(sensor.open(&originalOwner, "INA226", "{}", initial));
    assert(manager.clientCount() == 1 && manager.isAcquired(&originalOwner));
    TwoWire* originalWire = sensor.wire();
    const unsigned begins = Wire.begins, ends = Wire.ends;

    std::atomic<bool> locked{false}, release{false};
    std::thread holder([&] {
        assert(manager.lock(100));
        locked = true;
        while (!release.load()) std::this_thread::yield();
        manager.unlock();
    });
    while (!locked.load()) std::this_thread::yield();

    JsonDocument replacement;
    const auto start = std::chrono::steady_clock::now();
    assert(!sensor.open(&replacementOwner, "INA226", "{}", replacement));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    assert(elapsed < 500); // Old code first waited release()'s 1000 ms timeout.
    assert(lastSemaphoreTimeout == pdMS_TO_TICKS(100));
    assert(std::strcmp(sensor.error(), "SENSOR_I2C_BUSY") == 0);
    assert(sensor.wire() == originalWire);
    assert(Wire.begins == begins && Wire.ends == ends);
    const auto closeStart = std::chrono::steady_clock::now();
    assert(!sensor.close());
    assert(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - closeStart).count() < 500);
    assert(sensor.wire() == originalWire);
    assert(!manager.release(&originalOwner, 5));

    release = true;
    holder.join();
    assert(manager.clientCount() == 1 && manager.isAcquired(&originalOwner));
    assert(!manager.isAcquired(&replacementOwner));
    assert(sensor.open(&replacementOwner, "INA226", "{}", replacement));
    assert(manager.clientCount() == 1 && manager.isAcquired(&replacementOwner));
    assert(!manager.isAcquired(&originalOwner));
    assert(Wire.begins == begins + 1 && Wire.ends == ends + 1);
    assert(sensor.close());
    assert(manager.clientCount() == 0 && Wire.ends == ends + 2);

    // All 16 leases can be transferred by destructors while another task owns
    // the bus. No object address is dereferenced by deferred cleanup.
    std::array<int, FastBee::I2CBusManager::MAX_CLIENTS> owners{};
    std::array<std::unique_ptr<FastBee::I2CSensorBus>, FastBee::I2CBusManager::MAX_CLIENTS> sensors;
    for (size_t i = 0; i < sensors.size(); ++i) {
        sensors[i].reset(new FastBee::I2CSensorBus);
        JsonDocument params;
        // The fixture fills all client slots with distinct physical addresses;
        // duplicate-address admission is now explicitly forbidden.
        const std::string settings = "{\"addr\":" + std::to_string(0x40 + i) + "}";
        assert(sensors[i]->open(&owners[i], "INA226", settings.c_str(), params));
    }
    assert(manager.clientCount() == sensors.size());
    locked = false; release = false;
    std::thread destructorHolder([&] {
        assert(manager.lock(100));
        locked = true;
        while (!release.load()) std::this_thread::yield();
        manager.unlock();
    });
    while (!locked.load()) std::this_thread::yield();
    for (auto& item : sensors) {
        const auto destroyStart = std::chrono::steady_clock::now();
        item.reset();
        assert(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - destroyStart).count() < 500);
    }
    release = true;
    destructorHolder.join();
    assert(manager.clientCount() == 0);
    for (auto& owner : owners) assert(!manager.isAcquired(&owner));

    // Reusing an old owner address on a different bus must not resurrect its
    // retired lease or conflict with the old physical configuration.
    JsonDocument reused;
    assert(sensor.open(&owners[0], "INA226", "{\"sda\":25,\"scl\":26}", reused));
    assert(manager.clientCount() == 1 && manager.sda() == 25 && manager.scl() == 26);
    assert(sensor.close() && manager.clientCount() == 0);
}
