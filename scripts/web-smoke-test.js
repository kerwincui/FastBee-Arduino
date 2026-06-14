'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { createWebAssetReport, formatBytes } = require('./web-asset-report');

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
    'js/modules/device-config.js.gz',
    'js/modules/periph-exec.js.gz',
    'js/modules/periph-exec-form.js.gz',
    'js/modules/periph-exec-modbus.js.gz',
    'js/modules/network.js.gz',
    'js/modules/admin-bundle.js.gz',
    'js/modules/peripherals.js.gz',
    'pages/dashboard.html.gz',
    'pages/device.html.gz',
    'pages/network.html.gz',
    'pages/peripheral.html.gz',
    'pages/protocol.html.gz',
    'pages/modals.html.gz'
];
const STANDARD_GZIP_ASSETS = [
    'sw.js.gz',
    'js/modules/protocol-full-config.js.gz',
    'js/modules/protocol-modbus-rtu.js.gz',
    'js/modules/protocol-modbus-control.js.gz',
    'js/modules/device-control.js.gz',
    'js/modules/device-control-view.js.gz',
    'js/modules/device-control-modbus.js.gz',
    'pages/fragments/protocol-mqtt.html.gz',
    'pages/fragments/protocol-modbus-rtu.html.gz'
];
const FULL_GZIP_ASSETS = [
    'js/modules/files.js.gz',
    'js/modules/logs.js.gz',
    'js/modules/roles.js.gz',
    'js/modules/rule-script.js.gz',
    'js/modules/users.js.gz',
    'pages/admin.html.gz',
    'pages/logs.html.gz',
    'pages/rule-script.html.gz',
    'pages/fullscreen.html.gz',
    'pages/fragments/device-ota.html.gz',
    'pages/fragments/protocol-coap.html.gz',
    'pages/fragments/protocol-http.html.gz',
    'pages/fragments/protocol-tcp.html.gz'
];

const COMPRESSIBLE_EXTENSIONS = new Set(['.html', '.css', '.js']);
const WEB_ASSET_BUDGETS = {
    lite: {
        totalGzip: 200 * 1024,
        bootGzip: 56 * 1024,
        singleAssetGzip: 28 * 1024,
        pageBundleGzip: 12 * 1024
    },
    standard: {
        totalGzip: 240 * 1024,
        bootGzip: 60 * 1024,
        singleAssetGzip: 30 * 1024,
        pageBundleGzip: 13 * 1024
    },
    full: {
        totalGzip: 280 * 1024,
        bootGzip: 64 * 1024,
        singleAssetGzip: 32 * 1024,
        pageBundleGzip: 14 * 1024
    }
};
const failures = [];

function normalizeRel(filePath) {
    return path.relative(WWW_DIR, filePath).replace(/\\/g, '/');
}

function assetExists(relPath) {
    return fs.existsSync(path.join(WWW_DIR, relPath));
}

