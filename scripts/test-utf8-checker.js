'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { checkFile, shouldScanFile } = require('./check-utf8-text');

[
    'browser.spec.ts',
    'browser.spec.tsx',
    'monitor.mjs',
    'deploy.py',
    'deploy.ps1',
    'release.sh'
].forEach((file) => {
    assert.strictEqual(shouldScanFile(file), true, `${file} must be checked as UTF-8 text`);
});

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-utf8-'));
try {
    const powershell = path.join(tempDir, 'deploy.ps1');
    fs.writeFileSync(powershell, Buffer.concat([Buffer.from([0xEF, 0xBB, 0xBF]), Buffer.from('Write-Host ok\n')]));
    assert.deepStrictEqual(checkFile(powershell), [], 'PowerShell BOM must remain compatible with Windows PowerShell 5.1');

    const typescript = path.join(tempDir, 'browser.spec.ts');
    fs.writeFileSync(typescript, Buffer.concat([Buffer.from([0xEF, 0xBB, 0xBF]), Buffer.from('export {};\n')]));
    assert(checkFile(typescript).includes('UTF-8 BOM'), 'TypeScript BOM must be rejected');

    const invalid = path.join(tempDir, 'invalid.py');
    fs.writeFileSync(invalid, Buffer.from([0xC3, 0x28]));
    assert(checkFile(invalid).includes('replacement character'), 'invalid UTF-8 must be rejected');
} finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
}

console.log('UTF-8 checker tests passed');
