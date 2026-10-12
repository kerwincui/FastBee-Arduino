#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RFID

#include "peripherals/drivers/RFIDDriver.h"
#include "core/PeripheralExecution.h"
#include "core/PeriphExecManager.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"
#include "core/SpiBusInitializer.h"
#include "core/FeatureInstanceRegistry.h"

#include <SPI.h>
#include <MFRC522.h>
#include <cstring>
#include <new>

RFIDDriver& RFIDDriver::getInstance() {
    static RFIDDriver instance;
    static const FastBee::FeatureInstanceObserver observer{"rfid", [](const void* context) {
        using namespace FastBee;
        SpiBusObservation guard;
        if (guard.unused()) return FeatureInstanceSnapshot{FeatureInstanceState::NotInitialized, 0};
        if (!guard.locked()) return FeatureInstanceSnapshot{FeatureInstanceState::Busy, 0};
        const auto* driver = static_cast<const RFIDDriver*>(context);
        if (driver->_initialized && SPI.bus() && SpiBusInitializer::instance().isAcquired(driver))
            return FeatureInstanceSnapshot{FeatureInstanceState::Initialized, 1};
        return FeatureInstanceSnapshot{driver->_mfrc522 ? FeatureInstanceState::CleanupPending
            : FeatureInstanceState::NotInitialized, 0};
    }, &instance};
    static const bool registered = FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
    (void)registered;
    return instance;
}

RFIDDriver::~RFIDDriver() {
    stop();
}

bool RFIDDriver::begin(uint8_t ssPin, uint8_t rstPin) {
    return begin(ssPin, rstPin, 255, 255, 255);
}

bool RFIDDriver::begin(uint8_t ssPin, uint8_t rstPin, uint8_t sckPin,
                       uint8_t mosiPin, uint8_t misoPin) {
    FastBee::SpiBusOperation operation;
    if (!operation.locked()) return false;
    const bool useDefaults = sckPin == 255 && mosiPin == 255 && misoPin == 255;
    if (!useDefaults && (sckPin == 255 || mosiPin == 255 || misoPin == 255)) {
        _lastError = "RFID_SPI_PINS_INVALID: specify all three SPI pins or none";
        return false;
    }
    if (_initialized) {
        if (_ssPin != ssPin || _rstPin != rstPin ||
            _sckPin != (useDefaults ? SCK : sckPin) ||
            _mosiPin != (useDefaults ? MOSI : mosiPin) ||
            _misoPin != (useDefaults ? MISO : misoPin)) {
            _lastError = "RFID_CONFIG_CHANGED: stop the existing reader first";
            return false;
        }
        if (!SPI.bus() || !FastBee::SpiBusInitializer::instance().isAcquired(this)) {
            _lastError = "RFID_SPI_LEASE_LOST: stop before recovering the bus";
            return false;
        }
        _lastError = "";
        return true;
    }
    if (_mfrc522) {
        _lastError = "RFID_STOP_REQUIRED: previous reader cleanup is pending";
        return false;
    }

    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("rfid")) {
        _lastError = "RFID_RUNTIME_DISABLED: reader is not enabled";
        LOGGER.info("[RFID] Compiled but runtime-disabled, skip init");
        return false;
    }

    if (FastBee::FeatureBudgetRegistry::instance().isDenied("rfid")) {
        _lastError = "RFID_BUDGET_DENIED: reader was not admitted";
        LOGGER.warning("[RFID] Disabled by memory budget, skip init");
        return false;
    }

    MFRC522* rfid = new (std::nothrow) MFRC522(ssPin, rstPin);
    if (!rfid) {
        _lastError = "RFID_ALLOCATION_FAILED: unable to allocate reader";
        return false;
    }
    if (!FastBee::SpiBusInitializer::instance().acquire(
            this, useDefaults ? SCK : sckPin, useDefaults ? MISO : misoPin,
            useDefaults ? MOSI : mosiPin, ssPin, _lastError, FastBee::ResourceConsumerRole::Sensor)) {
        delete rfid;
        return false;
    }
    rfid->PCD_Init();

    // 验证通信
    byte version = rfid->PCD_ReadRegister(MFRC522::VersionReg);
    if (version == 0x00 || version == 0xFF) {
        LOGGER.errorf("[RFID] MFRC522 not detected (version=0x%02X) SS=%d RST=%d",
                     version, ssPin, rstPin);
        _lastError = "RFID_NOT_DETECTED: MFRC522 version register is invalid";
        String releaseError;
        if (!FastBee::SpiBusInitializer::instance().release(this, releaseError)) {
            _mfrc522 = rfid; // Keep ownership until an explicit stop can retry.
            _lastError += String("; ") + releaseError;
            return false;
        }
        rfid->PCD_AntennaOff();
        delete rfid;
        return false;
    }

    rfid->PCD_SetAntennaGain(rfid->RxGain_max);  // 最大天线增益

    _mfrc522 = rfid;
    _initialized = true;
    _cardPresent = false;
    _lastUID = "";
    _lastCardType = "";
    _lastError = "";
    _sckPin = useDefaults ? SCK : sckPin;
    _mosiPin = useDefaults ? MOSI : mosiPin;
    _misoPin = useDefaults ? MISO : misoPin;
    _ssPin = ssPin;
    _rstPin = rstPin;
    _lastCheckTime = 0;
    _cardDetectedTime = 0;

    LOGGER.infof("[RFID] MFRC522 initialized (v=0x%02X) SS=%d RST=%d",
                 version, ssPin, rstPin);
    return true;
}

