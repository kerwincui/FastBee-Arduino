#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_IR_REMOTE

#include "peripherals/drivers/IRRemoteDriver.h"
#include "core/PeripheralExecution.h"
#include "core/PeriphExecManager.h"
#include "systems/LoggerSystem.h"

#include <IRrecv.h>
#include <IRutils.h>

IRRemoteDriver& IRRemoteDriver::getInstance() {
    static IRRemoteDriver instance;
    return instance;
}

IRRemoteDriver::~IRRemoteDriver() {
    stop();
}

bool IRRemoteDriver::begin(uint8_t recvPin, uint16_t bufferSize) {
    if (_initialized) return true;

    IRrecv* recv = new IRrecv(recvPin, bufferSize, 50, true);  // 50us timeout, save buffer
    recv->setUnknownThreshold(12);  // 未知协议最小脉冲数
    recv->enableIRIn();

    _irRecv = recv;
    _results = new decode_results();
    _initialized = true;
    _lastCode = "";
    _lastProtocol = "";
    _lastValue = 0;

    LOGGER.infof("[IR] Receiver initialized on pin %d (buffer=%d)", recvPin, bufferSize);
    return true;
}

void IRRemoteDriver::check() {
    if (!_initialized || !_irRecv || !_results) return;

    IRrecv* recv = static_cast<IRrecv*>(_irRecv);
    decode_results* results = static_cast<decode_results*>(_results);

    if (recv->decode(results)) {
        unsigned long now = millis();

        // 获取协议名称
        String protocol = typeToString(results->decode_type);
        uint64_t value = results->value;

        // 过滤重复码（NEC repeat = 0xFFFFFFFFFFFFFFFF）
        if (value == 0xFFFFFFFFFFFFFFFF || value == 0) {
            recv->resume();
            return;
        }

        // 防抖：同一编码在最小间隔内不重复触发
        if (now - _lastEventTime < MIN_EVENT_INTERVAL_MS &&
            value == _lastValue) {
            recv->resume();
            return;
        }

        // 构建编码字符串
        char hexBuf[20];
        snprintf(hexBuf, sizeof(hexBuf), "0x%08llX", (unsigned long long)value);

        _lastProtocol = protocol;
        _lastValue = value;
        _lastCode = protocol + ":" + String(hexBuf);
        _lastEventTime = now;

        LOGGER.infof("[IR] Received: %s value=%s bits=%d",
                     protocol.c_str(), hexBuf, results->bits);

        // 触发红外编码接收事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        String eventData = "{\"protocol\":\"" + protocol +
                           "\",\"code\":\"" + String(hexBuf) +
                           "\",\"bits\":" + String(results->bits) + "}";
        PeriphExecManager::getInstance().dispatchEventMatchedRules("ir_code_received", eventData);
#endif

        recv->resume();
    }
}

void IRRemoteDriver::stop() {
    if (_irRecv) {
        IRrecv* recv = static_cast<IRrecv*>(_irRecv);
        recv->disableIRIn();
        delete recv;
        _irRecv = nullptr;
    }
    if (_results) {
        decode_results* res = static_cast<decode_results*>(_results);
        delete res;
        _results = nullptr;
    }
    _initialized = false;
    _lastCode = "";
    _lastProtocol = "";
    _lastValue = 0;
    LOGGER.info("[IR] Receiver stopped");
}

#endif // FASTBEE_ENABLE_IR_REMOTE
