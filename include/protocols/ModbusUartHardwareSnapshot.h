#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#define FASTBEE_UART_GPIO_SNAPSHOT_SUPPORTED 0
#if defined(ESP32)
#include <esp_idf_version.h>
#if defined(CONFIG_IDF_TARGET_ESP32S3) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
#undef FASTBEE_UART_GPIO_SNAPSHOT_SUPPORTED
#define FASTBEE_UART_GPIO_SNAPSHOT_SUPPORTED 1
#include <driver/gpio.h>
#include <driver/uart.h>
#include <soc/gpio_struct.h>
#include <soc/io_mux_reg.h>
#include <soc/uart_periph.h>
#include <soc/system_reg.h>
#endif
#endif

namespace FastBee {

#if FASTBEE_UART_GPIO_SNAPSHOT_SUPPORTED
inline void appendModbusPinHardwareSnapshot(JsonObject out, int pin) {
    out["pin"] = pin;
    if (!GPIO_IS_VALID_GPIO(pin)) {
        out["valid"] = false;
        out["error"] = "INVALID_GPIO";
        return;
    }
    gpio_io_config_t state{};
    const esp_err_t err = gpio_get_io_config(static_cast<gpio_num_t>(pin), &state);
    out["errorCode"] = static_cast<int>(err);
    out["valid"] = err == ESP_OK;
    if (err != ESP_OK) return;
    out["function"] = state.fun_sel;
    out["outputSignal"] = state.sig_out;
    out["inputEnabled"] = state.ie;
    out["outputEnabled"] = state.oe;
    out["outputEnableControlledByPeripheral"] = state.oe_ctrl_by_periph;
    out["outputEnableInverted"] = state.oe_inv;
    out["pullUp"] = state.pu;
    out["pullDown"] = state.pd;
    out["openDrain"] = state.od;
    out["sleepSelected"] = state.slp_sel;
    out["driveCapability"] = static_cast<int>(state.drv);
    out["sampledInputLevel"] = gpio_get_level(static_cast<gpio_num_t>(pin));
    const uint32_t output = pin < 32 ? GPIO.out : GPIO.out1.val;
    out["outputLatchLevel"] = (output >> (pin < 32 ? pin : pin - 32)) & 1U;
}
#endif

// Explicit diagnostic snapshots only: no UART reads/writes, pin changes or resets.
inline void appendModbusUartHardwareSnapshot(JsonObject out, HardwareSerial* serial,
                                            int rxPin, int txPin) {
    out["schemaVersion"] = 1;
    out["sampledAtMs"] = millis();
    out["readOnly"] = true;
    out["supported"] = FASTBEE_UART_GPIO_SNAPSHOT_SUPPORTED != 0;
    out["configuredRxPin"] = rxPin;
    out["configuredTxPin"] = txPin;
#if FASTBEE_UART_GPIO_SNAPSHOT_SUPPORTED
    out["evidenceType"] = "LIVE_GPIO_UART_REGISTERS";
    int port = -1;
    if (serial == &Serial0) port = 0;
#if SOC_UART_NUM > 1
    if (serial == &Serial1) port = 1;
#endif
#if SOC_UART_NUM > 2
    if (serial == &Serial2) port = 2;
#endif
    out["uartPort"] = port;
    out["gpioMatrixFunction"] = PIN_FUNC_GPIO;
    const bool knownPort = port >= 0 && port < SOC_UART_HP_NUM;
    const bool driverReady = knownPort && serial && static_cast<bool>(*serial) &&
                             uart_is_driver_installed(static_cast<uart_port_t>(port));
    out["driverReady"] = driverReady;
    bool hardwareEnabled = false;
    if (knownPort) {
        // ESP32-S3 UART2 uses register bank 1, unlike UART0 and UART1.
        const uint32_t clock = REG_READ(port == 2 ? SYSTEM_PERIP_CLK_EN1_REG : SYSTEM_PERIP_CLK_EN0_REG);
        const uint32_t reset = REG_READ(port == 2 ? SYSTEM_PERIP_RST_EN1_REG : SYSTEM_PERIP_RST_EN0_REG);
        const uint32_t clockMask = port == 2 ? SYSTEM_UART2_CLK_EN :
                                   port == 1 ? SYSTEM_UART1_CLK_EN : SYSTEM_UART_CLK_EN;
        const uint32_t resetMask = port == 2 ? SYSTEM_UART2_RST :
                                   port == 1 ? SYSTEM_UART1_RST : SYSTEM_UART_RST;
        const bool clockEnabled = (clock & clockMask) != 0;
        const bool resetActive = (reset & resetMask) != 0;
        hardwareEnabled = clockEnabled && !resetActive;
        out["clockRegisterBank"] = port == 2 ? 1 : 0;
        out["rawClockRegister"] = clock;
        out["rawResetRegister"] = reset;
        out["hardwareClockEnabled"] = clockEnabled;
        out["hardwareResetActive"] = resetActive;
        out["hardwareEnabled"] = hardwareEnabled;
    }
    if (driverReady) {
        uint32_t baud = 0;
        const esp_err_t err = uart_get_baudrate(static_cast<uart_port_t>(port), &baud);
        out["baudErrorCode"] = static_cast<int>(err);
        if (err == ESP_OK) out["actualBaudRate"] = baud;
        out["baudGetterDisagreesWithHardware"] = err != ESP_OK && hardwareEnabled;
        if (hardwareEnabled) {
            out["registerBaudRate"] = serial->baudRate();
            out["registerBaudRateSource"] = "ARDUINO_CLOCK_DIVIDER_GETTER";
        }
    }
    appendModbusPinHardwareSnapshot(out["rx"].to<JsonObject>(), rxPin);
    appendModbusPinHardwareSnapshot(out["tx"].to<JsonObject>(), txPin);
    if (knownPort) {
        const auto& signals = uart_periph_signal[port];
        out["expectedTxSignal"] = signals.pins[SOC_UART_TX_PIN_IDX].signal;
        const int signal = signals.pins[SOC_UART_RX_PIN_IDX].signal;
        JsonObject input = out["rxInputMatrix"].to<JsonObject>();
        input["signalIndex"] = signal;
        if (signal < 0 || signal >= 256) {
            input["valid"] = false;
        } else {
            // ESP32-S3 register layout: GPIO selection [5:0], inversion [6], matrix [7].
            const uint32_t raw = GPIO.func_in_sel_cfg[signal].val;
            input["valid"] = true;
            input["rawRegister"] = raw;
            input["selectedGpio"] = raw & 0x3fU;
            input["inverted"] = (raw & 0x40U) != 0;
            input["matrixSelected"] = (raw & 0x80U) != 0;
        }
    }
#else
    (void)serial;
    out["evidenceType"] = "UNSUPPORTED_TARGET_OR_SDK";
#endif
}

} // namespace FastBee
