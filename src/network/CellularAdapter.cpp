/**
 * @file CellularAdapter.cpp
 * @brief 4G 蜂窝模块适配器实现 (EC801E-CN)
 * @author kerwincui
 * @date 2026-06-02
 */

#include "network/CellularAdapter.h"

#if FASTBEE_ENABLE_CELLULAR

#include "systems/LoggerSystem.h"
#include <SSLClient.h>
#include <ctype.h>
#include <new>

namespace {

struct ScopedBoolFlag {
    bool& flag;
    bool previous;
    explicit ScopedBoolFlag(bool& target) : flag(target), previous(target) {
        flag = true;
    }
    ~ScopedBoolFlag() {
        flag = previous;
    }
};

bool hasAssignedPdpAddress(const String& response, uint8_t cid) {
    String needle = String("+CGPADDR: ") + String(cid);
    int lineStart = response.indexOf(needle);
    if (lineStart < 0) {
        return false;
    }

    int quoteStart = response.indexOf('"', lineStart);
    int quoteEnd = quoteStart >= 0 ? response.indexOf('"', quoteStart + 1) : -1;
    if (quoteStart < 0 || quoteEnd <= quoteStart + 1) {
        return false;
    }

    String ip = response.substring(quoteStart + 1, quoteEnd);
    ip.trim();
    return !ip.isEmpty() && ip != "0.0.0.0" && ip != "::" && ip != "0:0:0:0:0:0:0:0";
}

bool hasUsableLocalIpv4(const IPAddress& ip) {
    return !(ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) &&
           !(ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255);
}

}

class CellularTrackedClient : public Client {
public:
    CellularTrackedClient(Client* inner, bool& busyFlag, bool& networkReady)
        : _inner(inner), _busy(busyFlag), _networkReady(networkReady) {}

    int connect(IPAddress ip, uint16_t port) override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        return _inner ? _inner->connect(ip, port) : 0;
    }

    int connect(const char* host, uint16_t port) override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        return _inner ? _inner->connect(host, port) : 0;
    }

    size_t write(uint8_t value) override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        return _inner ? _inner->write(value) : 0;
    }

    size_t write(const uint8_t* buf, size_t size) override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        return _inner ? _inner->write(buf, size) : 0;
    }

    int available() override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        return _inner ? _inner->available() : 0;
    }

    int read() override {
        if (!_networkReady) return -1;
        ScopedBoolFlag busy(_busy);
        return _inner ? _inner->read() : -1;
    }

    int read(uint8_t* buf, size_t size) override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        return _inner ? _inner->read(buf, size) : 0;
    }

    int peek() override {
        if (!_networkReady) return -1;
        ScopedBoolFlag busy(_busy);
        return _inner ? _inner->peek() : -1;
    }

    void flush() override {
        ScopedBoolFlag busy(_busy);
        if (_inner) _inner->flush();
    }

    void stop() override {
        ScopedBoolFlag busy(_busy);
        if (_inner) _inner->stop();
    }

    uint8_t connected() override {
        return (_networkReady && _inner) ? _inner->connected() : 0;
    }

    operator bool() override {
        return connected();
    }

private:
    Client* _inner;
    bool& _busy;
    bool& _networkReady;
};

class CellularSoftwareTlsClient : public SSLClient {
public:
    CellularSoftwareTlsClient(Client* inner, bool& busyFlag, bool& networkReady)
        : SSLClient(inner), _busy(busyFlag), _networkReady(networkReady) {
        prepareHandshake();
        setHandshakeTimeout(120);
        setTimeout(120000);
    }

    int connect(IPAddress ip, uint16_t port) override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        prepareHandshake();
        return SSLClient::connect(ip, port);
    }

    int connect(const char* host, uint16_t port) override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        prepareHandshake();
        return SSLClient::connect(host, port);
    }

    size_t write(uint8_t value) override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        return SSLClient::write(value);
    }

    size_t write(const uint8_t* buf, size_t size) override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        return SSLClient::write(buf, size);
    }

    int available() override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        return SSLClient::available();
    }

    int read() override {
        if (!_networkReady) return -1;
        ScopedBoolFlag busy(_busy);
        return SSLClient::read();
    }

    int read(uint8_t* buf, size_t size) override {
        if (!_networkReady) return 0;
        ScopedBoolFlag busy(_busy);
        return SSLClient::read(buf, size);
    }

    int peek() override {
        if (!_networkReady) return -1;
        ScopedBoolFlag busy(_busy);
        return SSLClient::peek();
    }

    void stop() override {
        ScopedBoolFlag busy(_busy);
        SSLClient::stop();
    }

    uint8_t connected() override {
        return _networkReady ? SSLClient::connected() : 0;
    }

    operator bool() override {
        return connected();
    }

