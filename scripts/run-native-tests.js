'use strict';

const { spawn } = require('child_process');
const path = require('path');
const { runNodeScriptUnderFirmwareLock } = require('./firmware-build-lock-runner');

const ROOT = path.resolve(__dirname, '..');
const ANSI_ESCAPE = /\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])/g;

function parseSummary(output) {
    const clean = output.replace(ANSI_ESCAPE, '');
    const pattern = /(\d+)\s+test cases?:\s+(\d+)\s+succeeded(?:,\s*(\d+)\s+failed)?/gi;
    let match;
    let last = null;
    while ((match = pattern.exec(clean)) !== null) {
        last = {
            total: Number(match[1]),
            passed: Number(match[2]),
            failed: Number(match[3] || 0)
        };
    }
    return last;
}

function isWindowsSuccessExit(code, output, platform = process.platform) {
    if (platform !== 'win32' || ![-1, 4294967295].includes(code)) return false;
    const summary = parseSummary(output);
    return Boolean(summary && summary.total > 0 && summary.passed === summary.total && summary.failed === 0);
}

function nativeInvocation(platform = process.platform) {
    if (platform === 'win32') {
        return {
            command: 'powershell.exe',
            args: [
                '-NoProfile',
                '-ExecutionPolicy', 'Bypass',
                '-File', path.join(ROOT, 'scripts', 'test-all.ps1'),
                '-Checks', 'native'
            ]
        };
    }
    return { command: 'pio', args: ['test', '-e', 'native'] };
}

function main() {
    // PlatformIO can return -1 on Windows before its test subprocess has
    // finished writing the summary. test-all.ps1 owns the proven Windows
    // wait/log/summary workaround, so keep the npm entry point on that path.
    const invocation = nativeInvocation();
    const child = spawn(invocation.command, invocation.args, {
        cwd: ROOT,
        env: process.env,
        windowsHide: true,
        stdio: ['ignore', 'pipe', 'pipe']
    });
    let output = '';

    child.stdout.on('data', (chunk) => {
        output += chunk.toString();
        process.stdout.write(chunk);
    });
    child.stderr.on('data', (chunk) => {
        output += chunk.toString();
        process.stderr.write(chunk);
    });
    child.on('error', (error) => {
        console.error(`Unable to start PlatformIO: ${error.message}`);
        process.exitCode = 1;
    });
    child.on('close', (code, signal) => {
        if (process.exitCode) return;
        if (code === 0) {
            process.exitCode = 0;
            return;
        }
        if (isWindowsSuccessExit(code, output)) {
            console.warn(`PlatformIO returned ${code} on Windows, but its current-run summary reports all tests passed.`);
            process.exitCode = 0;
            return;
        }
        const suffix = signal ? ` (signal ${signal})` : '';
        console.error(`Native tests failed with exit code ${code}${suffix}.`);
        process.exitCode = 1;
    });
}

if (require.main === module) {
    try {
        const lockExitCode = runNodeScriptUnderFirmwareLock({
            label: 'run-native-tests',
            environment: 'native'
        });
        if (lockExitCode === null) main();
        else process.exitCode = lockExitCode;
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}

module.exports = { isWindowsSuccessExit, nativeInvocation, parseSummary };
