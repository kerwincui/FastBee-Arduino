'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const zlib = require('zlib');
const vm = require('vm');
const { minifyRuntimeJS } = require('./minify-js');
const { verifyWebReleaseAssets } = require('./verify-web-release-assets');

const ROOT = path.resolve(__dirname, '..');
const platformioText = fs.readFileSync(path.join(ROOT, 'platformio.ini'), 'utf8');
const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-web-release-'));

// Exercise published runtime semantics, not merely the minifier's byte count.
const runtimeFixture = [
    'globalThis.AppState = { publicAction: function publicAction(longParameter) {',
    '  const readableLocal = longParameter + 1;',
    '  return { publicValue: readableLocal, url: "https://host/a//b", marker: "/*keep*/",',
    '    literal: `value // ${readableLocal}`, regex: /a[\\/]b/.test("a/b"), name: publicAction.name };',
    '} };',
    'globalThis.answer = AppState["publicAction"](41);'
].join(String.fromCharCode(10));
const originalContext = {}, minifiedContext = {};
vm.runInNewContext(runtimeFixture, originalContext);
const runtimeMinified = minifyRuntimeJS(runtimeFixture);
vm.runInNewContext(runtimeMinified, minifiedContext);
assert.deepStrictEqual(JSON.parse(JSON.stringify(minifiedContext.answer)),
    JSON.parse(JSON.stringify(originalContext.answer)));
assert.strictEqual(minifiedContext.answer.name, 'publicAction');
assert.ok(runtimeMinified.includes('publicAction') && runtimeMinified.includes('publicValue'));
assert.ok(!runtimeMinified.includes('readableLocal'), 'Local bindings should be reduced');
assert.strictEqual(minifyRuntimeJS(runtimeFixture), runtimeMinified, 'Publish output must be deterministic');
assert.throws(() => minifyRuntimeJS('function broken('), { name: 'SyntaxError' },
    'Malformed source must fail the build instead of publishing a partial script');

// Core chunks execute as separate classic scripts, sharing top-level lexical
// bindings and public properties. Local renaming must not break that boundary.
const chunkFixtures = [
    'const chunkSharedConfig = { publicLimit: 2 }; var chunkSharedState = { calls: 0 };' +
        'function chunkPublicAction(readableArgument) { const readableLocal = readableArgument + chunkSharedConfig.publicLimit;' +
        ' chunkSharedState.calls++; return readableLocal; }',
    'const chunkPublicApi = { invoke: chunkPublicAction };' +
        'globalThis.chunkListener = function stableListenerName(eventValue) { return chunkPublicApi.invoke(eventValue); };',
    'globalThis.chunkResult = { value: chunkListener(40), count: chunkSharedState.calls,' +
        ' name: chunkPublicAction.name, listenerName: chunkListener.name,' +
        ' sameReference: chunkPublicApi.invoke === chunkPublicAction };'
];
const originalChunks = vm.createContext({}), publishedChunks = vm.createContext({});
for (const source of chunkFixtures) {
    vm.runInContext(source, originalChunks);
    vm.runInContext(minifyRuntimeJS(source), publishedChunks);
}
assert.deepStrictEqual(JSON.parse(JSON.stringify(publishedChunks.chunkResult)),
    JSON.parse(JSON.stringify(originalChunks.chunkResult)));
assert.deepStrictEqual(JSON.parse(JSON.stringify(publishedChunks.chunkResult)),
    { value: 42, count: 1, name: 'chunkPublicAction', listenerName: 'stableListenerName', sameReference: true });
const pressureFixture = '(function(){const WEB_PROFILE_REQUEST_LIMITS=Object.freeze({maxConcurrent:1,maxWeight:1});' +
    'globalThis.pressure=WEB_PROFILE_REQUEST_LIMITS;})();';
const pressureMinified = minifyRuntimeJS(pressureFixture);
assert.ok(pressureMinified.includes('WEB_PROFILE_REQUEST_LIMITS=Object.freeze({maxConcurrent:1,maxWeight:1})'),
    'Target-specific pressure constants must remain inspectable by the release verifier');
const pressureContext = {};
vm.runInNewContext(pressureMinified, pressureContext);
assert.deepStrictEqual(JSON.parse(JSON.stringify(pressureContext.pressure)), {maxConcurrent:1,maxWeight:1});

// Compression must preserve observable reads, short-circuit guards and the
// order of callbacks; control/configuration code relies on all three.
const effectsFixture = `(function(){
    const calls=[];
    const source={get value(){calls.push('read');return -0;}};
    function read(){return source.value;}
    function rejected(){calls.push('unexpected');throw new Error('must not run');}
    const absent=null;
    const skipped=absent && rejected();
    const first=read(), second=read();
    const zero=Object.is(first,-0) && Object.is(second,-0);
    const fallback=(0 ?? rejected());
    try { (()=>{calls.push('throw');throw new Error('kept');})(); }
    catch(error){calls.push(error.message);}
    finally{calls.push('finally');}
    globalThis.effects={calls,skipped,zero,fallback};
})();`;
const originalEffects={}, publishedEffects={};
vm.runInNewContext(effectsFixture, originalEffects);
vm.runInNewContext(minifyRuntimeJS(effectsFixture), publishedEffects);
assert.deepStrictEqual(JSON.parse(JSON.stringify(publishedEffects.effects)),
    JSON.parse(JSON.stringify(originalEffects.effects)));

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
