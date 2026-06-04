#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RFID

#include "peripherals/drivers/RFIDDriver.h"
#include "core/PeripheralExecution.h"
#include "core/PeriphExecManager.h"
#include "systems/LoggerSystem.h"

#include <SPI.h>
#include <MFRC522.h>

RFIDDriver& RFIDDriver::getInstance() {
    static RFIDDriver instance;
    return instance;
}

RFIDDriver::~RFIDDriver() {
    stop();
}

bool RFIDDriver::begin(uint8_t ssPin, uint8_t rstPin) {
    if (_initialized) return true;

    SPI.begin();
    MFRC522* rfid = new MFRC522(ssPin, rstPin);
    rfid->PCD_Init();

    // 验证通信
    byte version = rfid->PCD_ReadRegister(MFRC522::VersionReg);
    if (version == 0x00 || version == 0xFF) {
        LOGGER.errorf("[RFID] MFRC522 not detected (version=0x%02X) SS=%d RST=%d",
                     version, ssPin, rstPin);
        delete rfid;
        return false;
    }

    rfid->PCD_SetAntennaGain(rfid->RxGain_max);  // 最大天线增益

    _mfrc522 = rfid;
    _initialized = true;
    _cardPresent = false;
    _lastUID = "";

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

            LOGGER.infof("[RFID] Card detected: %s (type=%s)",
                         uid.c_str(),
                         rfid->PICC_GetTypeName(rfid->PICC_GetType(rfid->uid.sak)));

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
    LOGGER.info("[RFID] Stopped");
}

#endif // FASTBEE_ENABLE_RFID