function inferPublishedProfile() {
    if (assetExists('pages/admin.html.gz') || assetExists('js/modules/files.js.gz')) {
        return 'full';
    }
    if (assetExists('js/modules/device-control.js.gz') ||
        assetExists('pages/fragments/protocol-modbus-rtu.html.gz')) {
        return 'standard';
    }
    return 'lite';
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

function checkRequiredAssets(profile) {
    let required = REQUIRED_GZIP_ASSETS.slice();
    if (profile === 'standard' || profile === 'full') {
        required = required.concat(STANDARD_GZIP_ASSETS);
    }
    if (profile === 'full') {
        required = required.concat(FULL_GZIP_ASSETS);
    }

    required.forEach((relPath) => {
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
    const protocolFull = assetExists('js/modules/protocol-full-config.js.gz')
        ? readGzipText('js/modules/protocol-full-config.js.gz')
        : '';
    const modbusRtu = assetExists('js/modules/protocol-modbus-rtu.js.gz')
        ? readGzipText('js/modules/protocol-modbus-rtu.js.gz')
        : '';
    const devicePage = readGzipText('pages/device.html.gz');

    const deviceConfig = readGzipText('js/modules/device-config.js.gz');

    // Config transfer multi-select modal logic
    assert(deviceConfig.includes('_showConfigTransferModal'), 'config transfer modal show method missing');
    assert(deviceConfig.includes('config-transfer-select-all'), 'config transfer select-all logic missing');
    assert(deviceConfig.includes('config-transfer-list'), 'config transfer list rendering logic missing');
    assert(deviceConfig.includes('config-transfer-confirm-btn'), 'config transfer confirm handler missing');
    assert(deviceConfig.includes('config-transfer-cancel-btn'), 'config transfer cancel handler missing');
    assert(deviceConfig.includes('new Promise'), 'config transfer modal should use Promise pattern');

    if (assetExists('sw.js.gz')) {
        const serviceWorker = readGzipText('sw.js.gz');
        assert(serviceWorker.includes('withCacheTimestamp'), 'service worker cache timestamp helper missing');
        assert(serviceWorker.includes('sw-cached-at'), 'service worker cache timestamp header missing');
    }
    assert(allJs.includes('apiBatchGetMany'), 'batchGetMany API missing from built JS');
    assert(allJs.includes('Request result ignored after page navigation'), 'page-navigation stale response guard missing');
    assert(protocolEntry.includes('_setProtocolFragmentLoading'), 'protocol loading placeholder helper missing');
    assert(!/正在加载\s*Modbus RTU/.test(protocolEntry + protocolFull + modbusRtu), 'old Modbus RTU loading detail text leaked into build');
    assert(!/fb-loading-placeholder-detail/.test(protocolEntry + protocolFull + modbusRtu), 'old loading detail DOM leaked into build');
    assert(allJs.includes('d.connecting'), 'MQTT status UI connecting-state guard missing');
    assert(allJs.includes('autoStartStarted'), 'MQTT status UI auto-start guard missing');
    assert(allJs.includes('mqtt-status-connecting'), 'MQTT status connecting badge class missing');

    const userIndex = devicePage.indexOf('id="dev-user-id"');
    const descIndex = devicePage.indexOf('id="dev-description"');
    assert(userIndex >= 0, 'device basic user ID field missing');
    assert(descIndex >= 0, 'device basic description field missing');
    assert(userIndex >= 0 && descIndex >= 0 && userIndex < descIndex, 'device basic field order should be user ID before description');
    assert(devicePage.includes('dev-basic-footer-field'), 'device basic footer layout class missing');

    // Config transfer modal structure
    assert(devicePage.includes('dev-config-import-file'), 'config transfer file input missing');
    assert(devicePage.includes('dev-config-import-btn'), 'config transfer import button missing');
    assert(devicePage.includes('dev-config-export-btn'), 'config transfer export button missing');
    const modalsPage = readGzipText('pages/modals.html.gz');
    assert(modalsPage.includes('config-transfer-modal'), 'config transfer modal container missing');
    assert(modalsPage.includes('config-transfer-select-all'), 'config transfer select-all checkbox missing');
    assert(modalsPage.includes('config-transfer-list'), 'config transfer list container missing');
    assert(modalsPage.includes('config-transfer-confirm-btn'), 'config transfer confirm button missing');
    assert(modalsPage.includes('config-transfer-cancel-btn'), 'config transfer cancel button missing');
}

function checkPaginationRegressionGuards(files) {
    const allJs = files
        .map(normalizeRel)
        .filter((relPath) => relPath.endsWith('.js.gz'))
        .map(readGzipText)
        .join('\n');

    // Pagination component must exist
    assert(allJs.includes('renderPagination'), 'renderPagination helper missing from built JS');
    assert(allJs.includes('u-pagination'), 'pagination CSS class missing from built JS');
    assert(allJs.includes('onPageChange'), 'pagination onPageChange callback missing');

    // Peripheral config pagination
    assert(allJs.includes('_periphCurrentPage'), 'peripheral config page state missing');
    assert(allJs.includes('_periphPageSize') || allJs.includes('periphPageSize'), 'peripheral config pageSize state missing');
    assert(allJs.includes('periph-pagination'), 'peripheral pagination container ID missing');

    // Periph-exec pagination
    assert(allJs.includes('_peCurrentPage'), 'periph-exec page state missing');
    assert(allJs.includes('_pePageSize') || allJs.includes('pePageSize'), 'periph-exec pageSize state missing');
    assert(allJs.includes('periph-exec-pagination'), 'periph-exec pagination container ID missing');

    // API URL must include pagination params
    assert(allJs.includes('page=') && allJs.includes('pageSize='), 'API pagination query params missing from JS');

    // Pagination uses server-returned total for page calculation
    assert(allJs.includes('totalPages') || allJs.includes('Math.ceil'), 'pagination page calculation logic missing');
}

function checkAssetBudgets(profile) {
    const budgets = WEB_ASSET_BUDGETS[profile] || WEB_ASSET_BUDGETS.full;
    const report = createWebAssetReport({ topN: 999 });

    assert(
        report.totals.gzip <= budgets.totalGzip,
        `web total gzip ${formatBytes(report.totals.gzip)} exceeds ${profile} budget ${formatBytes(budgets.totalGzip)}`
    );
    assert(
        report.boot.gzip <= budgets.bootGzip,
        `boot gzip ${formatBytes(report.boot.gzip)} exceeds ${profile} budget ${formatBytes(budgets.bootGzip)}`
    );

    const oversizedAssets = report.largest.filter((item) => {
        return (item.gzip || item.raw) > budgets.singleAssetGzip;
    });
    assert(
        oversizedAssets.length === 0,
        `oversized web assets: ${oversizedAssets.map((item) => `${item.path}=${formatBytes(item.gzip || item.raw)}`).join(', ')}`
    );

    const oversizedBundles = report.pageBundles.filter((item) => {
        return item.gzip > budgets.pageBundleGzip;
    });
    assert(
        oversizedBundles.length === 0,
        `runtime bundle gzip exceeds ${profile} budget: ${oversizedBundles.map((item) => `${item.path}=${formatBytes(item.gzip)}`).join(', ')}`
    );
}

function checkFirstPaintPath(profile) {
    const html = readGzipText('index.html.gz');
    if (!html) return;

    // Critical: index.html must inline or reference main.css
    assert(html.includes('main.css') || html.includes('body{'), 'index.html must reference or inline main.css for first paint');

    // Core chunks must be loaded in order
    const coreChunks = ['chunk-1-core-a', 'chunk-2-core-b', 'chunk-3-i18n-engine'];
    const corePresent = coreChunks.filter((name) => html.includes(name));
    assert(corePresent.length > 0, 'index.html must reference at least one core chunk for boot');

    // Dashboard module should be loaded on first page
    assert(html.includes('dashboard'), 'index.html must reference dashboard module for first page');

    // Service worker registration for offline caching
    const swJs = assetExists('sw.js.gz') ? readGzipText('sw.js.gz') : '';
    if (profile === 'standard' || profile === 'full') {
        assert(swJs.length > 0, `${profile} profile must include service worker for offline caching`);
    }
    if (swJs) {
        assert(swJs.includes('withCacheTimestamp'), 'service worker must use cache timestamp for versioning');
    }

    // i18n language bundles (e.g. i18n-zh-CN.js) should not be loaded synchronously in index.html
    const i18nLangRefs = (html.match(/i18n-[a-z]{2,3}(-[A-Z]{2,4})?\.js/g) || []).length;
    assert(i18nLangRefs === 0, 'i18n language bundles should be loaded on demand, not in index.html');
}

function run() {
    const files = [];
    walkDir(WWW_DIR, files);
    const profile = inferPublishedProfile();

    checkRequiredAssets(profile);
    checkCompressedOnly(files);
    checkJavascriptSyntax(files);
    checkUiRegressionGuards(files);
    checkPaginationRegressionGuards(files);
    checkAssetBudgets(profile);
    checkFirstPaintPath(profile);

    if (failures.length > 0) {
        console.error(`\nWeb smoke test failed: ${failures.length} issue(s)`);
        process.exit(1);
    }

    console.log(`Web smoke test passed: profile=${profile}, ${files.length} published file(s), ${files.filter((file) => file.endsWith('.gz')).length} gzip asset(s)`);
}

run();