private:
    void prepareHandshake() {
        setCACertBundle(nullptr);
        setInsecure();
    }

    bool& _busy;
    bool& _networkReady;
};

QuectelSslClient::QuectelSslClient(HardwareSerial& serial,
                                   uint8_t contextId,
                                   uint8_t connectId,
                                   uint8_t sslContextId)
    : _serial(&serial),
      _contextId(contextId),
      _connectId(connectId),
      _sslContextId(sslContextId) {
    resetRx();
}

void QuectelSslClient::setApn(const String& apn) {
    _apn = apn;
}

int QuectelSslClient::connect(IPAddress ip, uint16_t port) {
    char host[24];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return connect(host, port);
}

int QuectelSslClient::connect(const char* host, uint16_t port) {
    if (!_serial || !host || !*host) return 0;
    ScopedBoolFlag busy(_busy);

    stop();
    resetRx();
    _host = sanitizeToken(String(host));
    _port = port;
    _connected = false;
    _hasPendingData = false;

    ets_printf("[4G-QSSL] Opening EC801E TLS socket host=%s port=%u ctx=%u sslctx=%u client=%u\n",
               _host.c_str(), port, _contextId, _sslContextId, _connectId);

    if (!ensureInternetContext()) {
        ets_printf("[4G-QSSL] QIACT context is not active\n");
        return 0;
    }
    if (!configureSsl()) {
        ets_printf("[4G-QSSL] QSSLCFG failed\n");
        return 0;
    }

    char cmd[192];
    snprintf(cmd, sizeof(cmd), "AT+QSSLOPEN=%u,%u,%u,\"%s\",%u,0",
             _contextId, _sslContextId, _connectId, _host.c_str(), port);
    drainInput();
    _serial->print(cmd);
    _serial->print("\r\n");

    bool opened = waitForOpenResult(90000);
    _connected = opened;
    ets_printf("[4G-QSSL] QSSLOPEN %s\n", opened ? "OK" : "FAILED");
    return opened ? 1 : 0;
}

size_t QuectelSslClient::write(uint8_t value) {
    return write(&value, 1);
}

size_t QuectelSslClient::write(const uint8_t* buf, size_t size) {
    if (!_serial || !_connected || !buf || size == 0) return 0;
    ScopedBoolFlag busy(_busy);

    size_t sent = 0;
    while (sent < size && _connected) {
        size_t chunk = size - sent;
        if (chunk > 1024) chunk = 1024;

        char cmd[40];
        snprintf(cmd, sizeof(cmd), "AT+QSSLSEND=%u,%u", _connectId, (unsigned)chunk);
        drainInput();
        _serial->print(cmd);
        _serial->print("\r\n");
        if (!waitForPrompt(5000)) {
            ets_printf("[4G-QSSL] QSSLSEND prompt timeout\n");
            _connected = false;
            break;
        }

        _serial->write(buf + sent, chunk);
        _serial->flush();

        if (!waitForSendDone(20000)) {
            ets_printf("[4G-QSSL] QSSLSEND failed\n");
            _connected = false;
            break;
        }
        sent += chunk;
    }
    return sent;
}

int QuectelSslClient::available() {
    int buffered = bufferedAvailable();
    if (buffered > 0) return buffered;
    if (!_connected) return 0;

    unsigned long now = millis();
    if (!_hasPendingData && (now - _lastPollMs) < 120) return 0;
    _lastPollMs = now;

    fetchRx(RX_BUFFER_SIZE, _hasPendingData ? 1000 : 250);
    _hasPendingData = false;
    return bufferedAvailable();
}

int QuectelSslClient::read() {
    uint8_t value = 0;
    return read(&value, 1) == 1 ? value : -1;
}

