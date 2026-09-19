#ifndef SHT31_DRIVER_H
#define SHT31_DRIVER_H

#include "core/interfaces/ISensorDriver.h"
#include "peripherals/drivers/I2CSensorBus.h"

// Public and registered SHT3x drivers share the same leased I2C transport,
// single-shot measurement, CRC validation, and diagnostic codes.
class SHT31Driver : public ISensorDriver {
public:
    const char* getName() const override { return "SHT31"; }
    uint8_t getChannelCount() const override { return 2; }
    const char* getChannelName(uint8_t channel) const override;
    const char* getChannelUnit(uint8_t channel) const override;
    bool init(uint8_t pin, const char* params = nullptr) override;
    bool read(SensorReading& reading) override;
    void deinit() override;
    unsigned long getMinInterval() const override { return 500; }
    const char* getLastError() const override { return _bus.error(); }

private:
    FastBee::I2CSensorBus _bus;
    bool _ready = false;
};

// SHT30 uses the same command and response frame as SHT31. Its own name
// selects the corresponding registry metadata and persisted configuration.
class SHT30Driver final : public SHT31Driver {
public:
    const char* getName() const override { return "SHT30"; }
};

#endif // SHT31_DRIVER_H