void RFIDDriver::check() {
#if FASTBEE_ENABLE_PERIPH_EXEC
    const char* eventName = nullptr;
    String eventData;
#endif
    {
        // Polling must not stall the main loop behind another client's transfer.
        FastBee::SpiBusOperation operation(0);
        if (!operation.locked()) return;
        if (!_initialized || !_mfrc522) return;
        if (!SPI.bus() || !FastBee::SpiBusInitializer::instance().isAcquired(this)) {
            _lastError = "RFID_SPI_LEASE_LOST: stop before recovering the bus";
            return;
        }

        unsigned long now = millis();
        if (now - _lastCheckTime < CHECK_INTERVAL_MS) return;
        _lastCheckTime = now;

        MFRC522* rfid = static_cast<MFRC522*>(_mfrc522);

        // 尝试检测新卡
        if (rfid->PICC_IsNewCardPresent() && rfid->PICC_ReadCardSerial()) {
            String uid = "";
            for (byte i = 0; i < rfid->uid.size; i++) {
                if (rfid->uid.uidByte[i] < 0x10) uid += "0";
                uid += String(rfid->uid.uidByte[i], HEX);
            }
            uid.toUpperCase();
            _cardDetectedTime = now;

            // 新卡片或不同卡片；先保存事件，事务结束并解锁后派发。
            if (!_cardPresent || uid != _lastUID) {
                _cardPresent = true;
                _lastUID = uid;
                _lastCardType = rfid->PICC_GetTypeName(rfid->PICC_GetType(rfid->uid.sak));
                LOGGER.infof("[RFID] Card detected: %s (type=%s)",
                             uid.c_str(), _lastCardType.c_str());
#if FASTBEE_ENABLE_PERIPH_EXEC
                eventName = "rfid_card_detected";
                eventData = "{\"uid\":\"" + uid + "\",\"size\":" + String(rfid->uid.size) + "}";
#endif
            }
            rfid->PICC_HaltA();
            rfid->PCD_StopCrypto1();
        } else if (_cardPresent && (now - _cardDetectedTime > CARD_TIMEOUT_MS)) {
            _cardPresent = false;
            LOGGER.infof("[RFID] Card removed (was: %s)", _lastUID.c_str());
#if FASTBEE_ENABLE_PERIPH_EXEC
            eventName = "rfid_card_removed";
            eventData = "{\"uid\":\"" + _lastUID + "\"}";
#endif
        }
    } // Release SPI before a rule can re-enter a driver or storage mutex.
#if FASTBEE_ENABLE_PERIPH_EXEC
    if (eventName) PeriphExecManager::getInstance().dispatchEventMatchedRules(eventName, eventData);
#endif
}

