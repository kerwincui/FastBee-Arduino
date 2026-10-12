#ifndef FASTBEE_ADS1115_DRIVER_H
#define FASTBEE_ADS1115_DRIVER_H
#include "core/interfaces/ISensorDriver.h"
#include "peripherals/drivers/I2CSensorBus.h"
#include "peripherals/drivers/ADS1115ConversionSession.h"
namespace FastBee {
// One physical owner, optionally a bounded queue of up to four MUXes.
// The address remains reserved across all conversion waits.
class ADS1115Driver final : public ISensorDriver {
public:
    const char* getName() const override { return "ADS1115"; }
    uint8_t getChannelCount() const override { return _scanMask ? ads1115ScanCount(_scanMask) : 2; }
    const char* getChannelName(uint8_t channel) const override;
    const char* getChannelUnit(uint8_t channel) const override;
    bool init(uint8_t pin, const char* params = nullptr) override;
    bool read(SensorReading& reading) override;
    uint32_t cooperativeReadBudgetMs() const override;
    SensorReadStep readStep(SensorReading& reading) override;
    void deinit() override;
    unsigned long getMinInterval() const override;
    const char* getLastError() const override { return _bus.error(); }
private:
    SensorReadStep scanFailed(SensorReading& reading, SensorChannelError error, const char* detail);
    I2CSensorBus _bus;
    ADS1115ConversionSession _session;
    uint16_t _fullScaleMv = 4096, _rate = 128;
    uint8_t _mux = 4;
    uint8_t _scanMask = 0, _scanIndex = 0, _scanMux = 0;
    bool _scanActive = false;
    uint32_t _scanStarted = 0;
    SensorReading _scanReading;
    double _gain = 1, _offset = 0;
    uint32_t _timeoutMs = 100;
    bool _ready = false;
};
}
#endif
