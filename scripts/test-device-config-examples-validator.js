'use strict';

const assert = require('assert');
const { fingerprintDocuments } = require('./validate-device-config-examples');

function parseDocuments(entries) {
    return new Map(entries.map(([name, text]) => [name, JSON.parse(text)]));
}

const compact = parseDocuments([
    ['device.json', '{"productNumber":0,"deviceId":""}'],
    ['protocol.json', '{"mqtt":{"enabled":false,"port":1883},"modbusRtu":{"enabled":true}}']
]);
const reformattedAndReordered = parseDocuments([
    ['protocol.json', `{
        "modbusRtu": { "enabled": true },
        "mqtt": { "port": 1883, "enabled": false }
    }`],
    ['device.json', `{
        "deviceId": "",
        "productNumber": 0
    }`]
]);

assert.strictEqual(
    fingerprintDocuments(compact),
    fingerprintDocuments(reformattedAndReordered),
    'JSON whitespace, object key order, and document order must not bypass duplicate configuration detection'
);

const changed = parseDocuments([
    ['device.json', '{"productNumber":0,"deviceId":""}'],
    ['protocol.json', '{"mqtt":{"enabled":true,"port":1883},"modbusRtu":{"enabled":true}}']
]);
assert.notStrictEqual(
    fingerprintDocuments(compact),
    fingerprintDocuments(changed),
    'a real configuration value change must produce a different semantic fingerprint'
);

console.log('FastBee device configuration semantic fingerprint contract OK');
