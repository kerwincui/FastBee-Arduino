#include <cassert>
#include <cstdint>
#include <cstring>
#include <ArduinoJson.h>

class HardwareSerial {
public:
    bool ready = true;
    explicit operator bool() const { return ready; }
    uint32_t baudRate() const { return 9600; }
};
HardwareSerial Serial0, Serial1, Serial2;
uint32_t millis() { return 1234; }

#ifndef TEST_UNSUPPORTED
#define ESP32 1
#define CONFIG_IDF_TARGET_ESP32S3 1
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,4)
#define SOC_UART_NUM 3
#define SOC_UART_HP_NUM 3
#define SOC_UART_TX_PIN_IDX 0
#define SOC_UART_RX_PIN_IDX 1
#define PIN_FUNC_GPIO 1
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 49)
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) (GPIO_IS_VALID_GPIO(pin) && (pin) != 46)
#define SYSTEM_PERIP_CLK_EN0_REG 0
#define SYSTEM_PERIP_CLK_EN1_REG 1
#define SYSTEM_PERIP_RST_EN0_REG 2
#define SYSTEM_PERIP_RST_EN1_REG 3
#define SYSTEM_UART_CLK_EN (1U << 2)
#define SYSTEM_UART1_CLK_EN (1U << 5)
#define SYSTEM_UART2_CLK_EN (1U << 9)
#define SYSTEM_UART_RST SYSTEM_UART_CLK_EN
#define SYSTEM_UART1_RST SYSTEM_UART1_CLK_EN
#define SYSTEM_UART2_RST SYSTEM_UART2_CLK_EN
uint32_t systemRegs[4] = {SYSTEM_UART_CLK_EN | SYSTEM_UART1_CLK_EN, SYSTEM_UART2_CLK_EN, 0, 0};
#define REG_READ(reg) systemRegs[reg]
using esp_err_t = int;
using gpio_num_t = int;
using uart_port_t = int;
constexpr int ESP_OK = 0;
struct gpio_io_config_t {
    uint32_t fun_sel = 1, sig_out = 256;
    int drv = 2;
    bool pu = false, pd = false, ie = true, oe = false, oe_ctrl_by_periph = false;
    bool oe_inv = false, od = false, slp_sel = false;
};
struct GpioMock { struct { uint32_t val = 0; } func_in_sel_cfg[256], out1; uint32_t out = 0; };
GpioMock gpioStorage;
GpioMock& GPIO = gpioStorage;
struct Signals { struct { int signal; } pins[2]; };
const Signals uart_periph_signal[3] = {{{{14},{15}}}, {{{17},{18}}}, {{{198},{199}}}};
gpio_io_config_t pins[49];
int modeWrites = 0;
void gpio_ll_output_disable(GpioMock*, uint32_t pin) { ++modeWrites; pins[pin].oe = false; }
void gpio_ll_set_output_enable_ctrl(GpioMock*, uint8_t pin, bool peripheral, bool inverted) {
    ++modeWrites; pins[pin].oe_ctrl_by_periph = peripheral; pins[pin].oe_inv = inverted;
}
int pinError = 0, gpioReads = 0, levelReads = 0, baudReads = 0, driverReads = 0;
int baudError = 0;
bool installed = true;
esp_err_t gpio_get_io_config(int pin, gpio_io_config_t* out) {
    assert(GPIO_IS_VALID_GPIO(pin)); ++gpioReads;
    if (pinError) return pinError;
    *out = pins[pin]; return 0;
}
int gpio_get_level(int pin) { assert(GPIO_IS_VALID_GPIO(pin)); ++levelReads; return 1; }
bool uart_is_driver_installed(int port) { assert(port >= 0 && port < 3); ++driverReads; return installed; }
esp_err_t uart_get_baudrate(int port, uint32_t* baud) {
    assert(port >= 0 && port < 3); ++baudReads; *baud = 9600; return baudError;
}
#endif

#include "protocols/ModbusUartHardwareSnapshot.h"
#include "protocols/ModbusUartRxMode.h"

