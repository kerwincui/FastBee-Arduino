#include <Arduino.h>
constexpr int SDA = 21;
constexpr int SCL = 22;
#include "peripherals/drivers/I2CSensorBus.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <array>
#include <cstdio>
#include <cstdlib>
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #condition); std::exit(86); } } while (false)

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string mode = argv[1];
    auto& manager = FastBee::I2CBusManager::instance();
    FastBee::I2CSensorBus bus;
    int owner = 0, peer = 0;
    JsonDocument doc;
    CHECK(bus.open(&owner, "INA226", "{}", doc));
    uint8_t bytes[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    uint16_t result = 0xbeef;
    if (mode == "pointer_short") {
        Wire.writeLimit = 0;
        CHECK(!bus.readRegister16(0x02, result));
        CHECK(result == 0xbeef && Wire.requests == 0);
        CHECK(Wire.stops == std::vector<bool>{true});
        CHECK(std::strcmp(bus.error(), "SENSOR_I2C_WRITE_FAILED") == 0);
    } else if (mode == "pointer_nack") {
        for (uint8_t status = 1; status <= 5; ++status) {
            Wire.txStatus = status;
            CHECK(!bus.readRegister16(0x02, result));
            CHECK(result == 0xbeef && Wire.requests == 0);
        }
    } else if (mode == "read_missing") {
        Wire.input = {0x12};
        CHECK(!bus.readRegister16(0x02, result));
        CHECK(result == 0xbeef);
        CHECK(std::strcmp(bus.error(), "SENSOR_I2C_SHORT_READ") == 0);
    } else if (mode == "read_negative") {
        for (int index = 0; index < 2; ++index) {
            Wire.failReadAt = index;
            CHECK(!bus.readRegister16(0x02, result));
            CHECK(result == 0xbeef);
        }
    } else if (mode == "short_count") {
        for (size_t count : {size_t(0), size_t(1), size_t(3)}) {
            Wire.returnedCount = count;
            CHECK(!bus.readRegister16(0x02, result));
            CHECK(result == 0xbeef && Wire.available() == 0);
        }
    } else if (mode == "bounded_drain") {
        Wire.returnedCount = 0; Wire.stuckAvailable = true;
        CHECK(!bus.readRegister16(0x02, result));
        CHECK(Wire.reads <= 255 && Wire.reads > 0 && result == 0xbeef);
    } else if (mode == "drain_negative") {
        Wire.returnedCount = 0; Wire.failReadAt = 0;
        CHECK(!bus.readRegister16(0x02, result));
        CHECK(Wire.reads == 1);
    } else if (mode == "null_read" || mode == "zero_read" || mode == "oversized_read") {
        uint8_t* target = mode == "null_read" ? nullptr : bytes;
        const size_t length = mode == "zero_read" ? 0 : (mode == "oversized_read" ? 256 : 2);
        CHECK(!bus.read(target, length));
        CHECK(!bus.readRegister(0x02, target, length));
        CHECK(Wire.requests == 0 && Wire.transmissions == 0);
        CHECK(bytes[0] == 0xaa && bytes[1] == 0xbb);
        CHECK(std::strcmp(bus.error(), "SENSOR_I2C_READ_ARGUMENT_INVALID") == 0);
    } else if (mode == "null_write") {
        CHECK(!bus.write(nullptr, 1));
        CHECK(Wire.transmissions == 0 && Wire.requests == 0);
        CHECK(std::strcmp(bus.error(), "SENSOR_I2C_WRITE_ARGUMENT_INVALID") == 0);
    } else if (mode == "probe") {
        CHECK(bus.write(nullptr, 0));
        CHECK(Wire.transmissions == 1 && Wire.written.empty());
        CHECK(Wire.stops == std::vector<bool>{true});
        CHECK(bus.error()[0] == '\0');
    } else if (mode == "write_short") {
        for (size_t count = 0; count < 3; ++count) {
            Wire.writeLimit = count;
            CHECK(!bus.writeRegister16(0x05, 0x1234));
            CHECK(Wire.stops.back());
            CHECK(std::strcmp(bus.error(), "SENSOR_I2C_WRITE_FAILED") == 0);
        }
        CHECK(Wire.transmissions == 3 && Wire.stops.size() == 3);
    } else if (mode == "success") {
        CHECK(bus.writeRegister16(0x05, 0x1234));
        CHECK(Wire.written == std::vector<uint8_t>({5, 0x12, 0x34}));
        CHECK(bus.readRegister16(0x02, result) && result == 0x1234);
        CHECK(Wire.written == std::vector<uint8_t>{2});
        CHECK(Wire.stops == std::vector<bool>({true, false}));
        CHECK(Wire.address == 0x40 && Wire.requestedLength == 2);
    } else if (mode == "length_boundary") {
        std::array<uint8_t, 257> output{}; output.fill(0xcc);
        Wire.input.resize(255);
        for (unsigned index = 0; index < 255; ++index) Wire.input[index] = index;
        Wire.returnedCount = 255;
        CHECK(bus.read(output.data() + 1, 255));
        CHECK(Wire.requestedLength == 255 && output.front() == 0xcc && output.back() == 0xcc);
        for (unsigned index = 0; index < 255; ++index) CHECK(output[index + 1] == index);
    } else if (mode == "all_codes") {
        for (unsigned code = 0; code < 65536; ++code) {
            Wire.input = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)};
            CHECK(bus.readRegister16(0x02, result) && result == code);
        }
    } else if (mode == "recovery") {
        Wire.txStatus = 2; CHECK(!bus.readRegister16(0x02, result));
        Wire.txStatus = 0; CHECK(bus.readRegister16(0x02, result));
        CHECK(result == 0x1234 && bus.error()[0] == '\0');
        CHECK(manager.clientCount() == 1 && manager.isAcquired(&owner));
    } else if (mode == "busy") {
        std::atomic<bool> locked{false}, release{false};
        std::thread holder([&] {
            CHECK(manager.lock(100)); locked = true;
            while (!release.load()) std::this_thread::yield();
            manager.unlock();
        });
        while (!locked.load()) std::this_thread::yield();
        CHECK(!bus.readRegister16(0x02, result));
        CHECK(!bus.read(bytes, 2)); CHECK(!bus.write(bytes, 2));
        CHECK(std::strcmp(bus.error(), "SENSOR_I2C_BUSY") == 0);
        CHECK(Wire.transmissions == 0 && Wire.requests == 0 && result == 0xbeef);
        release = true; holder.join();
        CHECK(bus.readRegister16(0x02, result) && result == 0x1234);
    } else if (mode == "closed") {
        CHECK(bus.close());
        CHECK(!bus.readRegister16(0x02, result));
        CHECK(!bus.read(bytes, 2)); CHECK(!bus.write(bytes, 2));
        CHECK(std::strcmp(bus.error(), "SENSOR_I2C_NOT_READY") == 0);
        CHECK(result == 0xbeef && Wire.requests == 0 && Wire.transmissions == 0);
    } else if (mode == "peer_survives") {
        String error; CHECK(manager.acquire(&peer, 21, 22, 100000, error));
        Wire.writeLimit = 0; CHECK(!bus.readRegister16(2, result));
        CHECK(bus.close());
        CHECK(manager.clientCount() == 1 && manager.isAcquired(&peer) && Wire.ends == 0);
        CHECK(manager.release(&peer) && Wire.ends == 1);
    } else CHECK(false);
    CHECK(bus.close() && manager.clientCount() == 0);
    std::printf("transport %s passed\n", mode.c_str());
}
