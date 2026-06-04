/**
 * @file LoRaAdapter.cpp
 * @brief LoRa 网关透传适配器实现 (E22-400T22D)
 * @author kerwincui
 * @date 2026-06-02
 */

#include "network/LoRaAdapter.h"

#if FASTBEE_ENABLE_LORA

#include "systems/LoggerSystem.h"

// ============ LoRaClient 实现 ============

LoRaClient::LoRaClient() : _connected(false), _rxHead(0), _rxTail(0) {
    memset(_rxBuffer, 0, sizeof(_rxBuffer));
}

void LoRaClient::setSerial(HardwareSerial* serial) {
    _serial = serial;
}

int LoRaClient::connect(IPAddress ip, uint16_t port) {
    // LoRa 透传模式不需要真正的 TCP 连接
    // 只要串口可用就认为"已连接"
    if (_serial) {
        _connected = true;
        return 1;
    }
    return 0;
}

int LoRaClient::connect(const char* host, uint16_t port) {
    return connect(IPAddress(0, 0, 0, 0), port);
}

size_t LoRaClient::write(uint8_t b) {
    return write(&b, 1);
}

size_t LoRaClient::write(const uint8_t* buf, size_t size) {
    if (!_serial || !_connected) return 0;

    // E22-400T22D 单次最大 240 字节，需分帧发送
    size_t totalSent = 0;
    while (totalSent < size) {
        size_t chunkSize = min((size_t)LORA_MAX_FRAME_SIZE, size - totalSent);
        size_t sent = _serial->write(buf + totalSent, chunkSize);
        totalSent += sent;

        // 如果分帧发送，帧间等待确保 LoRa 模块处理完毕
        if (totalSent < size) {
            delay(50);  // 帧间间隔（E22 透传模式建议 > 20ms）
        }
    }
    return totalSent;
}

int LoRaClient::available() {
    pollSerial();
    if (_rxHead >= _rxTail) {
        return _rxHead - _rxTail;
    }
    return LORA_RX_BUFFER_SIZE - _rxTail + _rxHead;
}

int LoRaClient::read() {
    if (available() == 0) return -1;
    uint8_t b = _rxBuffer[_rxTail];
    _rxTail = (_rxTail + 1) % LORA_RX_BUFFER_SIZE;
    return b;
}

int LoRaClient::read(uint8_t* buf, size_t size) {
    int avail = available();
    if (avail == 0) return 0;

    size_t toRead = min((size_t)avail, size);
    for (size_t i = 0; i < toRead; i++) {
        buf[i] = _rxBuffer[_rxTail];
        _rxTail = (_rxTail + 1) % LORA_RX_BUFFER_SIZE;
    }
    return toRead;
}

int LoRaClient::peek() {
    if (available() == 0) return -1;
    return _rxBuffer[_rxTail];
}

void LoRaClient::flush() {
    if (_serial) _serial->flush();
}

void LoRaClient::stop() {
    _connected = false;
    _rxHead = 0;
    _rxTail = 0;
}

uint8_t LoRaClient::connected() {
    return _connected ? 1 : 0;
}

LoRaClient::operator bool() {
    return _connected;
}

void LoRaClient::pollSerial() {
    if (!_serial) return;

    while (_serial->available()) {
        size_t nextHead = (_rxHead + 1) % LORA_RX_BUFFER_SIZE;
        if (nextHead == _rxTail) break;  // 缓冲区满
        _rxBuffer[_rxHead] = _serial->read();
        _rxHead = nextHead;
    }
}

// ============ LoRaAdapter 实现 ============

LoRaAdapter::LoRaAdapter() {}

LoRaAdapter::~LoRaAdapter() {
    disconnect();
}

bool LoRaAdapter::begin(const WiFiConfig& config) {
    if (_initialized) {
        LOG_WARNING("LoRaAdapter: Already initialized");
        return true;
    }

    _pinConfig = config.lora;

    LOG_INFO("LoRaAdapter: Initializing E22-400T22D LoRa module...");
    LOGGER.infof("LoRaAdapter: UART pins - TX:%d, RX:%d, M1:%d, Baud:%d",
                 _pinConfig.txPin, _pinConfig.rxPin, _pinConfig.m1Pin, _pinConfig.baudRate);

    // 设置 M1 引脚为透传模式（低电平）
    setTransparentMode();

    // 初始化串口
    _serial = &Serial2;
    _serial->begin(_pinConfig.baudRate, SERIAL_8N1, _pinConfig.rxPin, _pinConfig.txPin);

    // 等待模块稳定
    delay(500);

    // 配置 LoRaClient
    _loraClient.setSerial(_serial);

    _initialized = true;
    LOG_INFO("LoRaAdapter: Initialized in transparent mode");
    return true;
}

bool LoRaAdapter::isConnected() const {
    // LoRa 透传模式：只要模块初始化成功就认为可用
    return _initialized;
}

void LoRaAdapter::disconnect() {
    if (_serial) {
        _serial->end();
    }
    _loraClient.stop();
    _initialized = false;
    LOG_INFO("LoRaAdapter: Disconnected");
}

Client* LoRaAdapter::getClient() {
    return &_loraClient;
}

String LoRaAdapter::getStatusString() const {
    if (!_initialized) return "uninitialized";
    return "ready";
}

bool LoRaAdapter::isTransparentMode() const {
    return _transparentMode;
}

uint16_t LoRaAdapter::getAddress() const {
    return _address;
}

String LoRaAdapter::getFrequencyString() const {
    if (_frequency == 0) return "433MHz";  // E22-400T22D 默认 433MHz
    char buf[16];
    snprintf(buf, sizeof(buf), "%dMHz", _frequency);
    return String(buf);
}

String LoRaAdapter::getAirRateString() const {
    switch (_airRate) {
        case 0: return "0.3kbps";
        case 1: return "1.2kbps";
        case 2: return "2.4kbps";
        case 3: return "4.8kbps";
        case 4: return "9.6kbps";
        case 5: return "19.2kbps";
        default: return "auto";
    }
}

uint8_t LoRaAdapter::getChannel() const {
    return _channel;
}

void LoRaAdapter::update() {
    // LoRa 透传模式暂无需周期性维护
}

void LoRaAdapter::setTransparentMode() {
    if (_pinConfig.m1Pin >= 0) {
        pinMode(_pinConfig.m1Pin, OUTPUT);
        digitalWrite(_pinConfig.m1Pin, LOW);  // M1=LOW: 透传模式
        LOG_INFO("LoRaAdapter: M1 pin set LOW (transparent mode)");
        delay(100);  // 等待模式切换完成
    }
}

#endif // FASTBEE_ENABLE_LORA