int QuectelSslClient::read(uint8_t* buf, size_t size) {
    if (!buf || size == 0) return 0;

    size_t copied = 0;
    while (copied < size) {
        int buffered = bufferedAvailable();
        if (buffered <= 0) {
            if (!_connected || !fetchRx(size - copied, 1000)) break;
            buffered = bufferedAvailable();
            if (buffered <= 0) break;
        }

        size_t take = size - copied;
        if (take > static_cast<size_t>(buffered)) take = static_cast<size_t>(buffered);
        memcpy(buf + copied, _rxBuffer + _rxPos, take);
        _rxPos += take;
        copied += take;
        if (_rxPos >= _rxLen) resetRx();
    }
    return static_cast<int>(copied);
}

int QuectelSslClient::peek() {
    if (available() <= 0) return -1;
    return _rxBuffer[_rxPos];
}

void QuectelSslClient::flush() {
    if (_serial) _serial->flush();
}

void QuectelSslClient::stop() {
    if (!_serial) return;
    ScopedBoolFlag busy(_busy);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+QSSLCLOSE=%u", _connectId);
    String discard;
    sendCommandCollect(cmd, discard, 5000);
    _connected = false;
    _hasPendingData = false;
    resetRx();
}

uint8_t QuectelSslClient::connected() {
    return _connected ? 1 : 0;
}

QuectelSslClient::operator bool() {
    return connected();
}

bool QuectelSslClient::configureSsl() {
    char cmd[72];
    snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"seclevel\",%u,0", _sslContextId);
    bool seclevel = sendCommandExpectOk(cmd, 10000);

    snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"sslversion\",%u,4", _sslContextId);
    bool version = sendCommandExpectOk(cmd, 10000);
    if (!version) {
        snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"sslversion\",%u,3", _sslContextId);
        version = sendCommandExpectOk(cmd, 10000);
    }

    snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"ignorelocaltime\",%u,1", _sslContextId);
    String discard;
    sendCommandCollect(cmd, discard, 5000);
    snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"sni\",%u,1", _sslContextId);
    sendCommandCollect(cmd, discard, 5000);
    snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"ciphersuite\",%u,0XFFFF", _sslContextId);
    sendCommandCollect(cmd, discard, 5000);

    return seclevel && version;
}

bool QuectelSslClient::ensureInternetContext() {
    if (queryInternetContextActive()) return true;

    String apn = sanitizeToken(_apn);
    if (apn.isEmpty()) apn = "CMNET";

    char cmd[160];
    snprintf(cmd, sizeof(cmd), "AT+QICSGP=%u,1,\"%s\",\"\",\"\",1", _contextId, apn.c_str());
    bool configured = sendCommandExpectOk(cmd, 10000);

    snprintf(cmd, sizeof(cmd), "AT+QIACT=%u", _contextId);
    bool activated = sendCommandExpectOk(cmd, 150000);

    if (!activated && queryInternetContextActive()) return true;
    return configured && (activated || queryInternetContextActive());
}

bool QuectelSslClient::queryInternetContextActive() {
    String response;
    if (!sendCommandCollect("AT+QIACT?", response, 10000)) return false;

    String needle = String("+QIACT: ") + String(_contextId) + ",";
    int pos = response.indexOf(needle);
    if (pos < 0) return false;
    int firstComma = response.indexOf(',', pos);
    int secondComma = firstComma >= 0 ? response.indexOf(',', firstComma + 1) : -1;
    if (secondComma < 0) return false;
    String state = response.substring(firstComma + 1, secondComma);
    state.trim();
    return state == "1";
}

bool QuectelSslClient::sendCommandExpectOk(const char* cmd, uint32_t timeoutMs) {
    String response;
    bool ok = sendCommandCollect(cmd, response, timeoutMs);
    if (!ok) {
        ets_printf("[4G-QSSL] AT failed: %s resp=%s\n", cmd, response.c_str());
    }
    return ok;
}

