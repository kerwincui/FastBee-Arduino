'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { createWebAssetReport, formatBytes } = require('./web-asset-report');

const ROOT_DIR = path.join(__dirname, '..');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');

function read(relPath) {
    return fs.readFileSync(path.join(ROOT_DIR, relPath), 'utf8');
}

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
    'js/modules/rule-script.js.gz',
    'pages/fragments/protocol-mqtt.html.gz',
    'pages/fragments/protocol-modbus-rtu.html.gz',
    'pages/rule-script.html.gz'
];
const FULL_GZIP_ASSETS = [
    'js/modules/files.js.gz',
    'js/modules/logs.js.gz',
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

function snippetAround(source, needle, before = 300, after = 500) {
    const index = source.indexOf(needle);
    if (index < 0) return '';
    return source.slice(Math.max(0, index - before), index + after);
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

function checkRoleCaseRegressionGuards() {
    // 2025-06: 角色管理已移除，单管理员模式 —— hasPermission/hasAnyPermission 已完全移除
    const stateSession = read('web-src/js/state-session.js');
    assert(!stateSession.includes('hasPermission'), 'state-session.js must NOT export hasPermission (roles/permissions removed)');
    assert(!stateSession.includes('hasAnyPermission'), 'state-session.js must NOT export hasAnyPermission (roles/permissions removed)');
    // 单管理员模式：不应有角色检查逻辑
    assert(!stateSession.includes("role === 'ADMIN'"), 'state-session.js must NOT use uppercase role comparison (roles removed)');
    assert(!stateSession.includes("role === 'VIEWER'"), 'state-session.js must NOT reference VIEWER role (roles removed)');
}

function checkMenuOrderRegressionGuards() {
    // 2025-06 修复: 规则脚本菜单应放在设备大屏后面
    const indexHtml = read('web-src/index.html');
    const controlIdx = indexHtml.indexOf('data-page="device-control"');
    const ruleIdx = indexHtml.indexOf('data-page="rule-script"');
    assert(controlIdx > 0, 'index.html should contain device-control menu item');
    assert(ruleIdx > 0, 'index.html should contain rule-script menu item');
    assert(ruleIdx > controlIdx, 'rule-script menu should appear AFTER device-control');
}

function checkButtonColorRegressionGuards() {
    // 2025-06 修复: 设备配置保存按钮应使用 fb-btn-primary 而非 fb-btn-warning
    const deviceHtml = read('web-src/pages/device.html');
    assert(deviceHtml.includes('fb-btn-primary fb-btn-action'), 'device save button should use fb-btn-primary');
    // 确保保存按钮区域不包含 fb-btn-warning
    const saveBtnSnippet = snippetAround(deviceHtml, '保存信息', 100, 200);
    assert(!saveBtnSnippet.includes('fb-btn-warning'), 'device "保存信息" button must NOT use fb-btn-warning');
    const ntpBtnSnippet = snippetAround(deviceHtml, '保存NTP配置', 100, 200);
    assert(!ntpBtnSnippet.includes('fb-btn-warning'), 'device "保存NTP配置" button must NOT use fb-btn-warning');
}

function checkGovernorOverloadRegressionGuards() {
    // 2025-06 修复: Modbus 初始化前应等待 1500ms cooldown，请求间隔 600ms
    const coreJs = read('web-src/modules/runtime/device-control/core.js');
    assert(coreJs.includes('setTimeout(resolve, 1500)'), 'Modbus init should start with 1500ms initial cooldown');
    assert(coreJs.includes('setTimeout(resolve, 600)'), 'Modbus inter-request delay should be 600ms');
    assert(!/setTimeout\(resolve, (150|300|800)\)/.test(coreJs), 'Modbus timing must NOT use old values (150/300/800ms)');

    // apiMqttTest 应通过 Governor.enqueue 调度，避免绕过请求调度器
    const govJs = read('web-src/js/request-governor.js');
    const mqttTestMatch = govJs.match(/apiMqttTest\s*=\s*function[\s\S]{0,200}/);
    assert(mqttTestMatch && mqttTestMatch[0].includes('Governor.enqueue'), 'apiMqttTest must use Governor.enqueue (not direct request)');
}

function checkPeriphExecResultsRemoved() {
    // 2025-06 修复: 外设执行页面应已删除“最近执行结果”功能
    const periphHtml = read('web-src/pages/peripheral.html');
    assert(!periphHtml.includes('pe-results-panel'), 'peripheral.html must NOT have pe-results-panel');
    assert(!periphHtml.includes('periph-exec-results-list'), 'peripheral.html must NOT have periph-exec-results-list');
    assert(!periphHtml.includes('periph-exec-results-refresh-btn'), 'peripheral.html must NOT have results refresh button');

    const peJs = read('web-src/modules/runtime/periph-exec.js');
    assert(!peJs.includes("getElementById('periph-exec-results-list')"), 'periph-exec.js must NOT query periph-exec-results-list');
    assert(!peJs.includes('/api/periph-exec/results'), 'periph-exec.js must NOT call /api/periph-exec/results API');
}

function checkModuleLoaderResilience() {
    // 2025-06 修复: 模块加载器应使用指数退避重试，而非固定延迟
    const mlJs = read('web-src/js/module-loader.js');
    assert(mlJs.includes('Math.pow(2, retries)'), 'Module loader should use exponential backoff');
    assert(mlJs.includes('retries < 3'), 'Module loader should retry up to 4 attempts');

    // MQTT 状态轮询应有定期 setInterval 备份
    const mqttJs = read('web-src/modules/runtime/protocol/mqtt-config.js');
    assert(mqttJs.includes('_mqttPollInterval'), 'MQTT status polling should have _mqttPollInterval');
    assert(mqttJs.includes('setInterval'), 'MQTT status polling should use setInterval as backup');
}

function checkBootTimingRegressionGuards() {
    // 2025-06 修复: 首次访问卡顿 - 请求加载应充分错开，避免压垂ESP32
    const indexHtml = read('web-src/index.html');
    // chunk加载间隔应为80ms而非30ms
    assert(indexHtml.includes('delayMs || 80'), 'chunk loading delay should be 80ms (not 30ms)');
    assert(indexHtml.includes("loadSequence(bootChunks, 80)"), 'boot chunk sequence should use 80ms interval');
    // SW注册应延迟5秒
    assert(indexHtml.includes('setTimeout') && indexHtml.includes('5000'), 'SW registration should be delayed 5000ms');
    // SW注册不应使用 window.addEventListener('load') 模式
    assert(!indexHtml.includes("window.addEventListener('load'"), 'SW registration should NOT use load event listener');

    const sessionJs = read('web-src/js/state-session.js');
    // 页面预加载应延迟5000ms，间隔800ms
    assert(sessionJs.includes('delayMs: 800'), 'page preload interval should be 800ms');
    assert(sessionJs.includes('}, 5000)'), 'page preload should be delayed 5000ms');
    // modals加载应延迟3000ms
    assert(sessionJs.includes('}, 3000)'), 'modals loading should be delayed 3000ms');
}

function checkNetworkNotificationRegressionGuards() {
    // 2025-06 修复: 网络设置保存提示应使用“网络设置保存成功”而非“WiFi配置保存成功”
    const networkJs = read('web-src/modules/runtime/network.js');
    assert(!networkJs.includes('WiFi配置保存成功'), 'network.js must NOT contain WiFi配置保存成功');
    assert(networkJs.includes('网络设置保存成功'), 'network.js should contain 网络设置保存成功');

    // 2025-06 修复: loadNetworkStatus 应检测DNS解析错误并跳过重试
    assert(networkJs.includes('_isDnsError'), 'network.js should have _isDnsError helper');
    assert(networkJs.includes('DNS resolution failed'), 'loadNetworkStatus should log DNS errors');

    const i18nZh = read('web-src/i18n/i18n-zh-CN.js');
    assert(!i18nZh.includes("'wifi-save-ok': 'WiFi配置保存成功"), 'i18n zh wifi-save-ok should not say WiFi配置保存成功');

    // 2025-06 修复: 4G 蜂窝面板应包含访问方式提示
    const networkHtml = read('web-src/pages/network.html');
    assert(networkHtml.includes('cellular-access-hint'), 'cellular panel should have access hint');
    assert(networkHtml.includes('fastbee-ap'), 'cellular hint should mention AP SSID');
    assert(networkHtml.includes('192.168.4.1'), 'cellular hint should mention AP IP');
    // 4G 访问提示不应包含 fastbee.local（AP 无 DNS 服务器，mDNS 不可靠）
    const cellularPanel = networkHtml.match(/id="cellular-panel"[\s\S]*?id="lora-panel"/);
    assert(cellularPanel, 'cellular panel section should exist');
    assert(!cellularPanel[0].includes('fastbee.local'), 'cellular access hint should NOT reference mDNS domain');

    // 2025-06 修复: 后端 4G 混合模式必须启动 mDNS
    const nmCpp = read('src/network/NetworkManager.cpp');
    // 4G 初始化路径应包含 startMDNS 调用
    const fourGSection = nmCpp.match(/NET_4G[\s\S]{0,2500}isInitialized = true[\s\S]{0,20}return true/);
    assert(fourGSection, '4G init section should exist');
    assert(fourGSection[0].includes('startMDNS'), '4G hybrid mode must call dnsManager->startMDNS()');
}

function checkWifiSecurityRegressionGuards() {
    // 2025-06: WiFi扫描返回具体加密类型字符串，前端安全下拉选项已细化为 wpa/wpa2/wpa3
    const networkHtml = read('web-src/pages/network.html');
    assert(networkHtml.includes('value="wpa2"'), 'wifi-security select must have wpa2 option');
    assert(networkHtml.includes('data-i18n="wifi-security-wpa2"'), 'wifi-security wpa2 option must have i18n key');
    assert(networkHtml.includes('selected') && networkHtml.includes('wpa2'), 'wpa2 should be the default security option');

    const networkJs = read('web-src/modules/runtime/network.js');
    // 扫描结果显示具体加密标签映射
    assert(networkJs.includes("encryptLabels"), 'network.js should have encryptLabels mapping');
    assert(networkJs.includes("'WPA2'"), 'encryptLabels should include WPA2 label');
    // 选择处理逻辑使用 encryption 字符串直接赋值，不再使用 '0'/'1'
    assert(networkJs.includes("secValue"), 'scan handler should use secValue variable');
    assert(!networkJs.includes("encryption === 'none' ? '0' : '1'"), 'scan handler must NOT use old 0/1 values');
    // 后端扫描返回具体加密类型，不再是 "open"/"secured" 二值
    const wmCpp = read('src/network/WiFiManager.cpp');
    assert(wmCpp.includes('WIFI_AUTH_WPA2_PSK'), 'WiFiManager scan should map WPA2_PSK');
    assert(wmCpp.includes('WIFI_AUTH_WPA3_PSK'), 'WiFiManager scan should map WPA3_PSK');
    assert(!wmCpp.includes('? "open" : "secured"'), 'WiFiManager scan must NOT return just open/secured');
}

function checkRoleManagementRemoved() {
    // 2025-06: 多角色管理功能已移除，系统运行在单管理员模式
    // 确保角色管理相关文件不会被误加回来
    const removedSrcFiles = [
        'include/security/RoleManager.h',
        'src/security/RoleManager.cpp',
        'include/network/handlers/RoleRouteHandler.h',
        'src/network/handlers/RoleRouteHandler.cpp',
        'data/config/roles.json'
    ];
    removedSrcFiles.forEach(f => {
        assert(!fs.existsSync(path.join(ROOT_DIR, f)),
            `[REGRESSION] 已移除的角色管理文件不应存在: ${f}`);
    });

    // 前端角色模块不应存在
    const removedWebModules = [
        'web-src/modules/admin/roles.js'
    ];
    removedWebModules.forEach(f => {
        assert(!fs.existsSync(path.join(ROOT_DIR, f)),
            `[REGRESSION] 已移除的角色管理前端模块不应存在: ${f}`);
    });

    // 角色管理压缩资产不应存在
    const removedGzipAssets = [
        'data/www/js/modules/roles.js.gz'
    ];
    removedGzipAssets.forEach(f => {
        assert(!fs.existsSync(path.join(ROOT_DIR, f)),
            `[REGRESSION] 已移除的角色管理压缩资产不应存在: ${f}`);
    });

    // admin.html 不应包含 roles-page
    if (fs.existsSync(path.join(ROOT_DIR, 'web-src/pages/admin.html'))) {
        const adminHtml = read('web-src/pages/admin.html');
        assert(!adminHtml.includes('roles-page'), 'admin.html must NOT contain roles-page');
    }

    // index.html 不应包含 data-page="roles"
    const indexHtml = read('web-src/index.html');
    assert(!indexHtml.includes('data-page="roles"'), 'index.html must NOT contain data-page="roles"');
}

function checkDeveloperModeAndModalEventBinding() {
    // 2025-06 修复: 模态窗事件必须在 _loadModals 后重新绑定，
    // 否则新增外设/新增规则等按钮打开模态窗后无法保存/取消/关闭

    // 1. state.js 必须包含模态窗事件重绑定机制
    const stateJs = read('web-src/js/state.js');
    assert(stateJs.includes('_registerModalBinder'),
        'state.js must provide _registerModalBinder for modal event rebinding');
    assert(stateJs.includes('_rebindAllModalEvents'),
        'state.js must provide _rebindAllModalEvents to trigger all modal binders');
    assert(stateJs.includes('self._rebindAllModalEvents()'),
        '_loadModals must call _rebindAllModalEvents after modals DOM is ready');

    // 2. peripherals.js 必须将模态窗事件分离并通过 binder 注册
    const peripheralsJs = read('web-src/modules/runtime/peripherals.js');
    assert(peripheralsJs.includes('_bindPeripheralModalEvents'),
        'peripherals.js must have _bindPeripheralModalEvents method for modal events');
    assert(peripheralsJs.includes("_registerModalBinder('peripherals'"),
        'peripherals.js must register modal binder via _registerModalBinder');
    // 确保 setupPeripheralsEvents 不再绑定模态窗内按钮事件
    const setupSnippet = snippetAround(peripheralsJs, 'setupPeripheralsEvents', 0, 800);
    assert(!setupSnippet.includes('close-peripheral-modal'),
        'setupPeripheralsEvents must NOT bind close-peripheral-modal (moved to _bindPeripheralModalEvents)');
    assert(!setupSnippet.includes('save-peripheral-btn'),
        'setupPeripheralsEvents must NOT bind save-peripheral-btn (moved to _bindPeripheralModalEvents)');

    // 3. periph-exec.js 必须将模态窗事件分离并通过 binder 注册
    const periphExecJs = read('web-src/modules/runtime/periph-exec.js');
    assert(periphExecJs.includes('_bindPeriphExecModalEvents'),
        'periph-exec.js must have _bindPeriphExecModalEvents method for modal events');
    assert(periphExecJs.includes("_registerModalBinder('periph-exec'"),
        'periph-exec.js must register modal binder via _registerModalBinder');

    // 4. rule-script.js 必须将模态窗事件分离并通过 binder 注册
    const ruleScriptJs = read('web-src/modules/admin/rule-script.js');
    assert(ruleScriptJs.includes('_bindRuleScriptModalEvents'),
        'rule-script.js must have _bindRuleScriptModalEvents method for modal events');
    assert(ruleScriptJs.includes("_registerModalBinder('rule-script'"),
        'rule-script.js must register modal binder via _registerModalBinder');

    // 5. 外设配置页面必须有开发环境禁用提示条
    const peripheralHtml = read('web-src/pages/peripheral.html');
    assert(peripheralHtml.includes('peripheral-dev-mode-hint'),
        'peripheral.html must have developer-mode-disabled hint bar');
    assert(peripheralsJs.includes('peripheral-dev-mode-hint'),
        'peripherals.js must show/hide developer mode hint bar');

    // 6. 开发环境状态管理: applyDeveloperModeState 必须覆盖关键按钮
    assert(stateJs.includes("'#add-peripheral-btn'"),
        'applyDeveloperModeState must target #add-peripheral-btn');
    assert(stateJs.includes("'#periph-exec-page-add-btn'"),
        'applyDeveloperModeState must target #periph-exec-page-add-btn');

    // 7. 角色权限已移除，前端不应有任何权限检查
    assert(!peripheralsJs.includes('hasPermission') && !peripheralsJs.includes('checkPermission'),
        'peripherals.js must NOT contain permission checks (roles removed)');
    assert(!periphExecJs.includes('hasPermission') && !periphExecJs.includes('checkPermission'),
        'periph-exec.js must NOT contain permission checks (roles removed)');

    // 8. CSS 双态: dev-mode-locked 类必须存在
    const mainCss = read('web-src/css/main.css');
    assert(mainCss.includes('.dev-mode-locked'),
        'main.css must define .dev-mode-locked class for disabled-state styling');

    // 9. state.js 必须包含开发环境状态管理三件套
    assert(stateJs.includes('isDeveloperModeEnabled'),
        'state.js must provide isDeveloperModeEnabled() method');
    assert(stateJs.includes('setDeveloperModeState'),
        'state.js must provide setDeveloperModeState() method');
    assert(stateJs.includes('guardDeveloperModeAction'),
        'state.js must provide guardDeveloperModeAction() method');
    // setDeveloperModeState 必须操作 developer-mode-disabled 类 (双态切换)
    const setDmSnippet = snippetAround(stateJs, 'setDeveloperModeState', 0, 400);
    assert(setDmSnippet.includes('developer-mode-disabled'),
        'setDeveloperModeState must toggle developer-mode-disabled CSS class');

    // 10. periph-exec 提示条嵌入在 peripheral.html 中（非独立页面）
    assert(peripheralHtml.includes('periph-exec-dev-mode-hint'),
        'peripheral.html must have periph-exec developer-mode-disabled hint bar');
    assert(periphExecJs.includes('periph-exec-dev-mode-hint'),
        'periph-exec.js must show/hide developer mode hint bar');

    const ruleScriptHtml = read('web-src/pages/rule-script.html');
    assert(ruleScriptHtml.includes('rule-script-dev-mode-hint'),
        'rule-script.html must have developer-mode-disabled hint bar');
    assert(ruleScriptJs.includes('rule-script-dev-mode-hint'),
        'rule-script.js must show/hide developer mode hint bar');

    // 11. 容量锁定函数存在于各自模块中（非 state.js）
    assert(peripheralsJs.includes('_setPeripheralCapacity'),
        'peripherals.js must provide _setPeripheralCapacity for profile-based button limits');
    assert(periphExecJs.includes('_setPeriphExecCapacity'),
        'periph-exec.js must provide _setPeriphExecCapacity for profile-based button limits');
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
    checkRoleCaseRegressionGuards();
    checkMenuOrderRegressionGuards();
    checkButtonColorRegressionGuards();
    checkGovernorOverloadRegressionGuards();
    checkPeriphExecResultsRemoved();
    checkModuleLoaderResilience();
    checkBootTimingRegressionGuards();
    checkNetworkNotificationRegressionGuards();
    checkWifiSecurityRegressionGuards();
    checkRoleManagementRemoved();
    checkDeveloperModeAndModalEventBinding();
    checkAssetBudgets(profile);
    checkFirstPaintPath(profile);

    if (failures.length > 0) {
        console.error(`\nWeb smoke test failed: ${failures.length} issue(s)`);
        process.exit(1);
    }

    console.log(`Web smoke test passed: profile=${profile}, ${files.length} published file(s), ${files.filter((file) => file.endsWith('.gz')).length} gzip asset(s)`);
}

run();