int main() {
    JsonDocument doc;
#ifdef TEST_UNSUPPORTED
    FastBee::appendModbusUartHardwareSnapshot(doc.to<JsonObject>(), &Serial2, 43, 44);
    assert(doc["supported"] == false);
    assert(doc["evidenceType"] == "UNSUPPORTED_TARGET_OR_SDK");
    assert(doc["rx"].isNull());
#else
    pins[43].oe = true; // Preserve a suspicious state, never silently normalize it.
    pins[44].oe = true;
    pins[44].sig_out = 198;
    gpioStorage.func_in_sel_cfg[199].val = 0x80 | 43;
    const auto originalGpio = gpioStorage;
    const auto originalRx = pins[43];
    FastBee::appendModbusUartHardwareSnapshot(doc.to<JsonObject>(), &Serial2, 43, 44);
    assert(doc["supported"] == true && doc["readOnly"] == true);
    assert(doc["uartPort"] == 2 && doc["driverReady"] == true);
    assert(doc["actualBaudRate"] == 9600);
    assert(doc["clockRegisterBank"] == 1 && doc["hardwareEnabled"] == true);
    assert(doc["registerBaudRate"] == 9600);
    assert(modeWrites == 0);
    assert(doc["rx"]["inputEnabled"] == true && doc["rx"]["outputEnabled"] == true);
    assert(doc["tx"]["outputSignal"] == 198 && doc["expectedTxSignal"] == 198);
    assert(doc["rxInputMatrix"]["selectedGpio"] == 43);
    assert(doc["rxInputMatrix"]["matrixSelected"] == true);
    assert(doc["rxInputMatrix"]["inverted"] == false);
    assert(std::memcmp(&gpioStorage, &originalGpio, sizeof(gpioStorage)) == 0);
    assert(pins[43].oe == originalRx.oe && pins[43].fun_sel == originalRx.fun_sel);
    assert(gpioReads == 2 && levelReads == 2 && baudReads == 1);

    doc.clear(); installed = false;
    FastBee::appendModbusUartHardwareSnapshot(doc.to<JsonObject>(), &Serial2, 43, 44);
    assert(doc["driverReady"] == false && doc["actualBaudRate"].isNull());
    assert(baudReads == 1);
    doc.clear(); installed = true; baudError = 259;
    FastBee::appendModbusUartHardwareSnapshot(doc.to<JsonObject>(), &Serial2, 43, 44);
    assert(doc["baudErrorCode"] == 259 && doc["actualBaudRate"].isNull());
    assert(doc["baudGetterDisagreesWithHardware"] == true);
    assert(doc["registerBaudRate"] == 9600);
    systemRegs[SYSTEM_PERIP_RST_EN1_REG] = SYSTEM_UART2_RST;
    doc.clear();
    FastBee::appendModbusUartHardwareSnapshot(doc.to<JsonObject>(), &Serial2, 43, 44);
    assert(doc["hardwareEnabled"] == false && doc["hardwareResetActive"] == true);
    assert(doc["registerBaudRate"].isNull());
    systemRegs[SYSTEM_PERIP_RST_EN1_REG] = 0;
    systemRegs[SYSTEM_PERIP_CLK_EN1_REG] = 0;
    doc.clear();
    FastBee::appendModbusUartHardwareSnapshot(doc.to<JsonObject>(), &Serial2, 43, 44);
    assert(doc["hardwareEnabled"] == false && doc["hardwareClockEnabled"] == false);
    systemRegs[SYSTEM_PERIP_CLK_EN1_REG] = SYSTEM_UART2_CLK_EN;

    doc.clear(); pinError = 258;
    const int oldLevels = levelReads;
    FastBee::appendModbusUartHardwareSnapshot(doc.to<JsonObject>(), &Serial2, 43, 44);
    assert(doc["rx"]["valid"] == false && doc["rx"]["outputEnabled"].isNull());
    assert(levelReads == oldLevels);
    doc.clear(); pinError = 0;
    const int oldReads = gpioReads;
    FastBee::appendModbusUartHardwareSnapshot(doc.to<JsonObject>(), nullptr, 255, -1);
    assert(doc["uartPort"] == -1 && doc["driverReady"] == false);
    assert(doc["rx"]["valid"] == false && doc["tx"]["valid"] == false);
    assert(doc["rxInputMatrix"].isNull() && gpioReads == oldReads);

    doc.clear(); gpioStorage.func_in_sel_cfg[199].val = 0x40 | 44;
    FastBee::appendModbusUartHardwareSnapshot(doc.to<JsonObject>(), &Serial2, 43, 44);
    assert(doc["rxInputMatrix"]["matrixSelected"] == false);
    assert(doc["rxInputMatrix"]["inverted"] == true);
    assert(doc["rxInputMatrix"]["selectedGpio"] == 44);
    assert(doc["rootCauseConfirmed"].isNull());
    assert(modeWrites == 0);
    pins[43].oe_ctrl_by_periph = true;
    pins[43].oe_inv = true;
    assert(FastBee::configureModbusReceiveOnlyPin(43, 44));
    assert(!pins[43].oe && !pins[43].oe_ctrl_by_periph && !pins[43].oe_inv);
    assert(pins[43].ie && pins[44].oe && pins[43].fun_sel == originalRx.fun_sel);
    assert(gpioStorage.func_in_sel_cfg[199].val == (0x40 | 44));
    const int oldModeWrites = modeWrites;
    assert(!FastBee::configureModbusReceiveOnlyPin(43, 43));
    assert(!FastBee::configureModbusReceiveOnlyPin(255, 44));
    assert(!FastBee::configureModbusReceiveOnlyPin(43, 46));
    assert(modeWrites == oldModeWrites);
    pins[16].oe = true; pins[16].fun_sel = 2;
    assert(FastBee::configureModbusReceiveOnlyPin(16, 17));
    assert(!pins[16].oe && pins[16].fun_sel == 2);
#endif
    assert(doc["schemaVersion"] == 1 && doc["sampledAtMs"] == 1234);
    return 0;
}
