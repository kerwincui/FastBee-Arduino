#include "protocols/ModbusReadDiagnostics.h"
#include "protocols/ModbusUartRxMode.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#define LOG_INFOF(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
namespace Protocols { constexpr size_t MODBUS_BUFFER_SIZE = 256; }
namespace FastBee { namespace MemoryBudget { unsigned dramFree() { return 0; } } }
enum OneShotError { ONESHOT_SUCCESS, ONESHOT_TIMEOUT, ONESHOT_CRC_ERROR, ONESHOT_EXCEPTION, ONESHOT_NOT_INITIALIZED };
constexpr unsigned SERIAL_8N1 = 0;
constexpr uint8_t MODBUS_EX_ILLEGAL_DATA_VALUE = 3;
static unsigned long now;
unsigned long millis() { return now; }
void delay(unsigned ms) { now += ms; }
void yield() {}
void vTaskDelay(unsigned ticks) { now += ticks; }
bool isReadFunctionCode(uint8_t fc) { return fc >= 1 && fc <= 4; }
unsigned long modbusRtuFrameGapMs(uint32_t) { return 4; }

struct Serial {
    std::vector<std::vector<uint8_t>> replies;
    std::vector<uint8_t> current;
    unsigned sent = 0;
    size_t pos = 0;
    bool shortWrite = false;
    bool ready = true, failBegin = false;
    uint32_t baud = 9600;
    int beginCalls = 0, endCalls = 0;
    int rxPin = -1, txPin = -1;
    unsigned timeout = 0;
    unsigned firstByteDelayMs = 0, interByteDelayMs = 0, flushDelayMs = 0;
    unsigned long responseAt = 0;
    explicit operator bool() const { return ready; }
    uint32_t baudRate() { return ready ? baud : 0; }
    void end() { ready = false; ++endCalls; }
    void begin(uint32_t speed, unsigned, int rx, int tx) {
        ++beginCalls; ready = !failBegin; baud = speed; rxPin = rx; txPin = tx;
    }
    void setTimeout(unsigned value) { timeout = value; }
    int available() {
        return pos < current.size() && now >= responseAt + pos * interByteDelayMs;
    }
    int read() { return available() ? current[pos++] : -1; }
    size_t write(const uint8_t*, size_t length) {
        current = sent < replies.size() ? replies[sent] : std::vector<uint8_t>{};
        responseAt = now + firstByteDelayMs;
        pos = 0; ++sent;
        return shortWrite ? length - 1 : length;
    }
    void flush() { now += flushDelayMs; }
};

struct ModbusHandler {
    Serial serial;
    Serial* modbusSerial = &serial;
    bool _controlPending = false, defer = false;
    std::string _lastTxHex, _lastRxHex;
    struct Config {
        uint32_t baudRate = 9600;
        int rxPin = 43, txPin = 44;
        unsigned responseTimeout = 1000;
        struct Master { uint16_t responseTimeout = 12; uint8_t maxRetries = 0; } master;
    } config;
    bool _shouldDeferSlaveRequest(uint8_t, uint32_t) { return defer; }
    void _recordSlaveOutcome(uint8_t, OneShotError, uint32_t) {}
    void setTransmitMode(bool) {}
    bool initializeSerial();
    uint16_t calculateCRC(const uint8_t*, size_t);
    bool validateFrame(const uint8_t*, size_t);
    OneShotError _executeModbusTransactionRaw(const uint8_t*,size_t,uint8_t,uint8_t*,size_t,size_t&,
                                             uint8_t&,uint16_t,uint8_t,bool,ModbusReadDiagnostics*);
};
#include "modbus_read_diagnostics_production.inc"

std::vector<uint8_t> frame(ModbusHandler& bus, std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> result(bytes);
    const uint16_t crc = bus.calculateCRC(result.data(), result.size());
    result.push_back(crc & 255); result.push_back(crc >> 8);
    return result;
}

