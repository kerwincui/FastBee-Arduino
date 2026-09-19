#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SENSOR_DRIVER

#include "core/DriverRegistry.h"
#include "peripherals/drivers/SHT31Driver.h"

#include <ArduinoJson.h>

namespace {

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

} // namespace

const char* SHT31Driver::getChannelName(uint8_t channel) const {
    switch (channel) {
        case 0: return "temperature";
        case 1: return "humidity";
        default: return "unknown";
    }
}

const char* SHT31Driver::getChannelUnit(uint8_t channel) const {
    switch (channel) {
        case 0: return "\xC2\xB0" "C";
        case 1: return "%";
        default: return "";
    }
}

bool SHT31Driver::init(uint8_t pin, const char* params) {
    (void)pin;
    _ready = false;
    JsonDocument doc;
    // open() owns bounded admission and replacement of any existing lease.
    if (!_bus.open(this, getName(), params, doc)) return false;
    _ready = _bus.write(nullptr, 0);
    return _ready;
}

bool SHT31Driver::read(SensorReading& reading) {
    reading = SensorReading();
    if (!_ready) return _bus.fail("SHT3X_NOT_INITIALIZED");
    const uint8_t trigger[] = {0x24, 0x00};
    if (!_bus.write(trigger, sizeof(trigger))) return false;
    delay(20);
    uint8_t data[6];
    if (!_bus.read(data, sizeof(data))) return false;
    if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5]) {
        return _bus.fail("SHT3X_CRC_FAILED");
    }
    uint16_t rawTemp = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    uint16_t rawHum = (static_cast<uint16_t>(data[3]) << 8) | data[4];
    reading.values[0] = -45.0f + 175.0f * static_cast<float>(rawTemp) / 65535.0f;
    reading.values[1] = 100.0f * static_cast<float>(rawHum) / 65535.0f;
    reading.channelCount = 2;
    reading.success = true;
    reading.timestamp = millis();
    return true;
}

void SHT31Driver::deinit() {
    _ready = false;
    _bus.close();
}

FASTBEE_REGISTER_SENSOR("SHT31", SHT31Driver);
FASTBEE_REGISTER_SENSOR("SHT30", SHT30Driver);

#endif
