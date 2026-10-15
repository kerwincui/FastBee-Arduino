// FW-P0-01 (2026-09-15 audit) regression fixture: OneShotResult::data holds
// exactly Protocols::MODBUS_ONESHOT_BUFFER_SIZE (32) slots, so a decoded read
// above 32 values once wrote past the structure. This harness compiles the
// current production receive path (readRegistersOnce, _readRangeInto,
// _sendOneShotRequestRaw, _executeModbusTransactionRaw and the CRC/frame
// helpers) and drives the boundary matrix: quantities 14/15/16/32/33/125,
// exact-length and byteCount gates, truncated/exception/FC-mismatch frames,
// FC01 bit expansion, the busy semaphore path, and stack canaries around the
// structured result and caller buffers.
#include "protocols/ModbusReadDiagnostics.h"
#include "core/MotorTravelInterlock.h"
#include "core/ModbusOrdinaryControl.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define LOG_INFO(...) ((void)0)
#define LOG_INFOF(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)
#define LOG_DEBUGF(...) ((void)0)

namespace Protocols {
constexpr size_t MODBUS_BUFFER_SIZE = 256;
constexpr size_t MODBUS_MAX_SUB_DEVICES = 4;
constexpr uint16_t MODBUS_MAX_REGISTERS_PER_READ = 125;
constexpr uint8_t MODBUS_ONESHOT_BUFFER_SIZE = 32;
}
// Keep the heap guard open so the production TX/RX debug capture stays active
// and the harness can assert it was not silently dropped.
namespace FastBee { namespace MemoryBudget { unsigned dramFree() { return 100000; } } }

enum OneShotError { ONESHOT_SUCCESS, ONESHOT_TIMEOUT, ONESHOT_CRC_ERROR, ONESHOT_EXCEPTION, ONESHOT_NOT_INITIALIZED, ONESHOT_BUSY, ONESHOT_TRAVEL_BLOCKED, ONESHOT_CONTROL_REJECTED };
enum ModbusException : uint8_t {
    MODBUS_EX_ILLEGAL_FUNCTION      = 0x01,
    MODBUS_EX_ILLEGAL_DATA_ADDRESS  = 0x02,
    MODBUS_EX_ILLEGAL_DATA_VALUE    = 0x03,
    MODBUS_EX_SLAVE_DEVICE_FAILURE  = 0x04
};
constexpr uint16_t MODBUS_BUS_ACQUIRE_TIMEOUT_MS = 2000;
#define pdMS_TO_TICKS(x) (x)
constexpr int pdTRUE = 1;
constexpr int pdFALSE = 0;

// Field-for-field copies of include/protocols/ModbusHandler.h so the fixture
// stores results in the production layout (32-slot data first, then scalars).
struct ModbusDelayReceipt {
    uint32_t instance = 0, configuration = 0;
    uint64_t writeSequence = 0;
    uint8_t slave = 0, function = 0;
    uint16_t address = 0;
};

struct OneShotResult {
    OneShotError error;
    uint16_t data[Protocols::MODBUS_ONESHOT_BUFFER_SIZE];
    uint16_t count;
    uint8_t exceptionCode;
    const char* travelError = nullptr;
    ModbusDelayReceipt delayReceipt;
    bool outputAttempted = false;
    const char* controlReason = nullptr;

    OneShotResult() : error(ONESHOT_NOT_INITIALIZED), count(0), exceptionCode(0) {
        memset(data, 0, sizeof(data));
    }
};

// Minimal String wrapper for the operations the extracted methods perform:
// reserve/length/isEmpty/c_str and += / = of char and C strings.
struct String {
    std::string value;
    String() = default;
    String(const char* text) : value(text ? text : "") {}
    void reserve(size_t size) { value.reserve(size); }
    size_t length() const { return value.length(); }
    bool isEmpty() const { return value.empty(); }
    const char* c_str() const { return value.c_str(); }
    String& operator=(const char* text) { value = text ? text : ""; return *this; }
    String& operator+=(const char* text) { value += (text ? text : ""); return *this; }
    String& operator+=(char ch) { value += ch; return *this; }
};