bool QuectelSslClient::sendCommandCollect(const char* cmd, String& response, uint32_t timeoutMs) {
    if (!_serial || !cmd) return false;
    response = "";
    drainInput();
    _serial->print(cmd);
    _serial->print("\r\n");

    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        String line = readLine(250);
        if (line.isEmpty()) continue;
        if (line == cmd) continue;
        handleUrc(line);
        response += line;
        response += "\n";
        if (line == "OK") return true;
        if (line == "ERROR" || line.startsWith("+CME ERROR") || line.startsWith("+CMS ERROR")) return false;
    }
    return false;
}

bool QuectelSslClient::waitForPrompt(uint32_t timeoutMs) {
    String seen;
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (_serial->available()) {
            char c = static_cast<char>(_serial->read());
            if (c == '>') return true;
            seen += c;
            if (seen.endsWith("ERROR\r\n") || seen.endsWith("+CME ERROR")) return false;
            if (seen.length() > 96) seen = seen.substring(seen.length() - 48);
        }
        delay(5);
    }
    return false;
}

bool QuectelSslClient::waitForSendDone(uint32_t timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        String line = readLine(250);
        if (line.isEmpty()) continue;
        handleUrc(line);
        if (line == "SEND OK" || line == "OK") return true;
        if (line == "ERROR" || line.startsWith("+CME ERROR") || line.startsWith("+CMS ERROR")) return false;
    }
    return false;
}

bool QuectelSslClient::waitForOpenResult(uint32_t timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        String line = readLine(500);
        if (line.isEmpty()) continue;
        handleUrc(line);
        if (line == "ERROR" || line.startsWith("+CME ERROR") || line.startsWith("+CMS ERROR")) return false;
        if (!line.startsWith("+QSSLOPEN:")) continue;

        int err = -1;
        if (parseLastInteger(line, err) && err == 0) return true;
        ets_printf("[4G-QSSL] QSSLOPEN URC: %s\n", line.c_str());
        return false;
    }
    return false;
}

bool QuectelSslClient::fetchRx(size_t wanted, uint32_t timeoutMs) {
    if (!_serial || !_connected) return false;
    ScopedBoolFlag busy(_busy);
    resetRx();

    size_t request = wanted;
    if (request == 0 || request > RX_BUFFER_SIZE) request = RX_BUFFER_SIZE;

    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+QSSLRECV=%u,%u", _connectId, (unsigned)request);
    drainInput();
    _serial->print(cmd);
    _serial->print("\r\n");

    unsigned long start = millis();
    int rxLen = -1;
    while (millis() - start < timeoutMs) {
        String line = readLine(100);
        if (line.isEmpty()) continue;
        handleUrc(line);
        if (line.startsWith("+QSSLRECV:")) {
            int len = -1;
            if (parseLastInteger(line, len)) rxLen = len;
            break;
        }
        if (line == "ERROR" || line.startsWith("+CME ERROR") || line.startsWith("+CMS ERROR")) return false;
    }

    if (rxLen <= 0) {
        unsigned long endStart = millis();
        while (millis() - endStart < 300) {
            String line = readLine(50);
            if (line.isEmpty()) continue;
            handleUrc(line);
            if (line == "OK" || line == "ERROR") break;
        }
        return rxLen == 0;
    }

    size_t toRead = static_cast<size_t>(rxLen);
    if (toRead > RX_BUFFER_SIZE) toRead = RX_BUFFER_SIZE;

    size_t got = 0;
    unsigned long dataStart = millis();
    while (got < toRead && millis() - dataStart < timeoutMs) {
        if (_serial->available()) {
            _rxBuffer[got++] = static_cast<uint8_t>(_serial->read());
        } else {
            delay(2);
        }
    }
    _rxPos = 0;
    _rxLen = got;

    unsigned long endStart = millis();
    while (millis() - endStart < 500) {
        String line = readLine(50);
        if (line.isEmpty()) continue;
        handleUrc(line);
        if (line == "OK" || line == "ERROR") break;
    }
    return got > 0;
}

int QuectelSslClient::bufferedAvailable() const {
    return _rxLen > _rxPos ? static_cast<int>(_rxLen - _rxPos) : 0;
}

void QuectelSslClient::resetRx() {
    _rxPos = 0;
    _rxLen = 0;
}

void QuectelSslClient::drainInput(uint32_t maxMs) {
    if (!_serial) return;
    unsigned long start = millis();
    while (millis() - start < maxMs) {
        bool hadData = false;
        while (_serial->available()) {
            _serial->read();
            hadData = true;
        }
        if (!hadData) delay(2);
    }
}