bool RFIDDriver::selectCard(String& errorMsg) {
    if (!_initialized || !_mfrc522) {
        errorMsg = "RFID_NOT_INITIALIZED: reader is disabled or unavailable";
        _lastError = errorMsg;
        return false;
    }
    if (!SPI.bus() || !FastBee::SpiBusInitializer::instance().isAcquired(this)) {
        errorMsg = "RFID_SPI_LEASE_LOST: stop before recovering the bus";
        _lastError = errorMsg;
        return false;
    }
    MFRC522* rfid = static_cast<MFRC522*>(_mfrc522);
    if (!rfid->PICC_IsNewCardPresent() || !rfid->PICC_ReadCardSerial()) {
        errorMsg = "RFID_CARD_NOT_PRESENT: place a supported card on the reader";
        _lastError = errorMsg;
        return false;
    }

    _lastUID = "";
    for (byte i = 0; i < rfid->uid.size; ++i) {
        if (rfid->uid.uidByte[i] < 0x10) _lastUID += "0";
        _lastUID += String(rfid->uid.uidByte[i], HEX);
    }
    _lastUID.toUpperCase();
    _lastCardType = rfid->PICC_GetTypeName(rfid->PICC_GetType(rfid->uid.sak));
    _cardPresent = true;
    _cardDetectedTime = millis();
    _lastError = "";
    return true;
}

void RFIDDriver::finishCardOperation() {
    if (!_mfrc522) return;
    MFRC522* rfid = static_cast<MFRC522*>(_mfrc522);
    rfid->PCD_StopCrypto1();
    rfid->PICC_HaltA();
}

bool RFIDDriver::scanNow(String& uid, String& cardType, String& errorMsg) {
    FastBee::SpiBusOperation operation;
    if (!operation.locked()) {
        errorMsg = "SPI_BUSY: RFID scan deferred";
        return false;
    }
    errorMsg = "";
    if (!selectCard(errorMsg)) return false;
    uid = _lastUID;
    cardType = _lastCardType;
    finishCardOperation();
    return true;
}

bool RFIDDriver::authenticateBlock(uint8_t block, const uint8_t key[6],
                                   String& errorMsg) {
    if (!_mfrc522 || !key) {
        errorMsg = "RFID_AUTH_INVALID: a six-byte key is required";
        _lastError = errorMsg;
        return false;
    }
    MFRC522* rfid = static_cast<MFRC522*>(_mfrc522);
    MFRC522::MIFARE_Key authKey;
    memcpy(authKey.keyByte, key, sizeof(authKey.keyByte));
    const MFRC522::StatusCode status = rfid->PCD_Authenticate(
        MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &authKey, &rfid->uid);
    if (status != MFRC522::STATUS_OK) {
        errorMsg = String("RFID_AUTH_FAILED: ") + rfid->GetStatusCodeName(status);
        _lastError = errorMsg;
        return false;
    }
    return true;
}

bool RFIDDriver::readBlock(uint8_t block, const uint8_t key[6], uint8_t data[16],
                           String& errorMsg) {
    FastBee::SpiBusOperation operation;
    if (!operation.locked()) {
        errorMsg = "SPI_BUSY: RFID read deferred";
        return false;
    }
    errorMsg = "";
    if (!data || block == 0 || (block % 4) == 3 || block > 62) {
        errorMsg = "RFID_BLOCK_FORBIDDEN: only MIFARE Classic data blocks 1-62 are allowed";
        _lastError = errorMsg;
        return false;
    }
    if (!selectCard(errorMsg)) return false;
    if (!authenticateBlock(block, key, errorMsg)) {
        finishCardOperation();
        return false;
    }
    MFRC522* rfid = static_cast<MFRC522*>(_mfrc522);
    uint8_t buffer[18] = {0};
    byte length = sizeof(buffer);
    const MFRC522::StatusCode status = rfid->MIFARE_Read(block, buffer, &length);
    if (status != MFRC522::STATUS_OK || length < 16) {
        errorMsg = String("RFID_READ_FAILED: ") + rfid->GetStatusCodeName(status);
        _lastError = errorMsg;
        finishCardOperation();
        return false;
    }
    memcpy(data, buffer, 16);
    finishCardOperation();
    _lastError = "";
    return true;
}

bool RFIDDriver::isWritableDataBlock(uint8_t block) {
    return block > 0 && block <= 62 && (block % 4) != 3;
}

