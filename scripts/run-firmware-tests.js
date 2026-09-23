'use strict';

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

function main() {
    const environments = parseEnvironments(process.argv.slice(2));
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

module.exports = { parseEnvironments };
