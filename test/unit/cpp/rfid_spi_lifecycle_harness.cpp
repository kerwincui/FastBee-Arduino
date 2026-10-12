#include "peripherals/drivers/RFIDDriver.h"
#include "core/SpiBusInitializer.h"
#include "core/PeriphExecManager.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"
#include "core/FeatureInstanceRegistry.h"
#include <MFRC522.h>
#include <cassert>
#include <thread>

int main() {
    auto& instances = FastBee::FeatureInstanceRegistry::instance();
    using InstanceState = FastBee::FeatureInstanceState;
    assert(!instances.observe("rfid").observed());
    auto& reader = RFIDDriver::getInstance();
    assert(instances.observe("rfid").state == InstanceState::NotInitialized && spiMutexCreates == 0);
    auto& bus = FastBee::SpiBusInitializer::instance();
    String error, uid, cardType;
    auto begin = [&] { return reader.begin(5,27,18,23,19); };
    spiMutexFailAllocation = true;
    assert(!begin() && MFRC522::live == 0 && SPI.begins == 0);
    spiMutexFailAllocation = false;
    FastBee::RuntimeFeatureManager::enabled = false;
    assert(!begin() && SPI.begins == 0);
    assert(reader.getLastError().startsWith("RFID_RUNTIME_DISABLED"));
    FastBee::RuntimeFeatureManager::enabled = true;
    FastBee::FeatureBudgetRegistry::denied = true;
    assert(!begin() && SPI.begins == 0);
    assert(reader.getLastError().startsWith("RFID_BUDGET_DENIED"));
    FastBee::FeatureBudgetRegistry::denied = false;
    MFRC522::failAllocation = true;
    assert(!begin() && reader.getLastError().startsWith("RFID_ALLOCATION_FAILED"));
    MFRC522::failAllocation = false;
    assert(!reader.begin(5,27,18,255,19));
    assert(SPI.begins == 0 && MFRC522::live == 0);
    SPI.failAfterAllocation = true;
    assert(!begin() && !SPI.active && !bus.isAcquired(&reader));
    assert(MFRC522::initialized == 0 && MFRC522::live == 0);
    SPI.failAfterAllocation = false;
    for (byte invalid : {byte(0), byte(0xff)}) {
        MFRC522::version = invalid;
        assert(!begin() && reader.getLastError().startsWith("RFID_NOT_DETECTED"));
        assert(!bus.isAcquired(&reader) && MFRC522::live == 0 && SPI.active);
    }
    MFRC522::failReleaseOnProbe = true;
    assert(!begin());
    spiMutexFailTake = false; MFRC522::failReleaseOnProbe = false;
    assert(bus.isAcquired(&reader) && MFRC522::live == 1 && !reader.isInitialized());
    assert(instances.observe("rfid").state == InstanceState::CleanupPending);
    assert(!begin() && reader.getLastError().startsWith("RFID_STOP_REQUIRED"));
    assert(reader.stop(&error) && !bus.isAcquired(&reader) && MFRC522::live == 0);
    MFRC522::version = 0x92;
    assert(begin() && reader.isInitialized() && bus.isAcquired(&reader));
    const int beforeObservation = MFRC522::hardwareCalls;
    for (int i = 0; i < 1000; ++i) assert(instances.observe("rfid").initializedCount == 1);
    assert(MFRC522::hardwareCalls == beforeObservation);
    const int initCount = MFRC522::initialized, begins = SPI.begins;
    assert(begin() && reader.begin(5,27) && MFRC522::initialized == initCount);
    assert(!reader.begin(6,27,18,23,19));
    assert(reader.getLastError().startsWith("RFID_CONFIG_CHANGED") && MFRC522::live == 1);
    int peer;
    assert(bus.acquire(&peer,18,19,23,6,error));
    assert(!bus.end(error) && error.startsWith("SPI_IN_USE"));
    byte key[6] = {1,2,3,4,5,6}, data[16] = {42}, output[16] = {};
    assert(reader.scanNow(uid,cardType,error) && uid == "A1B2C3D4");
    RFIDDriver::Status snapshot;
    assert(reader.getStatus(snapshot,error) && snapshot.initialized && snapshot.cardPresent);
    assert(snapshot.uid == uid && snapshot.cardType == cardType);
    for (byte forbidden : {byte(0),byte(3),byte(63),byte(255)}) {
        const int calls = MFRC522::hardwareCalls;
        assert(!reader.readBlock(forbidden,key,output,error));
        assert(!reader.writeBlockVerified(forbidden,key,data,error));
        assert(MFRC522::hardwareCalls == calls);
    }
    assert(reader.writeBlockVerified(4,key,data,error));
    assert(reader.readBlock(4,key,output,error) && std::memcmp(output,data,16) == 0);
    MFRC522::authStatus = MFRC522::STATUS_ERROR;
    const int beforeAuthWrites = MFRC522::writes;
    assert(!reader.writeBlockVerified(4,key,data,error));
    assert(error.startsWith("RFID_AUTH_FAILED") && MFRC522::writes == beforeAuthWrites);
    MFRC522::authStatus = MFRC522::STATUS_OK;
    MFRC522::corruptRead = true;
    assert(!reader.writeBlockVerified(4,key,data,error) && error.startsWith("RFID_VERIFY_FAILED"));
    MFRC522::corruptRead = false; MFRC522::shortRead = true;
    assert(!reader.readBlock(4,key,output,error) && error.startsWith("RFID_READ_FAILED"));
    MFRC522::shortRead = false;
    MFRC522::writeStatus = MFRC522::STATUS_ERROR;
    assert(!reader.writeBlockVerified(4,key,data,error) && error.startsWith("RFID_WRITE_FAILED"));
    MFRC522::writeStatus = MFRC522::STATUS_OK; MFRC522::readStatus = MFRC522::STATUS_ERROR;
    assert(!reader.writeBlockVerified(4,key,data,error) && error.startsWith("RFID_VERIFY_FAILED"));
    MFRC522::readStatus = MFRC522::STATUS_OK; MFRC522::cardPresent = false;
    assert(!reader.scanNow(uid,cardType,error) && error.startsWith("RFID_CARD_NOT_PRESENT"));
    MFRC522::cardPresent = true;
    // A real production read holds the shared lock until authentication/read/cleanup ends.
    MFRC522::holdRead = true; MFRC522::readEntered = false;
    bool readOk = false;
    std::thread transfer([&] { String e; readOk = reader.readBlock(4,key,output,e); });
    while (!MFRC522::readEntered.load()) std::this_thread::yield();
    assert(instances.observe("rfid").state == InstanceState::Busy && spiMutexLastRequest == 0);
    const int antennaBefore = MFRC522::antennaOff, ioBefore = MFRC522::hardwareCalls;
    reader.check(); // Nonblocking: a polling tick cannot steal an in-flight transfer.
    assert(!reader.getStatus(snapshot,error) && error.startsWith("SPI_BUSY"));
    assert(snapshot.initialized && snapshot.uid == "A1B2C3D4"); // Failed snapshot is not fabricated.
    assert(!reader.stop(&error) && error.startsWith("SPI_BUSY"));
    assert(!bus.end(error) && error.startsWith("SPI_BUSY"));
    assert(MFRC522::live == 1 && MFRC522::antennaOff == antennaBefore && MFRC522::hardwareCalls == ioBefore);
    MFRC522::holdRead = false; transfer.join();
    assert(readOk && bus.isAcquired(&reader) && bus.isAcquired(&peer));
    assert(reader.stop(&error) && SPI.active && bus.isAcquired(&peer));
    assert(!reader.begin(6,27,18,23,19) && reader.getLastError().startsWith("SPI_CS_CONFLICT"));
    assert(MFRC522::live == 0 && bus.isAcquired(&peer));
    assert(bus.release(&peer,error));
    for (int i=0;i<1000;++i) {
        assert(begin() && bus.isAcquired(&reader));
        assert(reader.stop(&error) && !bus.isAcquired(&reader) && MFRC522::live == 0);
    }
    assert(SPI.begins == begins && spiMutexCreates - spiMutexDeletes == 1);
    assert(begin());
    SPI.active = false;
    assert(instances.observe("rfid").state == InstanceState::CleanupPending);
    const int lostIo = MFRC522::hardwareCalls;
    assert(reader.getStatus(snapshot,error) && !snapshot.initialized && !snapshot.cardPresent);
    assert(snapshot.lastError.startsWith("RFID_SPI_LEASE_LOST"));
    assert(!reader.scanNow(uid,cardType,error) && error.startsWith("RFID_SPI_LEASE_LOST"));
    assert(!begin() && reader.getLastError().startsWith("RFID_SPI_LEASE_LOST"));
    reader.check();
    assert(reader.stop(&error) && MFRC522::hardwareCalls == lostIo);
    assert(begin() && bus.isAcquired(&reader));
#if FASTBEE_ENABLE_PERIPH_EXEC
    int events = 0;
    PeriphExecManager::onEvent = [&](const String& event, const String& payload) {
        ++events;
        assert(event == "rfid_card_detected" && payload.indexOf("A1B2C3D4") >= 0);
        // Another thread can stop during dispatch: the polling lock is gone.
        bool stopped = false;
        std::thread callback([&] { String e; stopped = reader.stop(&e); });
        callback.join();
        assert(stopped);
    };
    rfidTestClock = 1000;
    reader.check();
    assert(events == 1 && !reader.isInitialized() && !bus.isAcquired(&reader));
    assert(begin());
    PeriphExecManager::onEvent = [&](const String& event, const String& payload) {
        ++events;
        assert(payload.indexOf("A1B2C3D4") >= 0);
        assert(event == (events == 2 ? "rfid_card_detected" : "rfid_card_removed"));
        bool unlocked = false;
        std::thread callback([&] { FastBee::SpiBusOperation guard(0); unlocked = guard.locked(); });
        callback.join(); assert(unlocked);
    };
    rfidTestClock = 2000; reader.check();
    MFRC522::cardPresent = false;
    rfidTestClock = 3500; reader.check();
    rfidTestClock = 5000; reader.check();
    assert(events == 3 && !reader.isCardPresent());
    MFRC522::cardPresent = true;
    PeriphExecManager::onEvent = nullptr;
#endif
    assert(reader.stop(&error) && reader.stop(&error) && MFRC522::live == 0);
    assert(instances.observe("rfid").state == InstanceState::NotInitialized);
    assert(bus.end(error)); // Explicit cleanup only after all mock users stop.
}
