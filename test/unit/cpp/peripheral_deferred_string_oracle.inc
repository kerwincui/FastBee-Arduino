// Frozen pre-repair support bodies. Do not rewrite into the corrected version.
void serializeSensorConfig(JsonObject params, const PeripheralConfig& config) {
    const auto& sensor = config.params.sensor;
    params["sensorType"] = sensor.sensorType;
    params["sampleInterval"] = sensor.sampleInterval ? sensor.sampleInterval : 1000;
    if (!sensor.driver[0]) return;
    params["driver"] = sensor.driver;
    params["address"] = sensor.address;
    params["mode"] = sensor.mode;
    params["frequency"] = sensor.frequency;
    params["busPeripheralId"] = sensor.busPeripheralId;
    if (sensorNameEquals(sensor.driver, "ADS1115")) {
        params["mux"] = sensor.mux;
        params["fullScaleMv"] = sensor.fullScaleMv;
        params["dataRate"] = sensor.dataRate;
        const auto* meta = sensorDriverMetadata(sensor.driver);
        const auto* gain = meta ? meta->getParameter("calibrationGain") : nullptr;
        const auto* offset = meta ? meta->getParameter("calibrationOffset") : nullptr;
        params["calibrationGain"] = gain ? gain->canonicalFloatValue(sensor.calibrationGain) : double(sensor.calibrationGain);
        params["calibrationOffset"] = offset ? offset->canonicalFloatValue(sensor.calibrationOffset) : double(sensor.calibrationOffset);
    }
    if (sensorNameEquals(sensor.driver, "INA226")) {
        params["shuntResistance"] = sensor.shuntResistance;
        params["maxCurrent"] = sensor.maxCurrent;
        params["conversionTimeUs"] = sensor.conversionTimeUs;
        params["averaging"] = sensor.averaging;
    }
}
void serializePca9685Config(JsonObject params, const PeripheralConfig& config) {
    const auto& p = config.params.pca9685;
    params["busPeripheralId"] = p.busPeripheralId; params["oePeripheralId"] = p.oePeripheralId;
    params["address"] = p.address; params["channelMask"] = p.channelMask;
    params["pwmHz"] = p.pwmHz; params["clockHz"] = p.clockHz;
    params["minAngle"] = p.minAngle; params["maxAngle"] = p.maxAngle;
    params["minPulseUs"] = p.minPulseUs; params["maxPulseUs"] = p.maxPulseUs;
    params["openDrain"] = p.openDrain;
}
