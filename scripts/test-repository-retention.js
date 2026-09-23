'use strict';

const assert = require('assert');
const { auditRepository } = require('./audit-repository-retention');

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
assert(report.invalidScripts.includes('scripts/web-test-lightweight.js'),
    'the invalid legacy script must remain explicitly quarantined until removal');

console.log('Repository retention audit contract OK');
