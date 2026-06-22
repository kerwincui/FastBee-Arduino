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

    ets_printf("[4G] Initializing EC801E-CN 4G module...\n");
    ets_printf("[4G] UART pins - TX:%d, RX:%d, PWR:%d, Baud:%d\n",
                 _pinConfig.txPin, _pinConfig.rxPin, _pinConfig.pwrPin, _pinConfig.baudRate);

    // 上电（EC801E-CN PWRKEY 低电平有效，需要脉冲触发）
    powerOn();

    // 初始化串口 (UART2)
    _serial = &Serial2;
    _serial->begin(_pinConfig.baudRate, SERIAL_8N1, _pinConfig.rxPin, _pinConfig.txPin);

    // 等待模块启动（EC801E 上电后需要约 5-8 秒完成初始化）
    ets_printf("[4G] Waiting for modem startup (5s)...\n");
    delay(5000);

    // 清空串口缓冲
    while (_serial->available()) _serial->read();

    // 创建 TinyGSM modem 实例
    _modem = new TinyGsm(*_serial);
    _gsmClient = new TinyGsmClient(*_modem);

    // 等待 AT 响应（增加超时到 30 秒，EC801E 启动可能较慢）
    if (!waitForReady(30000)) {
        ets_printf("[4G] ERROR: Modem not responding to AT commands\n");
        // 尝试重新上电一次
        ets_printf("[4G] Retrying power cycle...\n");
        powerOff();
        delay(2000);
        powerOn();
        delay(5000);
        while (_serial->available()) _serial->read();
        if (!waitForReady(15000)) {
            ets_printf("[4G] ERROR: Modem still not responding after retry\n");
            powerOff();
            return false;
        }
    }

    // 获取模块信息
    String modemInfo = _modem->getModemInfo();
    ets_printf("[4G] Modem info: %s\n", modemInfo.c_str());
    
    // 打印 IMEI 用于诊断
    String imei = _modem->getIMEI();
    ets_printf("[4G] IMEI: %s\n", imei.length() > 0 ? imei.c_str() : "N/A");

    // 等待 SIM 卡就绪
    int simStatus = _modem->getSimStatus();
    if (simStatus != SIM_READY) {
        ets_printf("[4G] ERROR: SIM not ready, status: %d\n", simStatus);
        // SIM 可能需要额外时间初始化
        ets_printf("[4G] Waiting 3s for SIM...\n");
        delay(3000);
        simStatus = _modem->getSimStatus();
        if (simStatus != SIM_READY) {
            ets_printf("[4G] ERROR: SIM still not ready after retry, status: %d\n", simStatus);
            powerOff();
            return false;
        }
    }
    ets_printf("[4G] SIM ready\n");

    // 检查信号强度
    int16_t csq = _modem->getSignalQuality();
    ets_printf("[4G] Signal quality (CSQ): %d\n", csq);
    if (csq == 99 || csq == 0) {
        ets_printf("[4G] WARNING: Very weak or no signal (CSQ=%d)\n", csq);
    }

    // 激活网络
    if (!activateNetwork()) {
        ets_printf("[4G] ERROR: Failed to activate network\n");
        powerOff();
        return false;
    }

    _initialized = true;
    _connected = true;
    ets_printf("[4G] Initialized and connected successfully\n");
    return true;
}

bool CellularAdapter::isConnected() {
    if (!_initialized || !_modem) return false;
    // TinyGSM 的 isGprsConnected() 在 EC801E 上不可靠，使用 CGPADDR
    return _connected && checkPdpActive();
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

    // TinyGSM SIM7600 的 isGprsConnected() 使用 AT+CIPSTATUS，EC801E 不支持
    // 使用 AT+CGPADDR 检查 PDP 上下文是否仍有 IP 地址
    bool gprsOk = checkPdpActive();

    if (!gprsOk) {
        if (_connected) {
            _connected = false;
            ets_printf("[4G] GPRS connection lost (PDP no IP)\n");
        }
    } else if (!_connected) {
        _connected = true;
        ets_printf("[4G] GPRS connection restored\n");
    }
}

