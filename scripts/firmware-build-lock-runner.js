'use strict';

const path = require('node:path');
const { spawnSync } = require('node:child_process');

const ROOT = path.resolve(__dirname, '..');
const WRAPPER = path.join(__dirname, 'with-firmware-build-lock.ps1');
const CHILD_ENV = 'FASTBEE_FIRMWARE_LOCK_NODE_CHILD';
const REQUIRED_LOCK_ENV = [
    'FASTBEE_FIRMWARE_LOCK_TOKEN',
    'FASTBEE_FIRMWARE_LOCK_ROOT',
    'FASTBEE_FIRMWARE_LOCK_OWNER_PID',
    'FASTBEE_FIRMWARE_LOCK_OWNER_STARTED_TICKS'
];

function encodeArguments(args) {
    return Buffer.from(JSON.stringify(args), 'utf8').toString('base64');
}

function runNodeScriptUnderFirmwareLock(options = {}) {
    if (process.env[CHILD_ENV] === '1') {
        const missing = REQUIRED_LOCK_ENV.filter((name) => !process.env[name]);
        if (missing.length > 0) {
            throw new Error(`Firmware lock child marker is missing owner fields: ${missing.join(', ')}`);
        }
        return null;
    }

    const scriptPath = path.resolve(options.scriptPath || process.argv[1]);
    const scriptArguments = options.scriptArguments || process.argv.slice(2);
    const environment = options.environment || '';
    const port = options.port || '';
    const shell = process.platform === 'win32' ? 'powershell.exe' : 'pwsh';
    const shellArguments = ['-NoProfile'];
    if (process.platform === 'win32') shellArguments.push('-ExecutionPolicy', 'Bypass');
    shellArguments.push(
        '-File', WRAPPER,
        '-ProjectRoot', ROOT,
        '-Label', options.label || `node:${path.basename(scriptPath)}`,
        '-Environment', environment,
        '-Port', port,
        '-CommandPath', process.execPath,
        '-ArgumentsBase64', encodeArguments([scriptPath, ...scriptArguments])
    );

    const result = spawnSync(shell, shellArguments, {
        cwd: ROOT,
        env: { ...process.env, [CHILD_ENV]: '1' },
        stdio: 'inherit',
        windowsHide: true
    });
    if (result.error) {
        throw new Error(`Unable to start firmware lock wrapper: ${result.error.message}`);
    }
    if (result.signal) {
        throw new Error(`Firmware lock wrapper terminated by signal ${result.signal}`);
    }
    return Number.isInteger(result.status) ? result.status : 1;
}

module.exports = {
    CHILD_ENV,
    REQUIRED_LOCK_ENV,
    encodeArguments,
    runNodeScriptUnderFirmwareLock
};
