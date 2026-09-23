'use strict';

const assert = require('assert');
const path = require('path');
const { spawnSync } = require('child_process');
const {
    scanCredentialDocument,
    validateForbiddenTracked
} = require('./validate-project-hygiene');

const ROOT = path.resolve(__dirname, '..');

const options = { factoryAPPassword: 'admin123' };
const cleanIssues = [];
scanCredentialDocument('docs/clean-bundle.json', {
    type: 'fastbee-config-bundle',
    files: [
        {
            name: 'network.json',
            content: JSON.stringify({
                staSSID: '',
                staPassword: '',
                apPassword: 'admin123',
                networks: []
            })
        },
        {
            name: 'protocol.json',
            content: JSON.stringify({
                mqtt: { enabled: false, clientId: '', username: '', password: '', mqttSecret: '', authCode: '' }
            })
        },
        {
            name: 'device.json',
            content: JSON.stringify({ deviceId: '', productNumber: 0, userId: '' })
        }
    ]
}, cleanIssues, options);
scanCredentialDocument('data/config/users.json', {
    users: [{ username: 'admin', passwordHash: 'factory-default-hash', salt: 'factory-default-salt' }]
}, cleanIssues, options);
assert.deepStrictEqual(cleanIssues, [], 'sanitized public configuration and factory defaults must pass');

const secretValues = [
    'lab-private-ssid',
    'lab-private-password',
    'custom-ap-password',
    'mqtt-client-identity',
    'mqtt-private-user',
    'mqtt-private-password',
    'live-device-id',
    'live-user-id',
    'nested-mqtt-secret',
    'non-factory-hash'
];
const issues = [];
scanCredentialDocument('docs/bad/network.json', {
    staSSID: secretValues[0],
    staPassword: secretValues[1],
    apPassword: secretValues[2],
    networks: [{ ssid: 'backup-private-ssid', password: 'backup-private-password' }]
}, issues, options);
scanCredentialDocument('docs/bad/protocol.json', {
    mqtt: {
        clientId: secretValues[3],
        username: secretValues[4],
        password: secretValues[5]
    }
}, issues, options);
scanCredentialDocument('docs/bad/device.json', {
    deviceId: secretValues[6],
    productNumber: 1835,
    userId: secretValues[7]
}, issues, options);
scanCredentialDocument('docs/bad/bundle.json', {
    type: 'fastbee-config-bundle',
    files: [{
        name: 'protocol.json',
        content: JSON.stringify({ mqtt: { mqttSecret: secretValues[8] } })
    }]
}, issues, options);
scanCredentialDocument('docs/bad/users.json', {
    users: [{ passwordHash: secretValues[9], salt: 'non-factory-salt' }]
}, issues, options);

assert(issues.some((issue) => issue.includes("'staSSID'")), 'STA identity must be rejected');
assert(issues.some((issue) => issue.includes("'staPassword'")), 'STA password must be rejected');
assert(issues.some((issue) => issue.includes("'apPassword'")), 'custom AP password must be rejected');
assert(issues.some((issue) => issue.includes("'clientId'")), 'MQTT client identity must be rejected');
assert(issues.some((issue) => issue.includes("'deviceId'")), 'device identity must be rejected');
assert(issues.some((issue) => issue.includes('bundle.json::protocol.json')), 'bundle content must be scanned');
assert(issues.some((issue) => issue.includes("'passwordHash'")), 'non-factory authentication hashes must be rejected');
secretValues.forEach((secret) => {
    assert(!issues.some((issue) => issue.includes(secret)), 'credential values must never appear in diagnostics');
});

const privatePathIssues = [];
validateForbiddenTracked([
    '.device-config-backups/latest/example/network.json',
    'docs/.device-config-backups/example/network.json'
], privatePathIssues);
assert(privatePathIssues.some((issue) => issue.startsWith('.device-config-backups/')),
    'repository-root device configuration backups must never be tracked');
assert(privatePathIssues.some((issue) => issue.startsWith('docs/.device-config-backups/')),
    'legacy docs device configuration backups must never be tracked');

const ignoreResult = spawnSync(
    'git',
    ['check-ignore', '--no-index', '-q', '.device-config-backups/latest/example/network.json'],
    { cwd: ROOT, encoding: 'utf8', windowsHide: true }
);
assert.ifError(ignoreResult.error);
assert.strictEqual(ignoreResult.status, 0,
    'repository-root .device-config-backups/ must be ignored by Git');

console.log('FastBee public configuration credential gate contract OK');

require('./test-repository-retention');