bool CellularAdapter::checkPdpActive() {
    String pip = readATResponse("AT+CGPADDR=1");
    // 响应格式: +CGPADDR: 1,"x.x.x.x"
    return (pip.indexOf("CGPADDR") >= 0 && pip.indexOf("\"") >= 0 && pip.indexOf("\"0.0.0.0\"") < 0);
}

bool CellularAdapter::reconnect() {
    if (!_initialized || !_modem) return false;

    LOG_INFO("CellularAdapter: Attempting reconnect...");
    _modem->gprsDisconnect();
    _connected = false;
    delay(1000);
    _connected = activateNetwork();
    return _connected;
}

void CellularAdapter::powerOn() {
    if (_pinConfig.pwrPin >= 0) {
        pinMode(_pinConfig.pwrPin, OUTPUT);
        // EC801E-CN PWRKEY 低电平有效：拉低 1 秒触发上电，然后释放
        digitalWrite(_pinConfig.pwrPin, LOW);
        delay(1000);
        digitalWrite(_pinConfig.pwrPin, HIGH);
        ets_printf("[4G] Power ON pulse sent (GPIO%d, LOW 1s -> HIGH)\n", _pinConfig.pwrPin);
    }
}

void CellularAdapter::powerOff() {
    if (_pinConfig.pwrPin >= 0) {
        // EC801E-CN 关机：拉低 PWRKEY 800ms+ 触发正常关机
        digitalWrite(_pinConfig.pwrPin, LOW);
        delay(1000);
        digitalWrite(_pinConfig.pwrPin, HIGH);
        ets_printf("[4G] Power OFF pulse sent (LOW 1s -> HIGH)\n");
        delay(3000);  // 等待模块完成关机
    }
}

bool CellularAdapter::waitForReady(uint32_t timeoutMs) {
    ets_printf("[4G] Testing AT communication (timeout %lums)...\n", (unsigned long)timeoutMs);
    unsigned long start = millis();

    while (millis() - start < timeoutMs) {
        if (_modem->testAT(1000)) {
            ets_printf("[4G] AT OK (%lums)\n", millis() - start);
            return true;
        }
        delay(500);
    }

    ets_printf("[4G] ERROR: AT timeout after %lums\n", millis() - start);
    return false;
}