bool RFIDDriver::writeBlockVerified(uint8_t block, const uint8_t key[6],
                                    const uint8_t data[16], String& errorMsg) {
    FastBee::SpiBusOperation operation;
    if (!operation.locked()) {
        errorMsg = "SPI_BUSY: RFID write deferred";
        return false;
    }
    errorMsg = "";
    if (!data || !isWritableDataBlock(block)) {
        errorMsg = "RFID_BLOCK_FORBIDDEN: manufacturer and sector trailer blocks cannot be written";
        _lastError = errorMsg;
        return false;
    }
    if (!selectCard(errorMsg)) return false;
    if (!authenticateBlock(block, key, errorMsg)) {
        finishCardOperation();
        return false;
    }
    MFRC522* rfid = static_cast<MFRC522*>(_mfrc522);
    MFRC522::StatusCode status = rfid->MIFARE_Write(block, const_cast<byte*>(data), 16);
    if (status != MFRC522::STATUS_OK) {
        errorMsg = String("RFID_WRITE_FAILED: ") + rfid->GetStatusCodeName(status);
        _lastError = errorMsg;
        finishCardOperation();
        return false;
    }
    uint8_t verify[18] = {0};
    byte verifyLength = sizeof(verify);
    status = rfid->MIFARE_Read(block, verify, &verifyLength);
    if (status != MFRC522::STATUS_OK || verifyLength < 16 || memcmp(data, verify, 16) != 0) {
        errorMsg = "RFID_VERIFY_FAILED: read-back data differs after write";
        _lastError = errorMsg;
        finishCardOperation();
        return false;
    }
    finishCardOperation();
    _lastError = "";
    return true;
}

bool RFIDDriver::stop(String* errorMsg) {
    FastBee::SpiBusOperation operation;
    if (!operation.locked()) {
        if (errorMsg) *errorMsg = "SPI_BUSY: RFID reader and pins retained";
        return false;
    }
    String releaseError;
    const bool canAccessHardware = SPI.bus() &&
        FastBee::SpiBusInitializer::instance().isAcquired(this);
    if (!FastBee::SpiBusInitializer::instance().release(this, releaseError)) {
        _lastError = releaseError;
        if (errorMsg) *errorMsg = releaseError;
        return false;
    }
    if (_mfrc522) {
        MFRC522* rfid = static_cast<MFRC522*>(_mfrc522);
        if (canAccessHardware) rfid->PCD_AntennaOff();
        delete rfid;
        _mfrc522 = nullptr;
    }
    _initialized = false;
    _cardPresent = false;
    _lastUID = "";
    _lastCardType = "";
    _lastError = "";
    _sckPin = 255;
    _mosiPin = 255;
    _misoPin = 255;
    _ssPin = 255;
    _rstPin = 255;
    _lastCheckTime = 0;
    _cardDetectedTime = 0;
    if (errorMsg) *errorMsg = "";
    LOGGER.info("[RFID] Stopped");
    return true;
}

bool RFIDDriver::getStatus(Status& status, String& errorMsg) const {
    FastBee::SpiBusOperation operation(0);
    if (!operation.locked()) {
        errorMsg = "SPI_BUSY: RFID status unavailable";
        return false;
    }
    status.initialized = _initialized;
    status.cardPresent = _cardPresent;
    status.uid = _lastUID;
    status.cardType = _lastCardType;
    status.lastError = _lastError;
    if (_initialized && (!SPI.bus() || !FastBee::SpiBusInitializer::instance().isAcquired(this))) {
        status.initialized = false;
        status.cardPresent = false;
        status.lastError = "RFID_SPI_LEASE_LOST: stop before recovering the bus";
    }
    errorMsg = "";
    return true;
}

String RFIDDriver::getLastUID() const {
    FastBee::SpiBusOperation operation;
    return operation.locked() ? _lastUID : String();
}

String RFIDDriver::getLastCardType() const {
    FastBee::SpiBusOperation operation;
    return operation.locked() ? _lastCardType : String();
}

String RFIDDriver::getLastError() const {
    FastBee::SpiBusOperation operation;
    return operation.locked() ? _lastError : String("SPI_BUSY: RFID status unavailable");
}

bool RFIDDriver::isCardPresent() const {
    FastBee::SpiBusOperation operation;
    return operation.locked() && _cardPresent;
}

bool RFIDDriver::isInitialized() const {
    FastBee::SpiBusOperation operation;
    return operation.locked() && _initialized;
}

#endif // FASTBEE_ENABLE_RFID
