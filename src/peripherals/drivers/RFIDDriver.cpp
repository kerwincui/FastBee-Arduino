#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RFID

#include "peripherals/drivers/RFIDDriver.h"
#include "core/PeripheralExecution.h"
#include "core/PeriphExecManager.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"

#include <SPI.h>
#include <MFRC522.h>
#include <cstring>

RFIDDriver& RFIDDriver::getInstance() {
    static RFIDDriver instance;
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
    if (_initialized) return true;

    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("rfid")) {
        LOGGER.info("[RFID] Compiled but runtime-disabled, skip init");
        return false;
    }

    if (FastBee::FeatureBudgetRegistry::instance().isDenied("rfid")) {
        LOGGER.warning("[RFID] Disabled by memory budget, skip init");
        return false;
    }

    if (sckPin != 255 && mosiPin != 255 && misoPin != 255) {
        SPI.begin(sckPin, misoPin, mosiPin, ssPin);
    } else {
        SPI.begin();
    }
    MFRC522* rfid = new MFRC522(ssPin, rstPin);
    rfid->PCD_Init();

    // 验证通信
    byte version = rfid->PCD_ReadRegister(MFRC522::VersionReg);
    if (version == 0x00 || version == 0xFF) {
        LOGGER.errorf("[RFID] MFRC522 not detected (version=0x%02X) SS=%d RST=%d",
                     version, ssPin, rstPin);
        delete rfid;
        _lastError = "RFID_NOT_DETECTED: MFRC522 version register is invalid";
        return false;
    }

    rfid->PCD_SetAntennaGain(rfid->RxGain_max);  // 最大天线增益

    _mfrc522 = rfid;
    _initialized = true;
    _cardPresent = false;
    _lastUID = "";
    _lastCardType = "";
    _lastError = "";
    _sckPin = sckPin;
    _mosiPin = mosiPin;
    _misoPin = misoPin;

    LOGGER.infof("[RFID] MFRC522 initialized (v=0x%02X) SS=%d RST=%d",
                 version, ssPin, rstPin);
    return true;
}

void RFIDDriver::check() {
    if (!_initialized || !_mfrc522) return;

    unsigned long now = millis();
    if (now - _lastCheckTime < CHECK_INTERVAL_MS) return;
    _lastCheckTime = now;

    MFRC522* rfid = static_cast<MFRC522*>(_mfrc522);

    // 尝试检测新卡
    if (rfid->PICC_IsNewCardPresent() && rfid->PICC_ReadCardSerial()) {
        // 构建 UID 字符串
        String uid = "";
        for (byte i = 0; i < rfid->uid.size; i++) {
            if (rfid->uid.uidByte[i] < 0x10) uid += "0";
            uid += String(rfid->uid.uidByte[i], HEX);
        }
        uid.toUpperCase();

        _cardDetectedTime = now;

        // 新卡片或不同卡片
        if (!_cardPresent || uid != _lastUID) {
            _cardPresent = true;
            _lastUID = uid;
            _lastCardType = rfid->PICC_GetTypeName(rfid->PICC_GetType(rfid->uid.sak));

            LOGGER.infof("[RFID] Card detected: %s (type=%s)",
                         uid.c_str(),
                         _lastCardType.c_str());

            // 触发 RFID 卡片检测事件
#if FASTBEE_ENABLE_PERIPH_EXEC
            String eventData = "{\"uid\":\"" + uid + "\",\"size\":" + String(rfid->uid.size) + "}";
            PeriphExecManager::getInstance().dispatchEventMatchedRules("rfid_card_detected", eventData);
#endif
        }

        rfid->PICC_HaltA();
        rfid->PCD_StopCrypto1();
    } else {
        // 检查卡片是否超时移除
        if (_cardPresent && (now - _cardDetectedTime > CARD_TIMEOUT_MS)) {
            _cardPresent = false;
            LOGGER.infof("[RFID] Card removed (was: %s)", _lastUID.c_str());

#if FASTBEE_ENABLE_PERIPH_EXEC
            String eventData = "{\"uid\":\"" + _lastUID + "\"}";
            PeriphExecManager::getInstance().dispatchEventMatchedRules("rfid_card_removed", eventData);
#endif
        }
    }
}

bool RFIDDriver::selectCard(String& errorMsg) {
    if (!_initialized || !_mfrc522) {
        errorMsg = "RFID_NOT_INITIALIZED: reader is disabled or unavailable";
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

void RFIDDriver::stop() {
    if (_mfrc522) {
        MFRC522* rfid = static_cast<MFRC522*>(_mfrc522);
        rfid->PCD_AntennaOff();
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
    LOGGER.info("[RFID] Stopped");
}

#endif // FASTBEE_ENABLE_RFID