int main() {
    ModbusHandler initialized;
    assert(initialized.initializeSerial());
    assert(initialized.serial.ready && initialized.serial.beginCalls == 1 && initialized.serial.endCalls == 1);
    assert(initialized.serial.rxPin == 43 && initialized.serial.txPin == 44);
    assert(initialized.serial.baud == 9600 && initialized.serial.timeout == 1000);
    ModbusHandler invalidPins;
    invalidPins.config.rxPin = invalidPins.config.txPin;
    assert(!invalidPins.initializeSerial());
    assert(!invalidPins.serial.ready && invalidPins.serial.sent == 0);
    ModbusHandler failedInit; failedInit.serial.failBegin = true;
    assert(!failedInit.initializeSerial() && failedInit.serial.timeout == 0);
    ModbusHandler nullInit; nullInit.modbusSerial = nullptr;
    assert(!nullInit.initializeSerial());
    const uint8_t request[] = {8,3,0,5,0,1,0x94,0x92};
    uint8_t raw[256]{}, exception = 0;
    size_t length = 0;
    ModbusReadDiagnostics trace;
    auto run = [&](ModbusHandler& bus, uint8_t retries = 0, size_t capacity = 256) {
        return bus._executeModbusTransactionRaw(request,8,8,raw,capacity,length,exception,12,retries,false,&trace);
    };
    ModbusHandler silent;
    assert(run(silent,2) == ONESHOT_TIMEOUT);
    assert(trace.attempts==3 && trace.txWritten==8 && trace.totalRxBuffered==0);
    assert(std::string(trace.reason)=="NO_RESPONSE_BYTES" && trace.rxPrefixLength==0);
    assert(trace.txPrefixLength==8 && !memcmp(trace.txPrefix,request,8));
    assert(trace.uartReady && trace.uartBaudRate == 9600);

    ModbusHandler stopped; stopped.serial.ready = false;
    assert(run(stopped) == ONESHOT_NOT_INITIALIZED && stopped.serial.sent == 0);
    assert(std::string(trace.reason) == "UART_DRIVER_NOT_READY");
    assert(!trace.uartReady && trace.uartBaudRate == 0 && trace.attempts == 0);
    assert(run(nullInit) == ONESHOT_NOT_INITIALIZED && nullInit.serial.sent == 0);

    ModbusHandler shortWrite; shortWrite.serial.shortWrite=true;
    assert(run(shortWrite)==ONESHOT_TIMEOUT);
    assert(std::string(trace.reason)=="SHORT_WRITE" && trace.txWritten==7 && trace.attempts==1);

    ModbusHandler shortFrame; shortFrame.serial.replies={{8,3}};
    assert(run(shortFrame)==ONESHOT_CRC_ERROR);
    assert(std::string(trace.reason)=="FRAME_TOO_SHORT" && trace.rxBuffered==2 && !trace.crcValid);

    ModbusHandler corrupt; corrupt.serial.replies={{8,3,2,0,255,0,0}};
    assert(run(corrupt)==ONESHOT_CRC_ERROR);
    assert(std::string(trace.reason)=="CRC_INVALID" && trace.rxPrefixLength==7 && !trace.crcValid);

    ModbusHandler wrong; wrong.serial.replies={frame(wrong,{9,3,2,0,255})};
    assert(run(wrong)==ONESHOT_CRC_ERROR);
    assert(std::string(trace.reason)=="SLAVE_ADDRESS_MISMATCH" && trace.crcValid && !trace.addressMatched);

    ModbusHandler except; except.serial.replies={frame(except,{8,0x83,2})};
    assert(run(except,2)==ONESHOT_EXCEPTION && exception==2 && trace.attempts==1);
    assert(std::string(trace.reason)=="MODBUS_EXCEPTION" && trace.crcValid && trace.addressMatched);

    ModbusHandler good; good.serial.replies={frame(good,{8,3,2,0,255})};
    assert(run(good)==ONESHOT_SUCCESS && length==7);
    assert(trace.crcValid && trace.addressMatched && std::string(trace.reason)=="FRAME_VALIDATED");
    const auto retained=trace;
    assert(run(silent)==ONESHOT_TIMEOUT && trace.rxPrefixLength==0 && !trace.crcValid);
    assert(retained.rxPrefixLength==7 && retained.crcValid);

    // Model asynchronous UART delivery without changing the production loop.
    ModbusHandler fragmented;
    fragmented.serial.firstByteDelayMs = 2;
    fragmented.serial.interByteDelayMs = 1;
    fragmented.serial.replies = {frame(fragmented, {8,3,2,0,255})};
    assert(run(fragmented) == ONESHOT_SUCCESS && length == 7);
    assert(trace.attempts == 1 && trace.totalRxBuffered == 7 && trace.crcValid);

    ModbusHandler bufferedDuringFlush;
    bufferedDuringFlush.serial.flushDelayMs = 9;
    bufferedDuringFlush.serial.replies = {frame(bufferedDuringFlush, {8,3,2,0,255})};
    assert(run(bufferedDuringFlush) == ONESHOT_SUCCESS && length == 7);
    assert(trace.rxBuffered == 7 && trace.addressMatched && trace.crcValid);

    ModbusHandler recovered;
    recovered.serial.firstByteDelayMs = 2;
    recovered.serial.replies = {{}, frame(recovered, {8,3,2,0,255})};
    assert(run(recovered, 1) == ONESHOT_SUCCESS && length == 7);
    assert(trace.attempts == 2 && trace.totalRxBuffered == 7 && trace.rxBuffered == 7);
    assert(trace.crcValid && std::string(trace.reason) == "FRAME_VALIDATED");

    ModbusHandler mixed; mixed.serial.replies={{8,3,2,0,255,0,0},{}};
    assert(run(mixed,1)==ONESHOT_TIMEOUT);
    assert(trace.totalRxBuffered==7 && trace.rxBuffered==0 && !trace.crcValid);

    ModbusHandler overflow; overflow.serial.replies={std::vector<uint8_t>(300,8)};
    assert(run(overflow)==ONESHOT_CRC_ERROR && trace.rxOverflow && trace.rxBuffered==256);
    assert(trace.rxPrefixLength==sizeof(trace.rxPrefix) && std::string(trace.reason)=="RX_OVERFLOW");

    ModbusHandler skipped; skipped.defer=true;
    assert(run(skipped)==ONESHOT_TIMEOUT && trace.attempts==0 && skipped.serial.sent==0);
    assert(std::string(trace.reason)=="SLAVE_BACKOFF" && trace.totalRxBuffered==0);

    ModbusHandler small; small.serial.replies={frame(small,{8,3,2,0,255})};
    assert(run(small,0,3)==ONESHOT_EXCEPTION && length==0);
    assert(std::string(trace.reason)=="RESPONSE_CAPACITY_EXCEEDED");
    assert(small._executeModbusTransactionRaw(nullptr,8,8,raw,256,length,exception,12,0,false,&trace)==ONESHOT_EXCEPTION);
    assert(trace.attempts==0 && std::string(trace.reason)=="INVALID_REQUEST");
    std::puts("UART initialization, transport diagnostics, and 3 receive-timing regressions passed; no device access");
}
