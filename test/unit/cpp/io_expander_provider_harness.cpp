#include "peripherals/IoExpanderProvider.h"
#include "core/I2CBusManager.h"
#include <freertos/semphr.h>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

static void wide_contract_rejects_without_truncation_or_state_change() {
    auto& bus = FastBee::I2CBusManager::instance();
    auto concrete = std::make_unique<FastBee::IoExpanderProvider>(0x20);
    String error;
    Wire.written = 1;
    assert(concrete->acquire(4, 5, error));
    std::unique_ptr<FastBee::DigitalIoProvider> provider = std::move(concrete);
    assert(provider->channelMask() == 0xff);
    for (unsigned channel = 0; channel < 256; ++channel)
        assert(provider->supportsChannel(static_cast<uint8_t>(channel)) == (channel < 8));
    const unsigned initialWrites = Wire.transmissions;
    for (uint16_t bit = 0x100; bit != 0; bit = static_cast<uint16_t>(bit << 1)) {
        assert(!provider->begin(bit, 0xff, 20, 0));
        assert(!provider->begin(1, bit, 20, 0));
        assert(!provider->begin(1, 0xff, 20, 0, bit));
    }
    assert(!provider->initialized() && Wire.transmissions == initialWrites);
    assert(provider->begin(1, 0xff, 20, 0, 2));
    const unsigned writes = Wire.transmissions, reads = Wire.requests;
    const auto generation = provider->generation(), sampled = provider->sampledAt();
    for (uint16_t bit = 0x100; bit != 0; bit = static_cast<uint16_t>(bit << 1)) {
        assert(!provider->configure(bit, 0xff, 1));
        assert(!provider->configure(1, bit, 1));
        assert(!provider->configure(1, 0xff, 1, bit));
        assert(provider->shutdown(bit) == FastBee::IoExpanderResult::Error);
    }
    for (unsigned channel = 8; channel < 256; ++channel) {
        bool high = true;
        assert(!provider->writeChannel(static_cast<uint8_t>(channel), false, 1));
        assert(!provider->readChannel(static_cast<uint8_t>(channel), true, 1, high) && high);
        assert(!provider->readChannel(static_cast<uint8_t>(channel), false, 1, high) && high);
    }
    assert(!provider->closing() && provider->initialized() && provider->healthy(1));
    assert(provider->generation() == generation && provider->sampledAt() == sampled);
    assert(provider->failures() == 0 && Wire.transmissions == writes && Wire.requests == reads);
    assert(bus.clientCount() == 1);
    assert(provider->writeChannel(0, false, 1));
    assert(provider->shutdown(0xfe) == FastBee::IoExpanderResult::Ok);
    assert(provider->closing() && !provider->initialized() && bus.clientCount() == 0);
    const unsigned closedWrites = Wire.transmissions, closedReads = Wire.requests;
    bool high = true;
    assert(!provider->begin(1, 0xff, 20, 2));
    assert(!provider->configure(1, 0xff, 2));
    assert(!provider->writeChannel(0, true, 2));
    assert(!provider->readChannel(0, true, 2, high) && high);
    assert(!provider->poll(100));
    assert(provider->shutdown(0xffff) == FastBee::IoExpanderResult::Ok);
    assert(Wire.transmissions == closedWrites && Wire.requests == closedReads);
    provider.reset(); // virtual destruction releases the concrete owner only once
    assert(bus.clientCount() == 0);
}

