'use strict';

const assert = require('assert');
const { fingerprintDocuments, validatePeripherals } = require('./validate-device-config-examples');

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

function triggerIssues(triggerType, actionTarget = 'relay') {
    const documents = new Map([
        ['peripherals.json', {peripherals: [{id: 'relay', type: 10, enabled: true, pins: [4]}]}],
        ['periph_exec.json', {rules: [{id: 'command', enabled: true,
            triggers: [{triggerType, triggerPeriphId: 'motor_forward_request'}],
            actions: [{actionType: 20, targetPeriphId: actionTarget}]}]}]
    ]);
    const issues = [];
    validatePeripherals({source: 'fixture', peripherals: {}}, documents, issues);
    return issues;
}
assert.deepStrictEqual(triggerIssues(0), [], 'platform command IDs are independent of hardware IDs');
assert(triggerIssues(5).some(issue => issue.includes('trigger references unknown peripheral')),
    'local polling sources still require an existing peripheral');
assert(triggerIssues(4).some(issue => issue.includes('trigger references unknown peripheral')),
    'local event sources still require an existing peripheral');
assert(triggerIssues(undefined).some(issue => issue.includes('trigger references unknown peripheral')),
    'missing trigger type must not acquire platform command semantics');
assert(triggerIssues(null).some(issue => issue.includes('trigger references unknown peripheral')),
    'null trigger type must not acquire platform command semantics');
assert(triggerIssues(0, 'missing_relay').some(issue => issue.includes('action references unknown peripheral')),
    'platform commands must still validate their physical action targets');

console.log('FastBee device configuration semantic fingerprint and platform trigger contracts OK');