String QuectelSslClient::readLine(uint32_t timeoutMs) {
    String line;
    line.reserve(96);
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (_serial && _serial->available()) {
            char c = static_cast<char>(_serial->read());
            if (c == '\r') continue;
            if (c == '\n') {
                line.trim();
                if (!line.isEmpty()) return line;
                continue;
            }
            line += c;
            if (line.length() > 180) {
                line.trim();
                return line;
            }
        }
        delay(2);
    }
    line.trim();
    return line;
}

void QuectelSslClient::handleUrc(const String& line) {
    if (line.indexOf("+QSSLURC:") < 0) return;
    if (line.indexOf("\"recv\"") >= 0) {
        _hasPendingData = true;
    } else if (line.indexOf("\"closed\"") >= 0 || line.indexOf("\"pdpdeact\"") >= 0) {
        _connected = false;
    }
}

String QuectelSslClient::sanitizeToken(String value) {
    value.replace("\"", "");
    value.replace("\r", "");
    value.replace("\n", "");
    value.trim();
    return value;
}

bool QuectelSslClient::parseLastInteger(const String& line, int& value) {
    int idx = static_cast<int>(line.length()) - 1;
    while (idx >= 0 && !isdigit(static_cast<unsigned char>(line[idx])) && line[idx] != '-') idx--;
    if (idx < 0) return false;
    int end = idx;
    while (idx >= 0 && (isdigit(static_cast<unsigned char>(line[idx])) || line[idx] == '-')) idx--;
    value = line.substring(idx + 1, end + 1).toInt();
    return true;
}

CellularAdapter::CellularAdapter() {}

CellularAdapter::~CellularAdapter() {
    disconnect();
    if (_softwareTlsClient) { delete _softwareTlsClient; _softwareTlsClient = nullptr; }
    if (_trackedClient) { delete _trackedClient; _trackedClient = nullptr; }
    if (_qsslClient) { delete _qsslClient; _qsslClient = nullptr; }
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
    _trackedClient = new (std::nothrow) CellularTrackedClient(_gsmClient, _clientBusy, _connected);
    Client* tlsBaseClient = _trackedClient ? static_cast<Client*>(_trackedClient) : static_cast<Client*>(_gsmClient);
    _softwareTlsClient = new (std::nothrow) CellularSoftwareTlsClient(tlsBaseClient, _clientBusy, _connected);
    if (_softwareTlsClient) {
        ets_printf("[4G] Software TLS client ready (TinyGSM TCP + ESP32 mbedTLS)\n");
    } else {
        ets_printf("[4G] WARNING: Software TLS client allocation failed; QSSL fallback only\n");
    }
    _qsslClient = new (std::nothrow) QuectelSslClient(*_serial, 1, 4, 1);
    if (_qsslClient) {
        _qsslClient->setApn(_apn);
    } else {
        ets_printf("[4G] WARNING: QuectelSslClient allocation failed\n");
    }

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
    if (_clientBusy) return _connected;
    if (_qsslClient && _qsslClient->isBusy()) return _connected;
    // EC801E status is validated through CGPADDR to avoid stale modem state.
    return _connected && checkPdpActive();
}

void CellularAdapter::disconnect() {
    if (_softwareTlsClient) _softwareTlsClient->stop();
    if (_trackedClient) _trackedClient->stop();
    if (_gsmClient) _gsmClient->stop();
    if (_qsslClient) _qsslClient->stop();
    if (_modem && _connected) {
        _modem->gprsDisconnect();
        LOG_INFO("CellularAdapter: GPRS disconnected");
    }
    _connected = false;
    _initialized = false;
    powerOff();
}

Client* CellularAdapter::getClient() {
    return _trackedClient ? static_cast<Client*>(_trackedClient) : nullptr;
}

