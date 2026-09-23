'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const zlib = require('zlib');
const { verifyWebReleaseAssets } = require('./verify-web-release-assets');

const ROOT = path.resolve(__dirname, '..');
const platformioText = fs.readFileSync(path.join(ROOT, 'platformio.ini'), 'utf8');
const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-web-release-'));

function writeFixture(environment, maxConcurrent, maxWeight) {
    const wwwDir = path.join(tempRoot, environment, 'www');
    fs.mkdirSync(path.join(wwwDir, 'js'), { recursive: true });
    fs.writeFileSync(path.join(wwwDir, 'web-resource-manifest.json'), JSON.stringify({
        environment,
        requestGovernor: { maxConcurrent, maxWeight, maxHeavy: 1 }
    }));
    const source = `const WEB_PROFILE_REQUEST_LIMITS = Object.freeze({\n` +
        `  maxConcurrent: ${maxConcurrent},\n  maxWeight: ${maxWeight}\n});\n`;
    fs.writeFileSync(path.join(wwwDir, 'js', 'chunk-1-core.js.gz'), zlib.gzipSync(source));
    fs.mkdirSync(path.join(wwwDir, 'assets'), { recursive: true });
    ['logo.png', 'favicon.ico'].forEach((name) => {
        fs.copyFileSync(path.join(ROOT, 'data', 'www', 'assets', name), path.join(wwwDir, 'assets', name));
    });
    fs.writeFileSync(path.join(wwwDir, 'index.html.gz'), zlib.gzipSync(
        '<link rel="icon" href="/assets/favicon.ico"><img src="/assets/logo.png?v=fixture">' +
        '<img data-lazy-src="/assets/logo.png">'
    ));
    return wwwDir;
}

try {
    const classic = writeFixture('esp32-F4R0', 1, 1);
    const c3 = writeFixture('esp32c3-F4R0', 1, 1);
    const s3 = writeFixture('esp32s3-F16R8', 2, 2);

    const classicResult = verifyWebReleaseAssets(classic, 'esp32-F4R0', { platformioText });
    const c3Result = verifyWebReleaseAssets(c3, 'esp32c3-F4R0', { platformioText });
    const s3Result = verifyWebReleaseAssets(s3, 'esp32s3-F16R8', { platformioText });
    assert.deepStrictEqual(classicResult.requestGovernor, {
        maxConcurrent: 1, maxWeight: 1, memoryProfile: 'internal-dram'
    });
    assert.deepStrictEqual(c3Result.requestGovernor, {
        maxConcurrent: 1, maxWeight: 1, memoryProfile: 'internal-dram'
    });
    assert.deepStrictEqual(s3Result.requestGovernor, {
        maxConcurrent: 2, maxWeight: 2, memoryProfile: 'psram'
    });
    assert.notStrictEqual(classicResult.treeSha256, s3Result.treeSha256,
        'PSRAM and no-PSRAM Web archives must not collapse to one tree');
    assert.deepStrictEqual(c3Result.htmlAssetReferences, ['assets/favicon.ico', 'assets/logo.png']);

    fs.unlinkSync(path.join(c3, 'assets', 'logo.png'));
    assert.throws(
        () => verifyWebReleaseAssets(c3, 'esp32c3-F4R0', { platformioText }),
        /references missing assets\/logo\.png/,
        'A snapshot which omits tracked data/www/assets must fail before deployment'
    );
    fs.copyFileSync(path.join(ROOT, 'data', 'www', 'assets', 'logo.png'), path.join(c3, 'assets', 'logo.png'));
    fs.unlinkSync(path.join(c3, 'assets', 'favicon.ico'));
    assert.throws(
        () => verifyWebReleaseAssets(c3, 'esp32c3-F4R0', { platformioText }),
        /references missing assets\/favicon\.ico/
    );
    fs.copyFileSync(path.join(ROOT, 'data', 'www', 'assets', 'favicon.ico'), path.join(c3, 'assets', 'favicon.ico'));
    fs.writeFileSync(path.join(c3, 'pages-lazy.html'), '<img data-lazy-src="/assets/lazy-only.png">');
    assert.throws(
        () => verifyWebReleaseAssets(c3, 'esp32c3-F4R0', { platformioText }),
        /references missing assets\/lazy-only\.png/,
        'Uncompressed pages and lazy references must also be covered'
    );
    fs.unlinkSync(path.join(c3, 'pages-lazy.html'));
    assert.strictEqual(verifyWebReleaseAssets(c3, 'esp32c3-F4R0', { platformioText }).treeSha256,
        c3Result.treeSha256, 'Restoring the exact assets must restore the archive digest');

    assert.throws(
        () => verifyWebReleaseAssets(classic, 'esp32s3-F16R8', { platformioText }),
        /manifest belongs to esp32-F4R0/
    );
    const invalid = writeFixture('esp32s3-F16R8-invalid', 1, 1);
    const invalidManifest = JSON.parse(fs.readFileSync(path.join(invalid, 'web-resource-manifest.json'), 'utf8'));
    invalidManifest.environment = 'esp32s3-F16R8';
    fs.writeFileSync(path.join(invalid, 'web-resource-manifest.json'), JSON.stringify(invalidManifest));
    assert.throws(
        () => verifyWebReleaseAssets(invalid, 'esp32s3-F16R8', { platformioText }),
        /manifest Request Governor is 1\/1, expected 2\/2/
    );
} finally {
    fs.rmSync(tempRoot, { recursive: true, force: true });
}

console.log('Web release archive contract OK: target profiles and HTML asset references verified');
