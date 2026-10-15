'use strict';

const assert = require('assert');
const {
  filterAndSortDevices,
  mergeDiscoveredDevice,
  normalizeMac,
  parseDeviceStatus,
} = require('../test/helpers/discover-devices');

assert.strictEqual(normalizeMac('d4-e9-f4-01-02-03'), 'D4:E9:F4:01:02:03');
assert.strictEqual(normalizeMac('  D4:E9:F4  '), 'D4:E9:F4');
assert.strictEqual(normalizeMac('146393902f64'), '14:63:93:90:2F:64');
assert.strictEqual(normalizeMac('146393902F64'), '14:63:93:90:2F:64');
assert.strictEqual(normalizeMac('14-63-93-90-2f-64'), '14:63:93:90:2F:64');
assert.strictEqual(normalizeMac('14:63:93:90:2f:64'), '14:63:93:90:2F:64');

const parsed = parseDeviceStatus('192.168.5.55', JSON.stringify({
  success: true,
  data: {
    macAddress: 'd4-e9-f4-01-02-03',
    ssid: 'lab',
    rssi: -61,
    uptime: 12345,
  },
}));
assert.deepStrictEqual(parsed, {
  ip: '192.168.5.55',
  mac: 'D4:E9:F4:01:02:03',
  ssid: 'lab',
  rssi: -61,
  uptimeMs: 12345,
});
assert.strictEqual(parseDeviceStatus('192.168.5.1', '{bad json'), null);
assert.strictEqual(parseDeviceStatus('192.168.5.1', { success: false }), null);
assert.strictEqual(parseDeviceStatus('192.168.5.1', { success: true, data: {} }), null);

const byMac = new Map();
mergeDiscoveredDevice(byMac, parsed);
mergeDiscoveredDevice(byMac, {
  ...parsed,
  ip: '192.168.5.19',
  mac: 'd4:e9:f4:01:02:03',
  uptimeMs: 13000,
});
mergeDiscoveredDevice(byMac, {
  ip: '192.168.5.7',
  mac: 'AA:BB:CC:00:00:01',
  ssid: '',
  rssi: null,
  uptimeMs: 1,
});
assert.strictEqual(byMac.size, 2, 'the same device must be merged across passes and MAC casing');

const sorted = filterAndSortDevices([...byMac.values()]);
assert.deepStrictEqual(sorted.map(device => device.ip), ['192.168.5.7', '192.168.5.19']);
assert.strictEqual(sorted[1].uptimeMs, 13000, 'the latest pass must replace stale device data');
assert.deepStrictEqual(
  filterAndSortDevices(sorted, 'd4-e9-f4').map(device => device.ip),
  ['192.168.5.19'],
  'MAC prefix filtering must ignore case and separator style'
);

const colonMacDevice = {
  ip: '192.168.5.20',
  mac: '14:63:93:90:2F:64',
  ssid: '',
  rssi: null,
  uptimeMs: 1,
};
const macFilterForms = [
  '146393902f64',
  '146393902F64',
  '14-63-93-90-2f-64',
  '14:63:93:90:2f:64',
  '14:63:93',
  '14-63-93',
];
for (const filter of macFilterForms) {
  assert.deepStrictEqual(
    filterAndSortDevices([colonMacDevice], filter).map(device => device.ip),
    ['192.168.5.20'],
    `separator-less 12-hex and prefix MAC filters must match the colon-separated device MAC (${filter})`
  );
}

console.log('FastBee device discovery contracts OK');
