#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Request-owned, bounded evidence. RX bytes are observations, not valid data.
// Optional so regular polling and control results do not grow on the stack.
struct ModbusReadDiagnostics {
    const char* reason = "NOT_SENT";
    uint16_t attempts = 0;
    uint16_t txLength = 0;
    uint16_t txWritten = 0;
    uint16_t rxBuffered = 0;
    uint32_t totalRxBuffered = 0;
    bool uartReady = false;
    uint32_t uartBaudRate = 0;
    bool rxOverflow = false;
    bool crcValid = false;
    bool addressMatched = false;
    uint8_t txPrefix[16]{};
    uint8_t rxPrefix[24]{};
    uint8_t txPrefixLength = 0;
    uint8_t rxPrefixLength = 0;

    void reset(const uint8_t* request = nullptr, size_t length = 0) {
        *this = ModbusReadDiagnostics{};
        if (!request) return;
        txLength = static_cast<uint16_t>(length);
        txPrefixLength = static_cast<uint8_t>(length < sizeof(txPrefix) ? length : sizeof(txPrefix));
        memcpy(txPrefix, request, txPrefixLength);
    }

    void beginAttempt() {
        ++attempts;
        reason = "TX_PENDING";
        txWritten = rxBuffered = rxPrefixLength = 0;
        rxOverflow = crcValid = addressMatched = false;
        memset(rxPrefix, 0, sizeof(rxPrefix));
    }

    void captureRx(const uint8_t* bytes, size_t length, bool overflow) {
        rxBuffered = static_cast<uint16_t>(length);
        totalRxBuffered += rxBuffered;
        rxOverflow = overflow;
        rxPrefixLength = static_cast<uint8_t>(length < sizeof(rxPrefix) ? length : sizeof(rxPrefix));
        memcpy(rxPrefix, bytes, rxPrefixLength);
    }
};