static unsigned long now;
unsigned long millis() { return now; }
int64_t esp_timer_get_time() { return static_cast<int64_t>(now) * 1000; }
void delay(unsigned ms) { now += ms; }
void yield() {}
void vTaskDelay(unsigned ticks) { now += ticks; }
bool isReadFunctionCode(uint8_t fc) { return fc >= 1 && fc <= 4; }
unsigned long modbusRtuFrameGapMs(uint32_t) { return 4; }

struct ModbusHandler;
int xSemaphoreTake(void* handle, unsigned long);
int xSemaphoreGive(void*);

struct Serial {
    std::vector<std::vector<uint8_t>> replies;
    std::vector<uint8_t> current;
    unsigned sent = 0;
    size_t pos = 0;
    bool shortWrite = false;
    bool ready = true;
    uint32_t baud = 9600;
    unsigned long responseAt = 0;
    unsigned firstByteDelayMs = 0, interByteDelayMs = 0, flushDelayMs = 0;
    explicit operator bool() const { return ready; }
    uint32_t baudRate() { return ready ? baud : 0; }
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
    const FastBee::ModbusWriteTicket* _ordinaryWriteTicket = nullptr;
    bool _ordinaryWriteAttempted = false;
    FastBee::MotorTravelInterlock<Protocols::MODBUS_MAX_SUB_DEVICES> _motorTravel;
    void* _oneShotSemaphore = this;
    bool semaphoreAvailable = true;
    const char* _lastTravelError = nullptr;
    volatile bool isOneShotReading = false;
    bool isInitialized = true;
    void _syncMotorTravel();
    Serial serial;
    Serial* modbusSerial = &serial;
    bool _controlPending = false, defer = false;
    String _lastTxHex, _lastRxHex;
    struct Config {
        uint32_t baudRate = 9600;
        int rxPin = 43, txPin = 44;
        unsigned responseTimeout = 1000;
        struct Master {
            uint16_t responseTimeout = 12; uint8_t maxRetries = 0, deviceCount = 0;
            struct Device {
                char deviceType[16] = "motor";
                bool enabled = true;
                uint8_t slaveAddress = 8;
                uint16_t motorRegs[5] = {0,1,2,5,7};
                int32_t motorMinPosition = 0, motorMaxPosition = 1000;
                struct Profile {
                    bool valid = true, finitePulseMove = true;
                    bool commandPosition = false, startupReference = false;
                    int32_t startupPosition = 0;
                    uint8_t readFunction = 3, writeFunction = 6;
                    uint16_t forwardValue = 1, reverseValue = 1, stopValue = 1;
                    uint16_t readRegister(const uint16_t* regs, bool pulse) const { return regs[pulse ? 4 : 3]; }
                } motorProtocol;
            } devices[Protocols::MODBUS_MAX_SUB_DEVICES];
        } master;
    } config;
    bool _shouldDeferSlaveRequest(uint8_t, uint32_t) { return defer; }
    void _recordSlaveOutcome(uint8_t, OneShotError, uint32_t) {}
    void trackCoilDelayWriteLocked(const uint8_t* request, size_t length) {
        assert(request && length >= 4);
    }
    void setTransmitMode(bool) {}
    uint16_t calculateCRC(const uint8_t*, size_t);
    bool validateFrame(const uint8_t*, size_t);
    OneShotError _executeModbusTransactionRaw(const uint8_t*,size_t,uint8_t,uint8_t*,size_t,size_t&,
                                             uint8_t&,uint16_t,uint8_t,bool,ModbusReadDiagnostics*);
    OneShotError _sendOneShotRequestRaw(const uint8_t*,size_t,uint8_t,uint8_t*,size_t,size_t&,
                                        uint8_t&,uint16_t,uint8_t,bool = false,ModbusReadDiagnostics* = nullptr);
    OneShotResult readRegistersOnce(uint8_t,uint8_t,uint16_t,uint16_t,bool,uint16_t,uint8_t,
                                    String*,ModbusReadDiagnostics*);
    OneShotError _readRangeInto(uint8_t,uint8_t,uint16_t,uint16_t,uint16_t*,uint16_t,uint16_t,uint8_t);
};

