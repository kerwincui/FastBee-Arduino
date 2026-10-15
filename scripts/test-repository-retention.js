'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { auditRepository } = require('./audit-repository-retention');

const REMOVED_LEGACY_SCRIPTS = Object.freeze([
    'scripts/web-test-lightweight.js',
    'scripts/web-browser-test.ps1',
    'scripts/web-browser-test.js',
    'scripts/web-access-check.js',
    'scripts/check-api-health.ps1',
    'scripts/net-check.ps1',
    'scripts/net-discovery.ps1',
    'scripts/serial_capture.py',
    'scripts/serial_monitor.py',
    'scripts/test-esp32c3.ps1',
    'scripts/compare-test-timing.js',
    'scripts/test-boot-script-transport.cjs',
    'scripts/test-device-login-stack.mjs',
    'scripts/test-panel-history-device.mjs',
    'scripts/audit-mdns-observation.py',
    'scripts/recommend-env.ps1',
    'scripts/test-motor-travel-settings-browser.cjs'
]);

const report = auditRepository();

assert.deepStrictEqual(report.issues, [], 'repository retention manifest must match the live inventory');
assert.strictEqual(report.images.length, 20, 'all tracked and committable repository images must be audited');
assert(report.duplicates.some((group) =>
    group.includes('images/case-device-modbus.png') && group.includes('images/device.png')),
'the known Modbus image duplicate must remain visible until quarantine completes');
assert(report.references['images/device.png'].includes('README.md'),
    'the canonical Modbus image must retain its README consumer');
assert(report.references['images/device_01.png'].includes('README.md'),
    'active UI screenshots must not be reported as unused');
assert(report.unreferenced.includes('images/case-device-modbus.png'),
    'the duplicate, unreferenced Modbus image must remain a cleanup candidate');
assert.deepStrictEqual(report.invalidScripts, [],
    'no invalid JavaScript may remain after the legacy script quarantine removal');

const manifest = JSON.parse(fs.readFileSync(
    path.join(__dirname, '..', 'docs', 'repository-retention-manifest.json'), 'utf8'));
const removedEntries = new Set(
    manifest.entries.filter((entry) => entry.status === 'removed').map((entry) => entry.path));
REMOVED_LEGACY_SCRIPTS.forEach((script) => {
    assert(removedEntries.has(script), `${script} must be registered as removed in the retention manifest`);
    assert(!fs.existsSync(path.join(__dirname, '..', script)),
        `${script} must be deleted from the working tree`);
});

console.log('Repository retention audit contract OK');
