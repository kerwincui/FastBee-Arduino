'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT_DIR = path.join(__dirname, '..');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');

const REQUIRED_GZIP_ASSETS = [
    'index.html.gz',
    'css/main.css.gz',
    'js/chunk-1-core-a.js.gz',
    'js/chunk-2-core-b.js.gz',
    'js/chunk-3-i18n-engine.js.gz',
    'js/chunk-8-state-1.js.gz',
    'js/chunk-9-state-2.js.gz',
    'js/modules/dashboard.js.gz',
    'js/modules/protocol.js.gz',
    'js/modules/protocol-full-config.js.gz',
    'js/modules/protocol-modbus-rtu.js.gz',
    'js/modules/protocol-modbus-control.js.gz',
    'pages/dashboard.html.gz',
    'pages/device.html.gz',
    'pages/protocol.html.gz',
    'pages/fragments/protocol-mqtt.html.gz',
    'pages/fragments/protocol-modbus-rtu.html.gz'
];

const COMPRESSIBLE_EXTENSIONS = new Set(['.html', '.css', '.js']);
const failures = [];

function normalizeRel(filePath) {
    return path.relative(WWW_DIR, filePath).replace(/\\/g, '/');
}

function walkDir(dir, files) {
    if (!fs.existsSync(dir)) return;
    fs.readdirSync(dir).forEach((entry) => {
        const fullPath = path.join(dir, entry);
        const stat = fs.statSync(fullPath);
        if (stat.isDirectory()) {
            walkDir(fullPath, files);
        } else if (stat.isFile()) {
            files.push(fullPath);
        }
    });
}

function fail(message) {
    failures.push(message);
    console.error(`FAIL ${message}`);
}

function assert(condition, message) {
    if (!condition) fail(message);
}

function readGzipText(relPath) {
    const filePath = path.join(WWW_DIR, relPath);
    assert(fs.existsSync(filePath), `missing ${relPath}`);
    if (!fs.existsSync(filePath)) return '';
    try {
        return zlib.gunzipSync(fs.readFileSync(filePath)).toString('utf8');
    } catch (error) {
        fail(`cannot gunzip ${relPath}: ${error.message}`);
        return '';
    }
}

function checkRequiredAssets() {
    REQUIRED_GZIP_ASSETS.forEach((relPath) => {
        assert(fs.existsSync(path.join(WWW_DIR, relPath)), `required asset not found: ${relPath}`);
    });
}

function checkCompressedOnly(files) {
    const rawCompressible = files
        .map(normalizeRel)
        .filter((relPath) => !relPath.endsWith('.gz'))
        .filter((relPath) => COMPRESSIBLE_EXTENSIONS.has(path.extname(relPath).toLowerCase()));
    assert(rawCompressible.length === 0, `data/www has uncompressed source files: ${rawCompressible.join(', ')}`);
}

function checkJavascriptSyntax(files) {
    const jsGzipFiles = files
        .map(normalizeRel)
        .filter((relPath) => relPath.endsWith('.js.gz'));

    jsGzipFiles.forEach((relPath) => {
        const source = readGzipText(relPath);
        if (!source) return;
        try {
            // Parse only. Do not execute browser globals.
            new Function(source);
        } catch (error) {
            fail(`JS parse failed in ${relPath}: ${error.message}`);
        }
    });
}

function checkUiRegressionGuards(files) {
    const allJs = files
        .map(normalizeRel)
        .filter((relPath) => relPath.endsWith('.js.gz'))
        .map(readGzipText)
        .join('\n');

    const protocolEntry = readGzipText('js/modules/protocol.js.gz');
    const protocolFull = readGzipText('js/modules/protocol-full-config.js.gz');
    const modbusRtu = readGzipText('js/modules/protocol-modbus-rtu.js.gz');
    const devicePage = readGzipText('pages/device.html.gz');

    assert(allJs.includes('apiBatchGetMany'), 'batchGetMany API missing from built JS');
    assert(allJs.includes('Request result ignored after page navigation'), 'page-navigation stale response guard missing');
    assert(protocolEntry.includes('_setProtocolFragmentLoading'), 'protocol loading placeholder helper missing');
    assert(!/正在加载\s*Modbus RTU/.test(protocolEntry + protocolFull + modbusRtu), 'old Modbus RTU loading detail text leaked into build');
    assert(!/fb-loading-placeholder-detail/.test(protocolEntry + protocolFull + modbusRtu), 'old loading detail DOM leaked into build');

    const userIndex = devicePage.indexOf('id="dev-user-id"');
    const descIndex = devicePage.indexOf('id="dev-description"');
    assert(userIndex >= 0, 'device basic user ID field missing');
    assert(descIndex >= 0, 'device basic description field missing');
    assert(userIndex >= 0 && descIndex >= 0 && userIndex < descIndex, 'device basic field order should be user ID before description');
    assert(devicePage.includes('dev-basic-footer-field'), 'device basic footer layout class missing');
}

function run() {
    const files = [];
    walkDir(WWW_DIR, files);

    checkRequiredAssets();
    checkCompressedOnly(files);
    checkJavascriptSyntax(files);
    checkUiRegressionGuards(files);

    if (failures.length > 0) {
        console.error(`\nWeb smoke test failed: ${failures.length} issue(s)`);
        process.exit(1);
    }

    console.log(`Web smoke test passed: ${files.length} published file(s), ${files.filter((file) => file.endsWith('.gz')).length} gzip asset(s)`);
}

run();
