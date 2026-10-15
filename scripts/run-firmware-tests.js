'use strict';

const fs = require('fs');
const { spawnSync } = require('child_process');
const path = require('path');

const { SUPPORTED_ENVIRONMENTS, BuildArtifactsValidator } = require('./test-build-artifacts');
const { runNodeScriptUnderFirmwareLock } = require('./firmware-build-lock-runner');

const ROOT = path.resolve(__dirname, '..');

function run(command, args) {
    console.log(`> ${command} ${args.join(' ')}`);
    const result = spawnSync(command, args, {
        cwd: ROOT,
        env: process.env,
        stdio: 'inherit',
        windowsHide: true
    });
    if (result.error) throw new Error(`Unable to start ${command}: ${result.error.message}`);
    if (result.status !== 0) throw new Error(`${command} exited with ${result.status}`);
}

function parseEnvironments(args) {
    if (args.length === 0) return SUPPORTED_ENVIRONMENTS;
    const requested = args.flatMap((value) => value.split(',')).filter(Boolean);
    const invalid = requested.filter((value) => !SUPPORTED_ENVIRONMENTS.includes(value));
    if (invalid.length > 0) throw new Error(`Unknown firmware environment(s): ${invalid.join(', ')}`);
    return [...new Set(requested)];
}

// C8: penv 的 platformio core 必须与 scripts/platformio-core-pin.json 钉版一致，
// 不匹配时在构建前明确报错，而非静默放行后依赖复核偶发失败（D11 证据）。
function assertPioCoreVersion() {
    const pinFile = path.join(__dirname, 'platformio-core-pin.json');
    const pin = JSON.parse(fs.readFileSync(pinFile, 'utf8'));
    const expected = String(pin.platformioCore);
    const result = spawnSync('pio', ['--version'], {
        cwd: ROOT,
        env: process.env,
        encoding: 'utf8',
        windowsHide: true
    });
    const output = `${result.stdout || ''}${result.stderr || ''}`;
    const match = output.match(/(\d+\.\d+\.\d+)/);
    const actual = match ? match[1] : 'unknown';
    if (result.error || result.status !== 0) {
        throw new Error(`Unable to run 'pio --version': ${result.error ? result.error.message : output.trim()}`);
    }
    if (actual !== expected) {
        throw new Error(
            `PlatformIO core 版本与钉版不符：期望 ${expected}（scripts/platformio-core-pin.json），实际 ${actual}。` +
            `请执行：~/.platformio/penv/Scripts/python.exe -m pip install platformio==${expected}`
        );
    }
}

function main() {
    const environments = parseEnvironments(process.argv.slice(2));
    assertPioCoreVersion();
    for (const environment of environments) {
        run('pio', ['run', '-e', environment]);
        run('pio', ['run', '-e', environment, '--target', 'buildfs']);
        const validator = new BuildArtifactsValidator({ quiet: false });
        if (!validator.validateCurrentBuild([environment])) {
            validator.printReport();
            throw new Error(`Artifact validation failed for ${environment}`);
        }
    }
    console.log(`Firmware build and filesystem validation passed for ${environments.length} environment(s).`);
}

if (require.main === module) {
    try {
        const requested = process.argv.slice(2);
        const lockExitCode = runNodeScriptUnderFirmwareLock({
            label: 'run-firmware-tests',
            environment: requested.join(',')
        });
        if (lockExitCode === null) main();
        else process.exitCode = lockExitCode;
    } catch (error) {
        console.error(error.message);
        process.exit(1);
    }
}

module.exports = { parseEnvironments, assertPioCoreVersion };
