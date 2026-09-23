'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { findUncompressedTextAssets } = require('./gzip-www');

const ROOT_DIR = path.resolve(__dirname, '..');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');

function readArgValue(prefix) {
    const arg = process.argv.slice(2).find((value) => value.startsWith(prefix));
    return arg ? arg.slice(prefix.length) : '';
}

const stageArg = readArgValue('--stage-dir=');
const publishOnly = process.argv.includes('--publish-only');
const STAGE_DATA_DIR = path.resolve(ROOT_DIR, stageArg || '.pio/test-fs-stage');
const STAGE_WWW_DIR = path.join(STAGE_DATA_DIR, 'www');

function collectFiles(rootDir) {
    const files = [];
    function walk(dir) {
        fs.readdirSync(dir, { withFileTypes: true }).forEach((entry) => {
            const absolutePath = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                walk(absolutePath);
            } else if (entry.isFile()) {
                files.push(path.relative(rootDir, absolutePath).replace(/\\/g, '/'));
            }
        });
    }
    walk(rootDir);
    return files.sort();
}

function gzipTextFiles(rootDir) {
    return collectFiles(rootDir).filter((relPath) =>
        relPath.endsWith('.html.gz') ||
        relPath.endsWith('.js.gz') ||
        relPath.endsWith('.css.gz'));
}

function readResourceManifest(rootDir) {
    const manifestPath = path.join(rootDir, 'web-resource-manifest.json');
    assert.ok(fs.existsSync(manifestPath), `resource manifest is missing: ${manifestPath}`);
    const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8').replace(/^\uFEFF/, ''));
    assert.strictEqual(manifest.runtimeConfigurationAffectsResources, false,
        'runtime features.json must not select filesystem resources');
    assert.ok(manifest.environment, 'resource manifest must identify its PlatformIO environment');
    assert.ok(manifest.compiledCapabilities, 'resource manifest must include compiled capabilities');
    assert.ok(Array.isArray(manifest.resources), 'resource manifest must list published resources');
    return manifest;
}

function verifyReadableGzip(rootDir, relPaths) {
    relPaths.forEach((relPath) => {
        const content = zlib.gunzipSync(fs.readFileSync(path.join(rootDir, relPath)));
        assert.ok(content.length > 0, `${relPath} must contain non-empty gzip data`);
    });
}

assert.ok(fs.existsSync(WWW_DIR), 'data/www must be generated before the gzip publish contract runs');
if (!publishOnly) {
    assert.ok(fs.existsSync(STAGE_WWW_DIR), `LittleFS staging directory is missing: ${STAGE_WWW_DIR}`);
}

const publishRaw = findUncompressedTextAssets(WWW_DIR);
assert.deepStrictEqual(publishRaw, [], `data/www contains raw HTML/JS/CSS: ${publishRaw.join(', ')}`);

const publishGzip = gzipTextFiles(WWW_DIR);
assert.ok(publishGzip.includes('index.html.gz'), 'data/www/index.html.gz must be published');

const publishManifest = readResourceManifest(WWW_DIR);
const expectedGzip = publishManifest.resources
    .filter((relPath) => /\.(?:html|js|css)$/.test(relPath))
    .map((relPath) => `${relPath}.gz`)
    .sort();
assert.deepStrictEqual(publishGzip, expectedGzip,
    'gzip publish set must exactly match the compile-capability resource manifest');

const expectedHtml = publishManifest.resources.filter((relPath) => relPath.endsWith('.html')).sort();
const publishedHtml = publishGzip
    .filter((relPath) => relPath.endsWith('.html.gz'))
    .map((relPath) => relPath.slice(0, -3));
assert.deepStrictEqual(publishedHtml, expectedHtml,
    'every manifest-selected HTML resource must have exactly one .html.gz publish artifact');

verifyReadableGzip(WWW_DIR, publishGzip);

if (!publishOnly) {
    const stageRaw = findUncompressedTextAssets(STAGE_WWW_DIR);
    assert.deepStrictEqual(stageRaw, [], `LittleFS staging contains raw HTML/JS/CSS: ${stageRaw.join(', ')}`);

    const stageGzip = gzipTextFiles(STAGE_WWW_DIR);
    assert.ok(stageGzip.includes('index.html.gz'), 'staging www/index.html.gz must be published');
    assert.deepStrictEqual(stageGzip, publishGzip,
        'data/www and LittleFS staging must contain the same gzip text asset set');
    const stageManifest = readResourceManifest(STAGE_WWW_DIR);
    assert.deepStrictEqual(stageManifest, publishManifest,
        'LittleFS staging must carry the exact compile-capability resource manifest');
    publishGzip.forEach((relPath) => {
        const published = fs.readFileSync(path.join(WWW_DIR, relPath));
        const staged = fs.readFileSync(path.join(STAGE_WWW_DIR, relPath));
        assert.ok(published.equals(staged), `${relPath} differs between data/www and LittleFS staging`);
    });
    verifyReadableGzip(STAGE_WWW_DIR, stageGzip);
}

console.log(
    `Gzip publish contract OK: ${publishedHtml.length} manifest HTML resource(s), ` +
    `${publishGzip.length} gzip text asset(s), no raw HTML/JS/CSS in ` +
    (publishOnly ? 'data/www' : 'byte-identical publish and staging trees')
);