Client* CellularAdapter::getSecureClient() {
    if (_softwareTlsClient) return static_cast<Client*>(_softwareTlsClient);
    if (_qsslClient) return static_cast<Client*>(_qsslClient);
    return getClient();
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
    if (_clientBusy) {
        ets_printf("[4G] Skipping PDP check while cellular client AT session is busy\n");
        return;
    }
    if (_qsslClient && _qsslClient->isBusy()) {
        ets_printf("[4G] Skipping PDP check while QSSL AT session is busy\n");
        return;
    }

    // EC801E periodic status is validated through CGPADDR.
    bool gprsOk = checkPdpActive();

    if (!gprsOk) {
        if (_connected) {
            _connected = false;
            if (_softwareTlsClient) _softwareTlsClient->stop();
            if (_trackedClient) _trackedClient->stop();
            if (_gsmClient) _gsmClient->stop();
            if (_qsslClient) _qsslClient->stop();
            ets_printf("[4G] GPRS connection lost (PDP no IP)\n");
        }
    } else if (!_connected) {
        _connected = true;
        ets_printf("[4G] GPRS connection restored\n");
    }
}

bool CellularAdapter::checkPdpActive() {
    String pip = readATResponse("AT+CGPADDR=1");
    // Response format: +CGPADDR: 1,"10.3.89.109" or +CGPADDR: 1,"2409:8a28::1".
    return hasAssignedPdpAddress(pip, 1);
}

bool CellularAdapter::reconnect() {
    if (!_initialized || !_modem) return false;

    LOG_INFO("CellularAdapter: Attempting reconnect...");
    if (_softwareTlsClient) _softwareTlsClient->stop();
    if (_trackedClient) _trackedClient->stop();
    if (_gsmClient) _gsmClient->stop();
    if (_qsslClient) _qsslClient->stop();
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
    if (checkPdpActive()) {
        IPAddress ip = localIP();
        if (hasUsableLocalIpv4(ip)) {
            ets_printf("[4G] PDP already active, IP: %s\n", ip.toString().c_str());
            _connected = true;
            return true;
        }
        ets_printf("[4G] PDP active but local IP invalid (%s), resetting context\n", ip.toString().c_str());
        _modem->gprsDisconnect();
        delay(1000);
    }

    // 激活 GPRS（最多重试 3 次，每次间隔递增）
    ets_printf("[4G] Connecting GPRS with APN: %s\n", _apn.c_str());
    for (int attempt = 1; attempt <= 3; attempt++) {
        ets_printf("[4G] GPRS connect attempt %d/3...\n", attempt);
        if (_modem->gprsConnect(_apn.c_str(), "", "")) {
            IPAddress ip = _modem->localIP();
            if (hasUsableLocalIpv4(ip) && checkPdpActive()) {
                _connected = true;
                LOGGER.infof("CellularAdapter: GPRS connected, IP: %s", ip.toString().c_str());
                return true;
            }
            if (hasUsableLocalIpv4(ip)) {
                ets_printf("[4G] GPRS connected but CGPADDR inactive, retrying\n");
            } else {
                ets_printf("[4G] GPRS connected but local IP invalid (%s), retrying\n", ip.toString().c_str());
            }
            _modem->gprsDisconnect();
            delay(1000);
        }
        // TinyGSM SIM7600 模式用 AT+CIPSTATUS 检查状态，EC801E 不支持
        // 直接检查 CGPADDR 判断 PDP 是否实际上已成功
        if (checkPdpActive()) {
            IPAddress ip = _modem->localIP();
            if (hasUsableLocalIpv4(ip)) {
                ets_printf("[4G] TinyGSM reported fail but PDP is active! IP: %s\n", ip.toString().c_str());
                _connected = true;
                return true;
            }
            ets_printf("[4G] PDP has CGPADDR but local IP invalid (%s), retrying\n", ip.toString().c_str());
            _modem->gprsDisconnect();
            delay(1000);
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
            if (checkPdpActive()) {
                IPAddress ip = _modem->localIP();
                if (hasUsableLocalIpv4(ip)) {
                    ets_printf("[4G] Manual PDP verified via CGPADDR, IP: %s\n", ip.toString().c_str());
                    _connected = true;
                    return true;
                }
                ets_printf("[4G] Manual PDP got invalid local IP (%s)\n", ip.toString().c_str());
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
                if (!result.isEmpty()) result += "\n";
                result += line;
            }
        } else {
            delay(50);
        }
    }
    return result;
}

#endif // FASTBEE_ENABLE_CELLULAR
