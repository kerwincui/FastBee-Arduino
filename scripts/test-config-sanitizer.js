'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { sanitizeBundle, sanitizeDocument, sanitizePath } = require('./sanitize-config-export');
const {
    hashJsonDirectory,
    validateForbiddenTracked,
    validatePrivateBackups
} = require('./validate-project-hygiene');

const network = {
    staSSID: 'private-network',
    staPassword: 'private-password',
    apPassword: 'custom-ap-password',
    networks: [{ ssid: 'backup', password: 'backup-password', priority: 1 }]
};
const sanitizedNetwork = sanitizeDocument('network.json', network);
assert.strictEqual(sanitizedNetwork.staSSID, '');
assert.strictEqual(sanitizedNetwork.staPassword, '');
assert.strictEqual(sanitizedNetwork.apPassword, 'admin123');
assert.deepStrictEqual(sanitizedNetwork.networks, []);
assert.strictEqual(network.staSSID, 'private-network', 'sanitizer must not mutate its source object');

const protocol = sanitizeDocument('protocol.json', {
    mqtt: {
        enabled: true,
        clientId: 'device-client',
        username: 'device-user',
        password: 'device-password',
        mqttSecret: 'device-secret',
        authCode: 'device-auth-code',
        deviceNum: 'device-number',
        userId: '7',
        summary: 'private summary'
    }
});
assert.strictEqual(protocol.mqtt.enabled, false);
['clientId', 'username', 'password', 'mqttSecret', 'authCode', 'deviceNum', 'userId', 'summary']
    .forEach((key) => assert.strictEqual(protocol.mqtt[key], '', `mqtt.${key} must be blank`));

const bundle = sanitizeBundle({
    type: 'fastbee-config-bundle',
    version: 1,
    exportedAt: '2026-09-12T00:00:00Z',
    files: [
        { name: 'auth.json', content: JSON.stringify({ admin: { passwordHash: 'hash', salt: 'salt' } }) },
        { name: 'network.json', content: JSON.stringify(network) },
        { name: 'protocol.json', content: JSON.stringify({ mqtt: { enabled: true, password: 'secret' } }) }
    ]
});
const remote = sanitizeDocument('protocol.json', {
    remoteChannels: [{ id: 'lamp', enabled: true }],
    weatherSources: [{ id: 'outdoor', enabled: true, apiKey: 'private-key',
        authHeader: 'Bearer private-token', url: 'https://weather.invalid/now?key=private-key' }]
});
assert.strictEqual(remote.remoteChannels[0].enabled, false);
assert.strictEqual(remote.weatherSources[0].enabled, false);
assert.strictEqual(remote.weatherSources[0].apiKey, '');
assert.strictEqual(remote.weatherSources[0].authHeader, '');
assert.strictEqual(remote.weatherSources[0].url, 'https://weather.invalid/now');
assert.strictEqual(bundle.sanitized, true);
assert.strictEqual(bundle.exportedAt, undefined);
assert.deepStrictEqual(bundle.files.map((entry) => entry.name), ['network.json', 'protocol.json']);
assert.strictEqual(JSON.parse(bundle.files[1].content).mqtt.password, '');

const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-config-sanitize-'));
try {
    const input = path.join(tempRoot, 'input.json');
    const output = path.join(tempRoot, 'output.json');
    fs.writeFileSync(input, JSON.stringify({ mqtt: { enabled: true, password: 'secret' } }), 'utf8');
    sanitizePath(input, output);
    const written = JSON.parse(fs.readFileSync(output, 'utf8'));
    assert.strictEqual(written.mqtt.password, '');
    assert.strictEqual(JSON.parse(fs.readFileSync(input, 'utf8')).mqtt.password, 'secret');
} finally {
    fs.rmSync(tempRoot, { recursive: true, force: true });
}

const forbiddenTrackedIssues = [];
validateForbiddenTracked([
    'docs/.device-config-backups/2026-09-12/esp32c3-F4R0/bundle.json'
], forbiddenTrackedIssues);
assert.strictEqual(forbiddenTrackedIssues.length, 1,
    'forced tracking of a private device backup must fail the hygiene gate');
assert.match(forbiddenTrackedIssues[0], /private, or internal file must not be tracked/);

const backupRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-private-backup-'));
try {
    const snapshot = path.join(backupRoot, 'fixture');
    const environments = [
        'esp32-F4R0',
        'esp32c3-F4R0',
        'esp32c6-F4R0',
        'esp32s3-F16R8',
        'esp32s3-F8R0'
    ];
    const devices = environments.map((environment, index) => {
        const relative = path.join(environment, 'deployed');
        const deployed = path.join(snapshot, relative);
        fs.mkdirSync(deployed, { recursive: true });
        fs.writeFileSync(path.join(deployed, 'device.json'), JSON.stringify({ index }), 'utf8');
        return {
            environment,
            path: relative.replace(/\\/g, '/'),
            jsonFiles: 1,
            sourceSha256: hashJsonDirectory(deployed).sha256,
            deployedPath: relative.replace(/\\/g, '/'),
            deployedJsonFiles: 1,
            deployedSha256: hashJsonDirectory(deployed).sha256
        };
    });
    fs.writeFileSync(path.join(snapshot, 'manifest.json'), JSON.stringify({
        private: true,
        trackedByGit: false,
        expectedEnvironments: environments,
        devices
    }), 'utf8');

    const validBackupIssues = [];
    validatePrivateBackups(validBackupIssues, backupRoot);
    assert.deepStrictEqual(validBackupIssues, []);

    fs.writeFileSync(
        path.join(snapshot, environments[0], 'deployed', 'device.json'),
        JSON.stringify({ index: 999 }),
        'utf8'
    );
    const tamperedBackupIssues = [];
    validatePrivateBackups(tamperedBackupIssues, backupRoot);
    assert(tamperedBackupIssues.some((issue) => issue.includes('deployedSha256 does not match')),
        'tampering with a private backup must fail the manifest hash check');
} finally {
    fs.rmSync(backupRoot, { recursive: true, force: true });
}

console.log('FastBee config sanitizer contract OK');