int xSemaphoreTake(void* handle, unsigned long) {
    return static_cast<ModbusHandler*>(handle)->semaphoreAvailable ? pdTRUE : pdFALSE;
}
int xSemaphoreGive(void*) { return pdTRUE; }

#include "modbus_oneshot_bounds_production.inc"

std::vector<uint8_t> adu(ModbusHandler& bus, std::vector<uint8_t> bytes) {
    const uint16_t crc = bus.calculateCRC(bytes.data(), bytes.size());
    bytes.push_back(static_cast<uint8_t>(crc & 0xFF));
    bytes.push_back(static_cast<uint8_t>(crc >> 8));
    return bytes;
}

// FC03/04 response with value(i) = 1000 + i (distinct high/low bytes per slot).
// declaredByteCount overrides the on-wire byteCount without changing the
// payload length, to exercise the byteCount gate behind a valid CRC.
std::vector<uint8_t> registerReply(ModbusHandler& bus, uint8_t slave, uint8_t fc,
                                   uint16_t quantity, uint8_t declaredByteCount = 0) {
    const uint8_t byteCount = declaredByteCount
        ? declaredByteCount : static_cast<uint8_t>(quantity * 2);
    std::vector<uint8_t> body{slave, fc, byteCount};
    for (uint16_t i = 0; i < quantity; ++i) {
        const uint16_t value = static_cast<uint16_t>(1000 + i);
        body.push_back(static_cast<uint8_t>(value >> 8));
        body.push_back(static_cast<uint8_t>(value & 0xFF));
    }
    return adu(bus, body);
}

void armCanaries(uint16_t* slots) { for (size_t i = 0; i < 16; ++i) slots[i] = 0xA5A5; }
bool canariesIntact(const uint16_t* slots) {
    for (size_t i = 0; i < 16; ++i) if (slots[i] != 0xA5A5) return false;
    return true;
}

// 16-slot guard bands on both sides of the structured result. readRegistersOnce
// returns by value, so the deterministic tripwires for intra-struct overflow
// are the trailing OneShotResult fields asserted after every case; the bands
// additionally fence the stack object the callee stores through (NRVO).
struct ResultCanary {
    uint16_t before[16];
    OneShotResult result;
    uint16_t after[16];
    void arm() { armCanaries(before); armCanaries(after); }
    bool intact() const { return canariesIntact(before) && canariesIntact(after); }
};

template <size_t N>
struct BufferCanary {
    uint16_t before[16];
    uint16_t buf[N];
    uint16_t after[16];
    void arm() {
        armCanaries(before); armCanaries(after);
        for (size_t i = 0; i < N; ++i) buf[i] = 0x5A5A;
    }
    bool intact() const { return canariesIntact(before) && canariesIntact(after); }
};

void assertTrailingFieldsIntact(const OneShotResult& result) {
    assert(result.travelError == nullptr);
    assert(!result.outputAttempted);
    assert(result.controlReason == nullptr);
    assert(result.delayReceipt.instance == 0 && result.delayReceipt.configuration == 0);
    assert(result.delayReceipt.writeSequence == 0);
    assert(result.delayReceipt.slave == 0 && result.delayReceipt.function == 0);
    assert(result.delayReceipt.address == 0);
}

void assertDataZero(const OneShotResult& result) {
    for (size_t i = 0; i < Protocols::MODBUS_ONESHOT_BUFFER_SIZE; ++i) {
        assert(result.data[i] == 0);
    }
}

void assertValues(const uint16_t* data, uint16_t quantity) {
    for (uint16_t i = 0; i < quantity; ++i) {
        assert(data[i] == static_cast<uint16_t>(1000 + i));
    }
}

