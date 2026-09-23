'use strict';

const assert = require('assert');
const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const shell = process.platform === 'win32' ? 'powershell' : 'pwsh';
const script = path.join(ROOT, 'scripts', 'test-all.ps1');

function runGuard(extraArgs) {
    const started = Date.now();
    const result = spawnSync(shell, [
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', script,
        '-Checks', 'device-smoke',
        ...extraArgs
    ], {
        cwd: ROOT,
        encoding: 'utf8',
        timeout: 10000,
        windowsHide: true
    });
    if (result.error) throw result.error;
    return {
        ...result,
        output: `${result.stdout || ''}\n${result.stderr || ''}`,
        elapsedMs: Date.now() - started
    };
}

const missing = runGuard([]);
assert.notStrictEqual(missing.status, 0, 'device smoke without BaseUrl must fail');
assert.match(missing.output, /requires an explicit STA -BaseUrl/);
assert.ok(missing.elapsedMs < 10000, 'missing URL guard must fail before attempting network access');

const ap = runGuard(['-BaseUrl', 'http://192.168.4.1']);
assert.notStrictEqual(ap.status, 0, 'device AP address must fail');
assert.match(ap.output, /refuses the device AP address 192\.168\.4\.1/);
assert.ok(ap.elapsedMs < 10000, 'AP guard must fail before attempting network access');

const apWithPort = runGuard(['-BaseUrl', 'http://192.168.4.1:80/']);
assert.notStrictEqual(apWithPort.status, 0, 'device AP address with port must fail');
assert.match(apWithPort.output, /refuses the device AP address 192\.168\.4\.1/);

const source = fs.readFileSync(script, 'utf8');
assert.match(source, /\[string\]\$DeviceProfile = "unified"/,
    'the unified firmware profile must be the default');
const expansion = source.indexOf('$Checks = @("doctor", "static", "native", "build"');
const earlyGuard = source.indexOf('$requestedDeviceChecks = @(');
const firstStep = source.indexOf('if ($Checks -contains "doctor")');
assert.ok(expansion >= 0 && earlyGuard > expansion && earlyGuard < firstStep,
    'all-mode device target validation must run before the first expensive test step');

const packageJson = JSON.parse(fs.readFileSync(path.join(ROOT, 'package.json'), 'utf8'));
assert.strictEqual(packageJson.scripts.test, 'npm run test:host',
    'npm test must remain the host regression entry point');
assert.match(packageJson.scripts['test:regression'], /npm run test:host/,
    'test:regression must include host regression');
assert.match(packageJson.scripts['test:regression'], /npm run test:native/,
    'test:regression must include native regression');

const documentedReleaseCommand =
    'powershell -ExecutionPolicy Bypass -Command ".\\scripts\\test-all.ps1 -Checks static,native,build,artifacts"';
for (const relativePath of ['README.md', 'README.en.md', 'test/README.md']) {
    const document = fs.readFileSync(path.join(ROOT, relativePath), 'utf8');
    assert.ok(document.includes(documentedReleaseCommand),
        `${relativePath} must document the verified release regression command`);
}

console.log('FastBee test entry contract OK: commands aligned, explicit STA required, AP address refused');
