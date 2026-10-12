// Device descriptor fixtures shared by the offline API and source-level UI gate.
const th = [{ name: 'temperature', unit: '°C' }, { name: 'humidity', unit: '%' }];
export const sensorDrivers = [
  { name: 'AHT20', defaultAddress: 0x38, addressMin: 0x38, addressMax: 0x38, minIntervalMs: 1000, channels: th, params: [] },
  { name: 'BH1750', defaultAddress: 0x23, addressMin: 0x23, addressMax: 0x5c, minIntervalMs: 500, channels: [{ name: 'illuminance', unit: 'lx' }], params: [] },
  { name: 'BMP280', defaultAddress: 0x76, addressMin: 0x76, addressMax: 0x77, minIntervalMs: 500, channels: [{ name: 'temperature', unit: '°C' }, { name: 'pressure', unit: 'hPa' }, { name: 'altitude', unit: 'm' }], params: [] },
  { name: 'MPU6050', defaultAddress: 0x68, addressMin: 0x68, addressMax: 0x69, minIntervalMs: 100, channels: [
    ...['accelX', 'accelY', 'accelZ'].map(name => ({ name, unit: 'm/s²', mode: 1 })),
    ...['gyroX', 'gyroY', 'gyroZ'].map(name => ({ name, unit: 'rad/s', mode: 2 })),
    { name: 'temperature', unit: '°C', mode: 0 },
  ], params: [{ key: 'mode', type: 'integer', min: 0, max: 1, default: 0, choices: [0, 1] }] },
  ...['SHT30', 'SHT31'].map(name => ({ name, defaultAddress: 0x44, addressMin: 0x44, addressMax: 0x45, minIntervalMs: 500, channels: th, params: [] })),
  { name: 'INA226', defaultAddress: 0x40, addressMin: 0x40, addressMax: 0x4f, minIntervalMs: 100, channels: [{ name: 'voltage', unit: 'V' }, { name: 'current', unit: 'A' }, { name: 'power', unit: 'W' }], params: [
    { key: 'shuntResistance', type: 'number', min: 0.000001, max: 1000, default: 0, unit: 'ohm' },
    { key: 'maxCurrent', type: 'number', min: 0.000001, max: 1000000, default: 0, unit: 'A' },
    { key: 'conversionTimeUs', type: 'integer', min: 140, max: 8244, default: 1100, unit: 'us', choices: [140, 204, 332, 588, 1100, 2116, 4156, 8244] },
    { key: 'averaging', type: 'integer', min: 1, max: 1024, default: 16, choices: [1, 4, 16, 64, 128, 256, 512, 1024] },
  ] },
];

export const genericPeripheralTypes = {
  gpio: [11, 12, 13, 14, 18, 19, 20].map(value => ({ value, name: value === 12 ? '数字输出' : '数字 IO', implemented: true, configurable: true, pinRoles: ['GPIO'] })),
  communication: [{ value: 2, name: 'I2C 总线', implemented: true, configurable: true, pinRoles: ['SDA', 'SCL'] }],
  analog: [],
  special: [
    { value: 56, name: '显示屏', implemented: true, configurable: true, featureActive: true, featureKey: 'screenPanel', minPinCount: 0, maxPinCount: 0, usesNamedPins: true,
      driverOptions: [
        { backend: 'spi_tft', driver: 'st7789', label: 'ST7789 SPI TFT', minWidth: 128, maxWidth: 320, minHeight: 128, maxHeight: 320 },
        { backend: 'spi_tft', driver: 'ili9341', label: 'ILI9341 SPI TFT', minWidth: 128, maxWidth: 320, minHeight: 128, maxHeight: 320 },
        { backend: 'rgb', driver: 'st7701', label: 'ST7701 RGB565 (type1)', minWidth: 480, maxWidth: 480, minHeight: 480, maxHeight: 480, rgbInitProfile: 'st7701_type1', requiresPsram: true },
      ], touchOptions: [{ backend: 'none', label: 'No touch' }, { backend: 'gt911', label: 'GT911 I2C', addresses: [93, 20] }],
      configLimits: { widthMin: 128, widthMax: 320, heightMin: 128, heightMax: 320, bufferLinesMin: 1, bufferLinesMax: 64, i2cFrequencies: [100000, 400000], rgbFrequencyMin: 1000000, rgbFrequencyMax: 16000000 } },
    { value: 38, name: '通用传感器', implemented: true, configurable: true, featureActive: true, feature: 'i2cSensors', pinRoles: ['SDA', 'SCL'], diagnostics: 'i2c-sensor', drivers: sensorDrivers.map(driver => driver.name), driverDescriptors: sensorDrivers },
    { value: 55, name: '通用 IO 扩展器', implemented: true, configurable: true, featureActive: true, feature: 'ioExpander', pinRoles: ['SDA', 'SCL'], channelCount: 8, drivers: ['PCF8574'], driverDescriptors: [{ name: 'PCF8574', defaultAddress: 0x20, addressMin: 0x20, addressMax: 0x27, minIntervalMs: 20, maxIntervalMs: 1000, defaultIntervalMs: 20, channelCount: 8, channels: [], params: [], configSchemaVersion: 1, configFields: [
      { key: 'address', type: 'integer', unit: '', default: 32, min: 32, max: 39, required: false },
      { key: 'sampleInterval', type: 'integer', unit: 'ms', default: 20, min: 20, max: 1000, required: false },
      { key: 'busPeripheralId', type: 'reference', unit: '', maxBytes: 40, required: false },
    ] }] },
  ],
};

export const genericPeripherals = [
  { id: 'offline_i2c', name: '共享 I2C', type: 2, enabled: true, implemented: true, featureActive: true, pins: [4, 5], pinCount: 2, params: { frequency: 100000 }, status: 3 },
  { id: 'offline_pcf', name: 'PCF8574 扩展器', type: 55, enabled: true, implemented: true, featureActive: true, pins: [], pinCount: 0, params: { driver: 'PCF8574', driverName: 'PCF8574', busPeripheralId: 'offline_i2c', address: 0x20, sampleInterval: 20 }, status: 3 },
  { id: 'offline_ina', name: '电源监测 INA226', type: 38, enabled: true, implemented: true, featureActive: true, pins: [], pinCount: 0, params: { driver: 'INA226', driverName: 'INA226', busPeripheralId: 'offline_i2c', address: 0x40, mode: 0, frequency: 100000, sampleInterval: 1000, shuntResistance: 0.01, maxCurrent: 5, conversionTimeUs: 1100, averaging: 16 }, status: 3 },
];