bool CellularAdapter::activateNetwork() {
    ets_printf("[4G] Registering network (timeout 120s)...\n");

    // 等待网络注册（增加到 120 秒，4G 模块首次注册可能较慢）
    if (!_modem->waitForNetwork(120000)) {
        int16_t csq = _modem->getSignalQuality();
        ets_printf("[4G] ERROR: Network registration failed (CSQ=%d)\n", csq);
        return false;
    }
    ets_printf("[4G] Network registered\n");

    // 打印注册后的信号质量
    int16_t csq = _modem->getSignalQuality();
    ets_printf("[4G] CSQ after registration: %d\n", csq);
    
    // 获取运营商信息
    String op = _modem->getOperator();
    ets_printf("[4G] Operator: %s\n", op.length() > 0 ? op.c_str() : "N/A");

    // 检查是否已经连接 GPRS（模块重启后 PDP 上下文可能仍激活）
    if (_modem->isGprsConnected()) {
        IPAddress ip = _modem->localIP();
        ets_printf("[4G] GPRS already connected, IP: %s\n", ip.toString().c_str());
        _connected = true;
        return true;
    }

    // 激活 GPRS（最多重试 3 次，每次间隔递增）
    ets_printf("[4G] Connecting GPRS with APN: %s\n", _apn.c_str());
    for (int attempt = 1; attempt <= 3; attempt++) {
        ets_printf("[4G] GPRS connect attempt %d/3...\n", attempt);
        if (_modem->gprsConnect(_apn.c_str(), "", "")) {
            _connected = true;
            IPAddress ip = _modem->localIP();
            LOGGER.infof("CellularAdapter: GPRS connected, IP: %s", ip.toString().c_str());
            return true;
        }
        // TinyGSM SIM7600 模式用 AT+CIPSTATUS 检查状态，EC801E 不支持
        // 直接检查 CGPADDR 判断 PDP 是否实际上已成功
        if (checkPdpActive()) {
            ets_printf("[4G] TinyGSM reported fail but PDP is active! (IP assigned)\n");
            _connected = true;
            return true;
        }
        // AT 命令级诊断（仅在第一次失败时执行）
        if (attempt == 1) {
            ets_printf("[4G] Running AT diagnostics...\n");
            sendATDiag("AT+CGATT?");   // PS附着状态
            sendATDiag("AT+CGACT?");   // PDP上下文激活状态
            sendATDiag("AT+CGDCONT?"); // PDP上下文定义
            sendATDiag("AT+COPS?");    // 当前运营商
            sendATDiag("AT+CEREG?");   // 网络注册状态
            sendATDiag("AT+CIPSTATUS"); // IP连接状态
            // 尝试手动 PDP 激活作为后备
            ets_printf("[4G] Trying manual PDP activation...\n");
            sendATCmd("AT+CGATT=1");     // 确保PS附着
            delay(1000);
            {
                char atcmd[128];
                snprintf(atcmd, sizeof(atcmd), "AT+CGDCONT=1,\"IP\",\"%s\"", _apn.c_str());
                sendATCmd(atcmd);
            }
            delay(1000);
            sendATCmd("AT+CGACT=1,1");  // 激活PDP上下文
            delay(3000);
            sendATDiag("AT+CGPADDR=1"); // 查看分配的IP地址
            // 检查是否已经连接
            if (_modem->isGprsConnected()) {
                IPAddress ip = _modem->localIP();
                ets_printf("[4G] Manual PDP activation succeeded! IP: %s\n", ip.toString().c_str());
                _connected = true;
                return true;
            }
            // TinyGSM SIM7600 模式使用 AT+CIPSTATUS 检查连接，但 EC801E 不支持该命令
            // 改用 AT+CGPADDR 直接检查 PDP 上下文是否已分配 IP
            if (checkPdpActive()) {
                ets_printf("[4G] Manual PDP verified via CGPADDR (IP assigned)\n");
                _connected = true;
                return true;
            }
        }
        if (attempt < 3) {
            int waitSec = attempt * 5;  // 5s, 10s
            ets_printf("[4G] GPRS attempt %d failed, waiting %ds...\n", attempt, waitSec);
            delay(waitSec * 1000);
            // 重试前确认网络仍然注册
            if (!_modem->isNetworkConnected()) {
                ets_printf("[4G] WARNING: Network lost during retry, re-registering...\n");
                if (!_modem->waitForNetwork(60000)) {
                    ets_printf("[4G] ERROR: Re-registration failed\n");
                    return false;
                }
            }
        }
    }

    ets_printf("[4G] ERROR: GPRS connect failed after 3 attempts\n");
    return false;
}

void CellularAdapter::sendATDiag(const char* cmd) {
    if (!_serial) return;
    ets_printf("[4G-AT] >> %s\n", cmd);
    _serial->println(cmd);
    unsigned long start = millis();
    while (millis() - start < 3000) {
        if (_serial->available()) {
            String line = _serial->readStringUntil('\n');
            line.trim();
            if (line.length() > 0) {
                ets_printf("[4G-AT] << %s\n", line.c_str());
            }
        } else {
            delay(50);
        }
    }
}

void CellularAdapter::sendATCmd(const char* cmd) {
    if (!_serial) return;
    _serial->println(cmd);
    delay(500);
    while (_serial->available()) _serial->read();
}

String CellularAdapter::readATResponse(const char* cmd) {
    String result;
    if (!_serial) return result;
    _serial->println(cmd);
    unsigned long start = millis();
    while (millis() - start < 3000) {
        if (_serial->available()) {
            String line = _serial->readStringUntil('\n');
            line.trim();
            if (line.length() > 0 && !line.startsWith("OK") && !line.startsWith("ERROR") && line != cmd) {
                result = line;
            }
        } else {
            delay(50);
        }
    }
    return result;
}

#endif // FASTBEE_ENABLE_CELLULAR
