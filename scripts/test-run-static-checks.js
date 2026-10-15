'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const MANIFEST_FILE = path.join(__dirname, 'static-checks.manifest.js');
const RUNNER_FILE = path.join(__dirname, 'run-static-checks.js');
const PACKAGE_FILE = path.join(ROOT_DIR, 'package.json');

// C1: 与改造前（HEAD）的 test:static 链逐项对齐，防止清单漂移；
// validate-libdeps-patches 在仓库中始终以 node 调用（该脚本无 shebang）。
const EXPECTED_COMMANDS = [
    'node scripts/check-utf8-text.js README.md README.en.md docs scripts test web-src include src data/config/peripherals.json data/config/periph_exec.json data/config/users.json',
    'node scripts/validate-test-coverage.js',
    'node scripts/validate-device-api-matrix.js',
    'node scripts/validate-device-config-examples.js',
    'node scripts/validate-mqtt-ntp-lifecycle.js',
    'node scripts/validate-build-matrix.js',
    'node scripts/validate-doc-links.js',
    'node scripts/validate-stability-thresholds.js',
    'node scripts/validate-config-defaults.js',
    'node scripts/validate-i18n.js',
    'node scripts/validate-file-sizes.js',
    'node scripts/validate-rule-script-profile.js',
    // scripts/validate-libdeps-patches.js 无 shebang，仓库内一直用 node 调用（HEAD package.json 同）。
    'node scripts/validate-libdeps-patches.js',
    'node scripts/validate-feature-budgets.js',
    'node scripts/validate-project-hygiene.js',
    'node scripts/test-fim-manifest-validator.js',
    'node scripts/validate-generic-architecture.js',
    'node scripts/validate-network-ui.js',
    'npm run test:web-assets',
    'npm run test:meta',
    'git diff --check'
];

function main() {
    assert.ok(fs.existsSync(MANIFEST_FILE), 'missing scripts/static-checks.manifest.js');
    assert.ok(fs.existsSync(RUNNER_FILE), 'missing scripts/run-static-checks.js');

    const runnerSource = fs.readFileSync(RUNNER_FILE, 'utf8');
    assert.ok(runnerSource.includes('windowsHide: true'),
        'run-static-checks.js must spawn checks with windowsHide to avoid console window flashes');

    const manifest = require('./static-checks.manifest.js');
    assert.ok(Array.isArray(manifest), 'static checks manifest must export an array');
    assert.ok(manifest.length > 0, 'static checks manifest must not be empty');
    const names = manifest.map((entry) => entry.name);
    assert.strictEqual(new Set(names).size, names.length, 'static checks manifest names must be unique');
    manifest.forEach((entry) => {
        assert.ok(typeof entry.name === 'string' && /^[a-z0-9-]+$/.test(entry.name),
            `invalid manifest name: ${JSON.stringify(entry.name)}`);
        assert.ok(typeof entry.command === 'string' && entry.command.trim().length > 0,
            `empty manifest command for ${entry.name}`);
    });

    manifest.forEach((entry) => {
        entry.command.split(/\s+/).forEach((token) => {
            if (!token.startsWith('scripts/')) return;
            const referenced = path.join(ROOT_DIR, token);
            assert.ok(fs.existsSync(referenced),
                `${entry.name} references missing file: ${token}`);
        });
    });

    const packageJson = JSON.parse(fs.readFileSync(PACKAGE_FILE, 'utf8'));
    assert.strictEqual(packageJson.scripts['test:static'], 'node scripts/run-static-checks.js',
        'package.json test:static must delegate to node scripts/run-static-checks.js');

    assert.deepStrictEqual(manifest.map((entry) => entry.command), EXPECTED_COMMANDS,
        'static checks manifest drifted from the package.json test:static chain');

    console.log('static checks runner contract OK');
}

try {
    main();
} catch (error) {
    console.error(error && error.message ? error.message : error);
    process.exit(1);
}
