/**
 * @file BLEProvisioner.cpp
 * @brief BLE 蓝牙配网管理器实现（NimBLE-Arduino 2.x）
 *
 * 服务/特征值 UUID 与历史蓝牙配网示例保持一致（Nordic UART Service），
 * 手机端写入 RX 特征值的 JSON：
 *   {"ssid":"...","password":"...","userId":"...","deviceNum":"...","extra":"..."}
 * 配网成功后通过 TX 特征值 notify "true"/"false" 结果。
 */

#include "network/BLEProvisioner.h"

#if FASTBEE_ENABLE_BLE

#include <NimBLEDevice.h>
#include <LittleFS.h>

#include "network/NetworkManager.h"
#include "network/handlers/ProvisionRouteHandler.h"
#include "systems/LoggerSystem.h"

// BLE UART 透传服务 UUID（与历史示例及手机端 App 兼容）
static const char* BLE_PROV_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9F";
static const char* BLE_PROV_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9F";  // 手机写入
static const char* BLE_PROV_TX_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9F";  // 结果通知

static const char* DEVICE_CONFIG_FILE = "/config/device.json";

// ---------------------------------------------------------------------------
// NimBLE 回调（运行在 NimBLE 宿主任务小栈上，仅置标志/拷贝数据）
// ---------------------------------------------------------------------------
class BleProvServerCallbacks : public NimBLEServerCallbacks {
public:
    explicit BleProvServerCallbacks(BLEProvisioner* owner) : owner(owner) {}

    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        (void)pServer;
        (void)connInfo;
        owner->_onClientConnect();
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        (void)pServer;
        (void)connInfo;
        (void)reason;
        owner->_onClientDisconnect();
    }

private:
    BLEProvisioner* owner;
};

class BleProvRxCallbacks : public NimBLECharacteristicCallbacks {
public:
    explicit BleProvRxCallbacks(BLEProvisioner* owner) : owner(owner) {}

    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        NimBLEAttValue value = pCharacteristic->getValue();
        if (value.size() > 0) {
            owner->_onRxData(reinterpret_cast<const char*>(value.data()), value.size());
        }
    }

private:
    BLEProvisioner* owner;
};

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
BLEProvisioner::BLEProvisioner(FBNetworkManager* netMgr) : netMgr(netMgr) {
    rxMutex = xSemaphoreCreateMutex();
    featureOwner_.ready();
}

BLEProvisioner::~BLEProvisioner() {
    featureOwner_.retiring();
    stop();
    if (rxMutex) {
        vSemaphoreDelete(rxMutex);
        rxMutex = nullptr;
    }
}

bool BLEProvisioner::begin(bool manual) {
    if (running) {
        return true;
    }
    if (!rxMutex) {
        rxMutex = xSemaphoreCreateMutex();
    }

    advName = _readAdvName();

    // 初始化协议栈并创建 GATT 服务
    if (!NimBLEDevice::init(advName.c_str())) {
        LOG_ERROR("[BLEProv] NimBLEDevice::init failed");
        return false;
    }
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // 最大发射功率，提升配网距离
    // 配网无需配对加密，保持 Just Works 开放写入
    NimBLEDevice::setSecurityAuth(false, false, false);

    server = NimBLEDevice::createServer();
    server->setCallbacks(new BleProvServerCallbacks(this));

    NimBLEService* service = server->createService(BLE_PROV_SERVICE_UUID);
    if (!service) {
        LOG_ERROR("[BLEProv] Failed to create BLE service");
        NimBLEDevice::deinit();
        server = nullptr;
        return false;
    }

    // TX：配网结果 notify；RX：配网数据 write
    txChar = service->createCharacteristic(BLE_PROV_TX_CHAR_UUID,
                                           NIMBLE_PROPERTY::NOTIFY);
    rxChar = service->createCharacteristic(BLE_PROV_RX_CHAR_UUID,
                                           NIMBLE_PROPERTY::WRITE);
    if (!txChar || !rxChar) {
        LOG_ERROR("[BLEProv] Failed to create BLE characteristics");
        NimBLEDevice::deinit();
        server = nullptr;
        txChar = nullptr;
        rxChar = nullptr;
        return false;
    }
    rxChar->setCallbacks(new BleProvRxCallbacks(this));
    service->start();

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_PROV_SERVICE_UUID);
    if (!advertising->start()) {
        LOG_ERROR("[BLEProv] Failed to start advertising");
        NimBLEDevice::deinit();
        server = nullptr;
        txChar = nullptr;
        rxChar = nullptr;
        return false;
    }

    running = true;
    manualStarted = manual;
    clientConnected = false;
    restartAdvPending = false;
    LOG_INFOF("[BLEProv] Started, advertising as '%s'", advName.c_str());
    return true;
}