static void shutdown_keeps_busy_lease_then_destroys_without_waiting() {
    auto& bus = FastBee::I2CBusManager::instance();
    using Result = FastBee::IoExpanderResult;
    auto provider = std::make_unique<FastBee::IoExpanderProvider>(0x20);
    String error;
    Wire.written = 1;
    assert(provider->acquire(4, 5, error));
    assert(provider->driver.begin(3, 0, 20, 0));
    assert(provider->driver.writeChannel(0, true, 0));
    const unsigned transmissions = Wire.transmissions;
    std::atomic<bool> locked{false}, release{false};
    auto holdBus = [&] {
        assert(bus.lock(100));
        locked = true;
        while (!release.load()) std::this_thread::yield();
        bus.unlock();
    };
    std::thread holder(holdBus);
    while (!locked.load()) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    assert(provider->shutdown(0xfc) == Result::Busy);
    assert(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count() < 100);
    assert(lastSemaphoreTimeout == 0 && Wire.transmissions == transmissions);
    assert(provider->closing() && !provider->driver.healthy(0));
    release = true;
    holder.join();
    assert(bus.clientCount() == 1 && bus.isAcquired(provider.get()));
    assert(provider->shutdown(0xff) == Result::Ok); // original requested byte is retained
    assert(Wire.lastWrite == 0xfc && Wire.transmissions == transmissions + 1);
    assert(bus.clientCount() == 0);

    locked = release = false;
    std::thread destructorHolder(holdBus);
    while (!locked.load()) std::this_thread::yield();
    lastSemaphoreTimeout = 123;
    const auto destructionStarted = std::chrono::steady_clock::now();
    provider.reset(); // no attempt at release()'s default 1000 ms admission
    assert(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - destructionStarted).count() < 100);
    assert(lastSemaphoreTimeout == 123);
    release = true;
    destructorHolder.join();

    FastBee::IoExpanderProvider failed(0x20);
    assert(failed.acquire(4, 5, error));
    assert(failed.driver.begin(1, 0, 20, 0));
    Wire.txStatus = 4;
    assert(failed.shutdown(0xfe) == Result::Error);
    assert(bus.clientCount() == 0 && !failed.driver.healthy(0));
    const unsigned attempts = Wire.transmissions;
    assert(failed.shutdown(0xff) == Result::Error && Wire.transmissions == attempts);
    Wire.txStatus = 0;
}

int main() {
    auto& bus = FastBee::I2CBusManager::instance();
    using Result = FastBee::IoExpanderResult;
    {
        FastBee::IoExpanderProvider provider(0x20);
        String error;
        assert(provider.acquire(4, 5, error));
        assert(provider.driver.begin(1, 0, 20, 0));
        assert(provider.driver.writeChannel(0, true, 0));
        const unsigned transmissions = Wire.transmissions, requests = Wire.requests;
        std::atomic<bool> locked{false}, release{false};
        std::thread owner([&] {
            assert(bus.lock(100));
            locked = true;
            while (!release.load()) std::this_thread::yield();
            bus.unlock();
        });
        while (!locked.load()) std::this_thread::yield();
        uint8_t value = 0;
        assert(provider.writeByte(0) == Result::Busy && lastSemaphoreTimeout == 0);
        assert(provider.readByte(value) == Result::Busy && lastSemaphoreTimeout == 0);
        assert(!provider.driver.writeChannel(0, false, 1));
        assert(provider.driver.healthy(1) && provider.driver.failures() == 0);
        assert(!provider.driver.poll(100));
        assert(provider.driver.failures() == 0 && provider.driver.outputShadow() == 0xff);
        assert(Wire.transmissions == transmissions && Wire.requests == requests);
        release = true;
        owner.join();
        assert(provider.driver.poll(101));
        assert(Wire.transmissions == transmissions); // no startup replay after contention
        Wire.txStatus = 4;
        assert(provider.writeByte(0) == Result::Error);
        Wire.txStatus = 0;
        Wire.written = 0;
        assert(provider.writeByte(0) == Result::Error);
        Wire.requested = 0;
        assert(provider.readByte(value) == Result::Error);
        Wire.requested = 1;
        assert(provider.readByte(value) == Result::Ok && value == 0xff);
        for(int invalid : {-1,256}) {
            Wire.readOverride=invalid; value=0x5a;
            assert(provider.readByte(value)==Result::Error && value==0x5a);
        }
        Wire.readOverride=-2;
        for(unsigned sample=0;sample<=255;++sample) {
            Wire.sample=static_cast<uint8_t>(sample);
            assert(provider.readByte(value)==Result::Ok && value==sample);
        }
        Wire.sample=0xff;
        // Transport failure reaches the real observation quality path; it is
        // not a fresh all-high sample and must not replay startup outputs.
        const unsigned beforeFailures=provider.driver.failures();
        Wire.readOverride=-1;
        assert(!provider.driver.poll(202));
        assert(provider.driver.failures()==beforeFailures+1);
        assert(!provider.driver.healthy(202));
        Wire.readOverride=-2;
    }
    assert(bus.clientCount() == 0 && Wire.ends == 1);
    shutdown_keeps_busy_lease_then_destroys_without_waiting();
    wide_contract_rejects_without_truncation_or_state_change();
}
