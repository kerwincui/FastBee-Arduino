/**
 * @file CellularAdapter.cpp
 * @brief 4G 蜂窝模块适配器实现 (EC801E-CN)
 * @author kerwincui
 * @date 2026-06-02
 */

#include "network/CellularAdapter.h"

#if FASTBEE_ENABLE_CELLULAR

#include "systems/LoggerSystem.h"

CellularAdapter::CellularAdapter() {}

CellularAdapter::~CellularAdapter() {
    disconnect();
    if (_gsmClient) { delete _gsmClient; _gsmClient = nullptr; }
    if (_modem) { delete _modem; _modem = nullptr; }
    // _serial 使用 HardwareSerial 全局实例，不 delete
}

bool CellularAdapter::begin(const WiFiConfig& config) {
    if (_initialized) {
        LOG_WARNING("CellularAdapter: Already initialized");
        return true;
    }

    _pinConfig = config.cellular;
    _apn = config.cellular.apn;

    LOG_INFO("CellularAdapter: Initializing EC801E-CN 4G module...");
    LOGGER.infof("CellularAdapter: UART pins - TX:%d, RX:%d, PWR:%d, Baud:%d",
                 _pinConfig.txPin, _pinConfig.rxPin, _pinConfig.pwrPin, _pinConfig.baudRate);

    // 上电
    powerOn();

    // 初始化串口 (UART2)
    _serial = &Serial2;
    _serial->begin(_pinConfig.baudRate, SERIAL_8N1, _pinConfig.rxPin, _pinConfig.txPin);

    // 等待模块启动
    LOG_INFO("CellularAdapter: Waiting for modem startup...");
    delay(3000);  // EC801E 上电后需要约 3 秒启动

    // 清空串口缓冲
    while (_serial->available()) _serial->read();

    // 创建 TinyGSM modem 实例
    _modem = new TinyGsm(*_serial);
    _gsmClient = new TinyGsmClient(*_modem);

    // 等待 AT 响应
    if (!waitForReady()) {
        LOG_ERROR("CellularAdapter: Modem not responding");
        powerOff();
        return false;
    }

    // 获取模块信息
    String modemInfo = _modem->getModemInfo();
    LOG_INFO("CellularAdapter: Modem info: " + modemInfo);

    // 等待 SIM 卡就绪
    int simStatus = _modem->getSimStatus();
    if (simStatus != SIM_READY) {
        LOG_ERROR("CellularAdapter: SIM not ready, status: " + String(simStatus));
        powerOff();
        return false;
    }
    LOG_INFO("CellularAdapter: SIM ready");

    // 激活网络
    if (!activateNetwork()) {
        LOG_ERROR("CellularAdapter: Failed to activate network");
        powerOff();
        return false;
    }

    _initialized = true;
    _connected = true;
    LOG_INFO("CellularAdapter: Initialized and connected successfully");
    return true;
}

bool CellularAdapter::isConnected() {
    if (!_initialized || !_modem) return false;
    return _connected && _modem->isGprsConnected();
}

void CellularAdapter::disconnect() {
    if (_modem && _connected) {
        _modem->gprsDisconnect();
        LOG_INFO("CellularAdapter: GPRS disconnected");
    }
    _connected = false;
    _initialized = false;
    powerOff();
}

Client* CellularAdapter::getClient() {
    return _gsmClient;
}

String CellularAdapter::getSignalQuality() {
    if (!_modem) return "N/A";
    int16_t csq = _modem->getSignalQuality();
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", csq);
    return String(buf);
}

String CellularAdapter::getICCID() {
    if (!_modem) return "";
    return _modem->getSimCCID();
}

String CellularAdapter::getIMEI() {
    if (!_modem) return "";
    return _modem->getIMEI();
}

String CellularAdapter::getOperator() {
    if (!_modem) return "";
    return _modem->getOperator();
}

String CellularAdapter::getNetworkType() {
    if (!_modem || !_connected) return "";
    
    // 通过 AT+COPS 获取网络类型
    String act = _modem->getLocalIP();  // 触发检查
    
    // TinyGSM 不提供直接的网络类型接口，使用信号质量辅助判断
    int16_t csq = _modem->getSignalQuality();
    if (csq > 0) {
        return "4G";  // EC801E 支持 4G LTE
    }
    return "unknown";
}

int CellularAdapter::getSignalQualityCSQ() {
    if (!_modem) return 99;
    int16_t csq = _modem->getSignalQuality();
    return (csq >= 0 && csq <= 31) ? csq : 99;
}

bool CellularAdapter::isSimReady() {
    if (!_modem) return false;
    int status = _modem->getSimStatus();
    return (status == SIM_READY);
}

String CellularAdapter::getStatusString() const {
    if (!_initialized) return "uninitialized";
    if (!_connected) return "disconnected";
    return "connected";
}

IPAddress CellularAdapter::localIP() {
    if (!_modem || !_connected) return IPAddress(0, 0, 0, 0);
    return _modem->localIP();
}

void CellularAdapter::update() {
    if (!_initialized || !_modem) return;

    // 每 30 秒检查连接状态
    unsigned long now = millis();
    if (now - _lastCheckTime < 30000) return;
    _lastCheckTime = now;

    if (!_modem->isGprsConnected()) {
        if (_connected) {
            _connected = false;
            LOG_WARNING("CellularAdapter: GPRS connection lost");
        }
    } else if (!_connected) {
        _connected = true;
        LOG_INFO("CellularAdapter: GPRS connection restored");
    }
}

bool CellularAdapter::reconnect() {
    if (!_initialized || !_modem) return false;

    LOG_INFO("CellularAdapter: Attempting reconnect...");
    _modem->gprsDisconnect();
    delay(1000);
    return activateNetwork();
}

void CellularAdapter::powerOn() {
    if (_pinConfig.pwrPin >= 0) {
        pinMode(_pinConfig.pwrPin, OUTPUT);
        digitalWrite(_pinConfig.pwrPin, HIGH);
        LOG_INFO("CellularAdapter: Power ON (GPIO" + String(_pinConfig.pwrPin) + ")");
    }
}

void CellularAdapter::powerOff() {
    if (_pinConfig.pwrPin >= 0) {
        digitalWrite(_pinConfig.pwrPin, LOW);
        LOG_INFO("CellularAdapter: Power OFF");
    }
}

bool CellularAdapter::waitForReady(uint32_t timeoutMs) {
    LOG_INFO("CellularAdapter: Testing AT communication...");
    unsigned long start = millis();

    while (millis() - start < timeoutMs) {
        if (_modem->testAT(1000)) {
            LOG_INFO("CellularAdapter: AT OK");
            return true;
        }
        delay(500);
    }

    LOG_ERROR("CellularAdapter: AT timeout");
    return false;
}

bool CellularAdapter::activateNetwork() {
    LOG_INFO("CellularAdapter: Registering network...");

    // 等待网络注册
    if (!_modem->waitForNetwork(60000)) {
        LOG_ERROR("CellularAdapter: Network registration failed");
        return false;
    }
    LOG_INFO("CellularAdapter: Network registered");

    // 激活 GPRS
    LOGGER.infof("CellularAdapter: Connecting GPRS with APN: %s", _apn.c_str());
    if (!_modem->gprsConnect(_apn.c_str(), "", "")) {
        LOG_ERROR("CellularAdapter: GPRS connect failed");
        return false;
    }

    _connected = true;
    IPAddress ip = _modem->localIP();
    LOGGER.infof("CellularAdapter: GPRS connected, IP: %s", ip.toString().c_str());
    return true;
}

#endif // FASTBEE_ENABLE_CELLULAR