void BLEProvisioner::stop() {
    if (!running) {
        return;
    }
    running = false;
    manualStarted = false;
    clientConnected = false;
    restartAdvPending = false;
    resultNotifyActive = false;

    // 停止广播并释放协议栈（回收 ~40KB RAM）
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (advertising) {
        advertising->stop();
    }
    NimBLEDevice::deinit();
    server = nullptr;   // deinit 已释放 GATT 对象
    txChar = nullptr;
    rxChar = nullptr;
    LOG_INFO("[BLEProv] Stopped, BLE stack released");
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------
void BLEProvisioner::update() {
    if (!running) {
        return;
    }

    // 1. 处理待处理的配网载荷（NimBLE 任务仅拷贝，重活在此执行）
    if (rxPending) {
        String rx;
        if (rxMutex && xSemaphoreTake(rxMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            rx = pendingRx;
            pendingRx = "";
            rxPending = false;
            xSemaphoreGive(rxMutex);
        }
        if (rx.length() > 0) {
            _processProvisionPayload(rx);
            if (!running) {
                return;  // 配网成功后可能已自动停止
            }
        }
    }

    // 2. 排空配网结果通知（最多 RESULT_NOTIFY_COUNT 次，间隔 100ms，
    //    与历史示例一致：过密 notify 会导致蓝牙协议栈拥塞）
    if (resultNotifyActive) {
        if (resultNotifyRemain > 0 &&
            millis() - lastNotifyMs >= RESULT_NOTIFY_INTERVAL_MS) {
            lastNotifyMs = millis();
            if (txChar) {
                txChar->setValue(buildBleProvisionResult(lastResultOk));
                txChar->notify();
            }
            resultNotifyRemain--;
        }
        if (resultNotifyRemain == 0) {
            resultNotifyActive = false;
            if (stopAfterNotify) {
                stop();  // 配网完成，释放 BLE 内存
                return;
            }
        }
    }

    // 3. 断连后延迟恢复广播（给协议栈留出稳定时间）
    if (restartAdvPending && millis() - restartAdvAt >= RESTART_ADV_DELAY_MS) {
        restartAdvPending = false;
        NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
        if (advertising && !clientConnected) {
            advertising->start();
            LOG_INFO("[BLEProv] Advertising restarted");
        }
    }
}

// ---------------------------------------------------------------------------
// NimBLE 回调入口（小栈上仅置标志/拷贝）
// ---------------------------------------------------------------------------
void BLEProvisioner::_onClientConnect() {
    clientConnected = true;
    restartAdvPending = false;
    LOG_INFO("[BLEProv] Client connected");
}

void BLEProvisioner::_onClientDisconnect() {
    clientConnected = false;
    if (running) {
        restartAdvPending = true;
        restartAdvAt = millis();
    }
    LOG_INFO("[BLEProv] Client disconnected");
}

void BLEProvisioner::_onRxData(const char* data, size_t len) {
    if (data == nullptr || len == 0 || len > RX_MAX_LEN) {
        return;
    }
    if (rxMutex && xSemaphoreTake(rxMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        pendingRx = String(data, static_cast<unsigned int>(len));
        rxPending = true;
        xSemaphoreGive(rxMutex);
    }
}

// ---------------------------------------------------------------------------
// 配网数据处理（loopTask 上执行）
// ---------------------------------------------------------------------------
void BLEProvisioner::_processProvisionPayload(const String& rx) {
    LOG_INFOF("[BLEProv] Provision payload received (%u bytes)",
              static_cast<unsigned>(rx.length()));

    BleProvisionPayload payload;
    if (!parseBleProvisionPayload(rx, payload)) {
        LOG_ERROR("[BLEProv] Invalid provision payload (missing ssid or bad JSON)");
        _startResultNotify(false, false);
        return;
    }
    LOG_INFOF("[BLEProv] SSID=%s userId=%s deviceNum=%s",
              payload.ssid.c_str(), payload.userId.c_str(),
              payload.deviceNum.c_str());

    // 1. 扩展参数写入 device.json（与 AP 配网 /api/wifi/config 同一路径）
    if (payload.hasUserId || payload.hasDeviceNum || payload.hasExtra) {
        ProvisionRouteHandler::_updateDeviceConfig(payload.userId,
                                                   payload.deviceNum,
                                                   payload.extra);
    }

    // 2. 通过 NetworkManager 保存 WiFi 配置并触发 STA 模式切换
    if (netMgr) {
        WiFiConfig cfg = netMgr->getConfig();
        cfg.staSSID = payload.ssid;
        cfg.staPassword = payload.password;
        cfg.mode = NetworkMode::NETWORK_STA;
        cfg.networkType = NetworkType::NET_WIFI;

        if (netMgr->updateConfig(cfg, true)) {
            LOG_INFO("[BLEProv] WiFi configuration saved, network will restart to STA");
            _startResultNotify(true, true);  // 成功后排空通知并自动停止
            return;
        }
        LOG_ERROR("[BLEProv] Failed to save WiFi configuration");
    } else {
        LOG_ERROR("[BLEProv] No NetworkManager available");
    }
    _startResultNotify(false, false);
}

void BLEProvisioner::_startResultNotify(bool success, bool autoStop) {
    lastResultOk = success;
    stopAfterNotify = autoStop;
    lastNotifyMs = millis();
    resultNotifyRemain = RESULT_NOTIFY_COUNT;
    resultNotifyActive = true;

    // 立即发送首次通知，其余在 update() 中按间隔排空
    if (txChar) {
        txChar->setValue(buildBleProvisionResult(success));
        txChar->notify();
        resultNotifyRemain--;
    }
    LOG_INFOF("[BLEProv] Provision result: %s", success ? "success" : "failed");
}

// ---------------------------------------------------------------------------
// 状态查询
// ---------------------------------------------------------------------------
BLEProvisionState BLEProvisioner::getState() const {
    if (!running) {
        return BLEProvisionState::IDLE;
    }
    if (lastResultOk) {
        return BLEProvisionState::PROVISIONED;
    }
    if (clientConnected) {
        return BLEProvisionState::CONNECTED;
    }
    return BLEProvisionState::ADVERTISING;
}

void BLEProvisioner::fillStatusJson(JsonObject& data) const {
    const char* stateStr = "idle";
    switch (getState()) {
        case BLEProvisionState::ADVERTISING: stateStr = "advertising"; break;
        case BLEProvisionState::CONNECTED:   stateStr = "connected";   break;
        case BLEProvisionState::PROVISIONED: stateStr = "provisioned"; break;
        default: break;
    }
    data["supported"] = true;
    data["running"] = running;
    data["state"] = stateStr;
    data["connected"] = static_cast<bool>(clientConnected);
    data["provisioned"] = static_cast<bool>(lastResultOk);
    data["manual"] = manualStarted;
    data["advName"] = advName;
}

String BLEProvisioner::_readAdvName() {
    String name = "FastBee";
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument doc;
            if (!deserializeJson(doc, f) &&
                doc["deviceName"].is<const char*>() &&
                strlen(doc["deviceName"].as<const char*>()) > 0) {
                name = doc["deviceName"].as<String>();
            }
            f.close();
        }
    }
    // 传统广播包名称空间有限，截断到 20 字符
    if (name.length() > 20) {
        name = name.substring(0, 20);
    }
    return name;
}

#endif // FASTBEE_ENABLE_BLE