int main() {
    ModbusReadDiagnostics diag;

    // A1: qty=14, fully inside the 32-slot buffer.
    {
        ModbusHandler bus;
        bus.serial.replies = {registerReply(bus, 8, 3, 14)};
        ResultCanary box; box.arm();
        box.result = bus.readRegistersOnce(8, 3, 0, 14, false, 0, 0xFF, nullptr, &diag);
        assert(box.result.error == ONESHOT_SUCCESS && box.result.count == 14);
        assertValues(box.result.data, 14);
        assertTrailingFieldsIntact(box.result);
        assert(box.intact() && bus.serial.sent == 1);
        assert(!bus._lastRxHex.isEmpty() && !bus._lastTxHex.isEmpty());
        assert(std::string(diag.reason) == "FRAME_VALIDATED");
    }

    // A2/A3: qty=15 and qty=16, the boundary decade reported by the audit.
    for (const uint16_t quantity : {static_cast<uint16_t>(15), static_cast<uint16_t>(16)}) {
        ModbusHandler bus;
        bus.serial.replies = {registerReply(bus, 8, 3, quantity)};
        OneShotResult result = bus.readRegistersOnce(8, 3, 0, quantity, false, 0, 0xFF, nullptr, &diag);
        assert(result.error == ONESHOT_SUCCESS && result.count == quantity);
        assertValues(result.data, quantity);
        assertTrailingFieldsIntact(result);
        assert(bus.serial.sent == 1);
    }

    // A4: qty=32, the structured ceiling itself.
    {
        ModbusHandler bus;
        bus.serial.replies = {registerReply(bus, 8, 3, 32)};
        ResultCanary box; box.arm();
        box.result = bus.readRegistersOnce(8, 3, 0, 32, false, 0, 0xFF, nullptr, &diag);
        assert(box.result.error == ONESHOT_SUCCESS && box.result.count == 32);
        assertValues(box.result.data, 32);
        assertTrailingFieldsIntact(box.result);
        assert(box.intact() && bus.serial.sent == 1);
    }

    // A5: qty=33 without the RAW channel is rejected at the entry gate, before
    // any bus access; an out-of-range function code fails the same way.
    {
        ModbusHandler bus;
        OneShotResult over = bus.readRegistersOnce(8, 3, 0, 33, false, 0, 0xFF, nullptr, &diag);
        assert(over.error == ONESHOT_EXCEPTION);
        assert(over.exceptionCode == MODBUS_EX_ILLEGAL_DATA_VALUE);
        assert(bus.serial.sent == 0 && std::string(diag.reason) == "INVALID_REQUEST");
        assertDataZero(over);
        assertTrailingFieldsIntact(over);
        OneShotResult badFc = bus.readRegistersOnce(8, 5, 0, 1, false, 0, 0xFF, nullptr, &diag);
        assert(badFc.error == ONESHOT_EXCEPTION);
        assert(badFc.exceptionCode == MODBUS_EX_ILLEGAL_FUNCTION);
        assert(bus.serial.sent == 0);
    }

    // A6: qty=125 with the RAW channel completes, keeps only the raw ADU hex
    // and never decodes into the 32-slot structured buffer.
    {
        ModbusHandler bus;
        bus.serial.replies = {registerReply(bus, 8, 3, 125)};
        String raw;
        ResultCanary box; box.arm();
        box.result = bus.readRegistersOnce(8, 3, 0, 125, false, 0, 0xFF, &raw, &diag);
        assert(box.result.error == ONESHOT_SUCCESS && box.result.count == 0);
        assert(raw.length() == static_cast<size_t>((125 * 2 + 5) * 2));
        assert(raw.value.compare(0, 6, "0803FA") == 0);
        assertDataZero(box.result);
        assertTrailingFieldsIntact(box.result);
        assert(box.intact() && bus.serial.sent == 1);
        assert(!bus._lastRxHex.isEmpty() && std::string(diag.reason) == "FRAME_VALIDATED");
    }

    // A7: qty=33 with the RAW channel follows the same raw-only contract.
    {
        ModbusHandler bus;
        bus.serial.replies = {registerReply(bus, 8, 3, 33)};
        String raw;
        OneShotResult result = bus.readRegistersOnce(8, 3, 0, 33, false, 0, 0xFF, &raw, &diag);
        assert(result.error == ONESHOT_SUCCESS && result.count == 0);
        assert(raw.length() == static_cast<size_t>((33 * 2 + 5) * 2));
        assertDataZero(result);
        assertTrailingFieldsIntact(result);
        assert(bus.serial.sent == 1);
    }

    // A8: declared byteCount disagrees with the payload behind a valid CRC.
    {
        ModbusHandler bus;
        bus.serial.replies = {registerReply(bus, 8, 3, 16, 30)};
        OneShotResult result = bus.readRegistersOnce(8, 3, 0, 16, false, 0, 0xFF, nullptr, &diag);
        assert(result.error == ONESHOT_CRC_ERROR);
        assert(std::string(diag.reason) == "BYTE_COUNT_MISMATCH");
        assertDataZero(result);
        assertTrailingFieldsIntact(result);
        assert(bus.serial.sent == 1);
    }

    // A9: truncated frame (valid CRC over fewer registers than requested) is
    // stopped by the exact-length gate.
    {
        ModbusHandler bus;
        bus.serial.replies = {registerReply(bus, 8, 3, 10)};
        OneShotResult result = bus.readRegistersOnce(8, 3, 0, 32, false, 0, 0xFF, nullptr, &diag);
        assert(result.error == ONESHOT_CRC_ERROR);
        assert(std::string(diag.reason) == "RESPONSE_LENGTH_MISMATCH");
        assertDataZero(result);
        assertTrailingFieldsIntact(result);
        assert(bus.serial.sent == 1);
    }

    // A10: Modbus exception frame surfaces the exception code verbatim.
    {
        ModbusHandler bus;
        bus.serial.replies = {adu(bus, {8, 0x83, 0x02})};
        OneShotResult result = bus.readRegistersOnce(8, 3, 0, 4, false, 0, 0xFF, nullptr, &diag);
        assert(result.error == ONESHOT_EXCEPTION);
        assert(result.exceptionCode == MODBUS_EX_ILLEGAL_DATA_ADDRESS);
        assert(std::string(diag.reason) == "MODBUS_EXCEPTION");
        assertDataZero(result);
        assertTrailingFieldsIntact(result);
        assert(bus.serial.sent == 1);
    }

    // A11: response function code mismatch is rejected as a frame error.
    {
        ModbusHandler bus;
        bus.serial.replies = {adu(bus, {8, 4, 2, 0, 1})};
        OneShotResult result = bus.readRegistersOnce(8, 3, 0, 1, false, 0, 0xFF, nullptr, &diag);
        assert(result.error == ONESHOT_CRC_ERROR);
        assert(std::string(diag.reason) == "FUNCTION_MISMATCH");
        assertDataZero(result);
        assertTrailingFieldsIntact(result);
        assert(bus.serial.sent == 1);
    }

    // A12: FC01 bit expansion fills exactly 32 slots, LSB-first per byte. The
    // third data byte is 0x00 so coils 16-23 read 0 (the audit matrix listed
    // 0x80 together with data[23]==0; those two contradict, the expected bit
    // values are authoritative here).
    {
        ModbusHandler bus;
        bus.serial.replies = {adu(bus, {8, 1, 4, 0x81, 0x01, 0x00, 0xFF})};
        ResultCanary box; box.arm();
        box.result = bus.readRegistersOnce(8, 1, 0, 32, false, 0, 0xFF, nullptr, &diag);
        assert(box.result.error == ONESHOT_SUCCESS && box.result.count == 32);
        assert(box.result.data[0] == 1 && box.result.data[1] == 0);
        assert(box.result.data[7] == 1 && box.result.data[8] == 1);
        assert(box.result.data[15] == 0 && box.result.data[16] == 0);
        assert(box.result.data[23] == 0);
        for (uint16_t i = 24; i < 32; ++i) assert(box.result.data[i] == 1);
        assertTrailingFieldsIntact(box.result);
        assert(box.intact() && bus.serial.sent == 1);
    }

    // A13: FC01 qty=33 with the RAW channel also stays raw-only.
    {
        ModbusHandler bus;
        bus.serial.replies = {adu(bus, {8, 1, 5, 0xAA, 0x55, 0xAA, 0x55, 0xAA})};
        String raw;
        OneShotResult result = bus.readRegistersOnce(8, 1, 0, 33, false, 0, 0xFF, &raw, &diag);
        assert(result.error == ONESHOT_SUCCESS && result.count == 0);
        assert(raw.length() == static_cast<size_t>((5 + 5) * 2));
        assertDataZero(result);
        assertTrailingFieldsIntact(result);
        assert(bus.serial.sent == 1);
    }

    // A14: a busy bus with an unavailable semaphore reports ONESHOT_BUSY
    // without touching the wire.
    {
        ModbusHandler bus;
        bus.isOneShotReading = true;
        bus.semaphoreAvailable = false;
        OneShotResult result = bus.readRegistersOnce(8, 3, 0, 1, false, 0, 0xFF, nullptr, &diag);
        assert(result.error == ONESHOT_BUSY);
        assert(bus.serial.sent == 0 && std::string(diag.reason) == "BUS_ACQUIRE_FAILED");
        assertDataZero(result);
        assertTrailingFieldsIntact(result);
    }

    // B15: merged-range read of 33 registers into a 40-slot caller buffer.
    {
        ModbusHandler bus;
        bus.serial.replies = {registerReply(bus, 8, 3, 33)};
        BufferCanary<40> box; box.arm();
        assert(bus._readRangeInto(8, 3, 0, 33, box.buf, 40, 0, 0xFF) == ONESHOT_SUCCESS);
        assertValues(box.buf, 33);
        assert(box.buf[33] == 0x5A5A);
        assert(box.intact() && bus.serial.sent == 1);
    }

    // B16: merged-range read at the protocol ceiling of 125 registers.
    {
        ModbusHandler bus;
        bus.serial.replies = {registerReply(bus, 8, 3, 125)};
        BufferCanary<125> box; box.arm();
        assert(bus._readRangeInto(8, 3, 0, 125, box.buf, 125, 0, 0xFF) == ONESHOT_SUCCESS);
        assertValues(box.buf, 125);
        assert(box.buf[124] == static_cast<uint16_t>(1000 + 124));
        assert(box.intact() && bus.serial.sent == 1);
    }

    // B17: quantity above the caller buffer capacity is rejected before the bus.
    {
        ModbusHandler bus;
        BufferCanary<5> box; box.arm();
        assert(bus._readRangeInto(8, 3, 0, 10, box.buf, 5, 0, 0xFF) == ONESHOT_EXCEPTION);
        for (size_t i = 0; i < 5; ++i) assert(box.buf[i] == 0x5A5A);
        assert(box.intact() && bus.serial.sent == 0);
    }

    // B18: byteCount mismatch in the merged-range path leaves the buffer alone.
    {
        ModbusHandler bus;
        bus.serial.replies = {registerReply(bus, 8, 3, 10, 18)};
        BufferCanary<16> box; box.arm();
        assert(bus._readRangeInto(8, 3, 0, 10, box.buf, 16, 0, 0xFF) == ONESHOT_CRC_ERROR);
        for (size_t i = 0; i < 16; ++i) assert(box.buf[i] == 0x5A5A);
        assert(box.intact() && bus.serial.sent == 1);
    }

    std::puts("FW-P0-01 OneShotResult bounds matrix passed: quantities 14/15/16/32/33/125, "
              "truncated/exception/FC-mismatch frames, FC01 bits, busy path, canaries intact");
    return 0;
}
