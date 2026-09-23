#ifndef FASTBEE_INA226_DRIVER_H
#define FASTBEE_INA226_DRIVER_H

#include "core/interfaces/ISensorDriver.h"
#include "peripherals/drivers/I2CSensorBus.h"

namespace FastBee {

struct INA226Calibration {
    uint16_t registerValue = 0;
    double currentLsb = 0;
    uint16_t configValue = 0;
    uint32_t conversionMs = 0;
};

// Pure validation/calculation, also used by configuration validation and tests.
bool calculateINA226Calibration(double shuntResistance, double maxCurrent,
                                uint16_t conversionTimeUs, uint16_t averaging,
                                INA226Calibration& calibration, const char*& error);

class INA226Driver final : public ISensorDriver {
public:
    const char* getName() const override { return "INA226"; }
    uint8_t getChannelCount() const override { return 3; }
    const char* getChannelName(uint8_t channel) const override;
    const char* getChannelUnit(uint8_t channel) const override;
    bool init(uint8_t pin, const char* params = nullptr) override;
    bool read(SensorReading& reading) override;
    void deinit() override;
    unsigned long getMinInterval() const override;
    const char* getLastError() const override { return _bus.error(); }

private:
    I2CSensorBus _bus;
    INA226Calibration _calibration;
    double _maxCurrent = 0;
    bool _ready = false;
};

} // namespace FastBee
#endif
