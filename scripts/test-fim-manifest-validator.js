'use strict';

// Exercise the FIM validator with generated sparse firmware images so CI does
// not silently pass just because the external documentation repository is
// unavailable. The files have the real logical flash sizes but consume almost
// no physical disk space on filesystems that support sparse files.
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const VALIDATOR = path.join(__dirname, 'validate-fim-manifest.js');
const ENVIRONMENTS = [
    'esp32-F4R0',
    'esp32-F8R0',
    'esp32-F8R8',
    'esp32c3-F4R0',
    'esp32c6-F4R0',
    'esp32c6-F8R0',
    'esp32s3-F8R0',
    'esp32s3-F8R8',
    'esp32s3-F16R8'
];

function runValidator(fimDir) {
    return spawnSync(process.execPath, [VALIDATOR, `--fim-dir=${fimDir}`, '--require'], {
        cwd: ROOT,
        encoding: 'utf8'
    });
}

const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-fim-validator-'));
try {
    const entries = ENVIRONMENTS.map((environment) => {
        const match = environment.match(/-F(4|8|16)R(?:0|8)$/);
        if (!match) throw new Error(`Invalid test environment: ${environment}`);
        const flashMb = Number(match[1]);
        const fileName = `fastbee-${environment}.bin`;
        fs.closeSync(fs.openSync(path.join(tempRoot, fileName), 'w'));
        fs.truncateSync(path.join(tempRoot, fileName), flashMb * 1024 * 1024);
        return { env: environment, name: environment, path: fileName, flashMb };
    });
    fs.writeFileSync(
        path.join(tempRoot, 'manifest.json'),
        `${JSON.stringify({ environments: entries }, null, 2)}\n`,
        'utf8'
    );

    const valid = runValidator(tempRoot);
    if (valid.status !== 0) {
        throw new Error(`valid online-flasher manifest was rejected:\n${valid.stdout}${valid.stderr}`);
    }

    const releaseEntries = entries.map((entry) => ({
        environment: entry.env,
        releaseFile: entry.path,
        flashMB: entry.flashMb,
        files: { 'factory-with-fs.bin': entry.path },
        sizes: { [entry.path]: entry.flashMb * 1024 * 1024 }
    }));
    fs.writeFileSync(
        path.join(tempRoot, 'manifest.json'),
        `${JSON.stringify({ environments: releaseEntries }, null, 2)}\n`,
        'utf8'
    );
    const validRelease = runValidator(tempRoot);
    if (validRelease.status !== 0) {
        throw new Error(`valid release manifest was rejected:\n${validRelease.stdout}${validRelease.stderr}`);
    }

    const firstImage = path.join(tempRoot, entries[0].path);
    fs.truncateSync(firstImage, entries[0].flashMb * 1024 * 1024 - 1);
    const invalidSize = runValidator(tempRoot);
    if (invalidSize.status === 0 || !`${invalidSize.stdout}${invalidSize.stderr}`.includes('size')) {
        throw new Error('wrong-size firmware image was not rejected with a size error');
    }

    const missingInput = runValidator(path.join(tempRoot, 'missing'));
    if (missingInput.status === 0 || !`${missingInput.stdout}${missingInput.stderr}`.includes('not found')) {
        throw new Error('required missing FIM directory was not rejected');
    }

    console.log('FIM manifest validator contract OK: flasher/release schemas, wrong-size, and missing-input cases covered');
} finally {
    const resolvedTemp = path.resolve(tempRoot);
    const resolvedOsTemp = path.resolve(os.tmpdir());
    if (resolvedTemp.startsWith(`${resolvedOsTemp}${path.sep}`)) {
        fs.rmSync(resolvedTemp, { recursive: true, force: true });
    }
}
