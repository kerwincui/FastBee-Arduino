'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { createWebAssetReport } = require('./web-asset-report');
const {
    resolveWebAssetBudgets,
    evaluateWebAssetReport
} = require('./web-quality-gate');
const {
    ASSET_CAPABILITY_REQUIREMENTS,
    capabilityRuleSatisfied
} = require('./web-profile');

const ROOT_DIR = path.join(__dirname, '..');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');

function read(relPath) {
    return fs.readFileSync(path.join(ROOT_DIR, relPath), 'utf8');
}

function hasPageNavigationStaleGuard(source) {
    const execute = source.match(/_execute\s*:\s*function[\s\S]*?(?=_isCooldownError\s*:)/);
    if (!execute) return false;
    // Verify rejection of an old page's response, independently of the text
    // returned by the shared page-abort error helper or local variable names.
    return /if\s*\([^{}]*\.pageGen\s*<\s*[\w$]+\._pageGen[^{}]*\)\s*\{\s*[\w$]+\.reject\s*\(/.test(execute[0]) &&
        /\._pageAborted\s*=\s*true/.test(source);
}

const REQUIRED_GZIP_ASSETS = [
    'index.html.gz',
    'css/main.css.gz',
    'js/chunk-1-core.js.gz',
    'js/chunk-2-app.js.gz',
    'js/chunk-3-state.js.gz',
    'js/modules/dashboard.js.gz',
    'js/modules/protocol.js.gz',
    'js/modules/device-config.js.gz',
    'js/modules/periph-exec.js.gz',
    'js/modules/periph-exec-form.js.gz',
    'js/modules/network.js.gz',
    'js/modules/feature-config.js.gz',
    'js/modules/admin-bundle.js.gz',
    'js/modules/peripherals.js.gz',
    'js/modules/peripheral-save.js.gz',
    'js/modules/peripheral-io.js.gz',
    'pages/dashboard.html.gz',
    'pages/device.html.gz',
    'pages/network.html.gz',
    'pages/peripheral.html.gz',
    'pages/protocol.html.gz',
    'pages/features.html.gz',
    'pages/modals.html.gz',
    'pages/fragments/peripheral-io.html.gz'
];
const RETIRED_ASSETS = [
    'js/modules/files.js',
    'js/modules/files.js.gz',
    'js/modules/peripheral-board.js',
    'js/modules/peripheral-board.js.gz',
    'js/modules/peripheral-experiments.js',
    'js/modules/peripheral-experiments.js.gz'
];

const COMPRESSIBLE_EXTENSIONS = new Set(['.html', '.css', '.js']);
const failures = [];

function normalizeRel(filePath) {
    return path.relative(WWW_DIR, filePath).replace(/\\/g, '/');
}

function assetExists(relPath) {
    return fs.existsSync(path.join(WWW_DIR, relPath));
}

function readPublishedManifest() {
    const manifestPath = path.join(WWW_DIR, 'web-resource-manifest.json');
    assert(fs.existsSync(manifestPath), 'web-resource-manifest.json must be published');
    if (!fs.existsSync(manifestPath)) return { resources: [], compiledCapabilities: {}, environment: 'unknown' };
    try {
        return JSON.parse(fs.readFileSync(manifestPath, 'utf8').replace(/^\uFEFF/, ''));
    } catch (error) {
        fail(`cannot parse web-resource-manifest.json: ${error.message}`);
        return { resources: [], compiledCapabilities: {}, environment: 'unknown' };
    }
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

function checkRequiredAssets(manifest) {
    REQUIRED_GZIP_ASSETS.forEach((relPath) => {
        assert(fs.existsSync(path.join(WWW_DIR, relPath)), `required asset not found: ${relPath}`);
    });

    const declared = new Set(Array.isArray(manifest.resources) ? manifest.resources : []);
    const context = { compiledCapabilities: manifest.compiledCapabilities || {} };
    Object.entries(ASSET_CAPABILITY_REQUIREMENTS).forEach(([relPath, requirements]) => {
        const expected = capabilityRuleSatisfied(requirements, context);
        const published = declared.has(relPath) && assetExists(`${relPath}.gz`);
        assert(published === expected,
            `${relPath} must be ${expected ? 'published' : 'omitted'} for compiled capabilities`);
    });
}

function checkRetiredAssetsAreAbsent() {
    RETIRED_ASSETS.forEach((relPath) => {
        assert(!assetExists(relPath), `retired asset must not be published: ${relPath}`);
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

function checkUiRegressionGuards(files, manifest) {
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
    const otaPage = assetExists('pages/fragments/device-ota.html.gz')
        ? readGzipText('pages/fragments/device-ota.html.gz')
        : '';
    const mainCss = readGzipText('css/main.css.gz');

    const deviceConfig = readGzipText('js/modules/device-config.js.gz');
    const peripheralsCore = readGzipText('js/modules/peripherals.js.gz');
    const peripheralSave = readGzipText('js/modules/peripheral-save.js.gz');
    const peripheralIo = readGzipText('js/modules/peripheral-io.js.gz');
    const capabilityContext = {
        compiledCapabilities: manifest.compiledCapabilities || {}
    };
    const otaCompiled = capabilityRuleSatisfied(['ota'], capabilityContext);
    const configTransferCompiled = capabilityRuleSatisfied(['configTransfer'], capabilityContext);

    assert(!peripheralsCore.includes('/api/boards/profiles') && !peripheralsCore.includes('/api/peripherals/resources'),
        'board/resource APIs must stay out of the peripherals core');
    assert(/ModuleLoader\.loadModule\(["']peripheral-save["'],[A-Za-z_$][\w$]*\)/.test(peripheralsCore),
        'peripheral save/preflight module must be loaded on demand');
    assert(/ModuleLoader\.loadModule\(["']peripheral-io["'],[A-Za-z_$][\w$]*\)/.test(peripheralsCore),
        'peripheral diagnostics module must be loaded on demand');
    assert(peripheralSave.includes('/api/peripherals/preflight'),
        'peripheral save module preflight API coverage missing');
    assert(peripheralIo.includes("loadFragment('','peripheral-io')"),
        'peripheral diagnostics fragment must be loaded on demand');
    // Config transfer multi-select modal logic
    if (configTransferCompiled) {
        assert(deviceConfig.includes('_showConfigTransferModal'), 'config transfer modal show method missing');
        assert(deviceConfig.includes('config-transfer-select-all'), 'config transfer select-all logic missing');
        assert(deviceConfig.includes('config-transfer-list'), 'config transfer list rendering logic missing');
        assert(deviceConfig.includes('config-transfer-confirm-btn'), 'config transfer confirm handler missing');
        assert(deviceConfig.includes('config-transfer-cancel-btn'), 'config transfer cancel handler missing');
        assert(deviceConfig.includes('new Promise'), 'config transfer modal should use Promise pattern');
    }

    assert(allJs.includes('apiBatchGetMany'), 'batchGetMany API missing from built JS');
    assert(hasPageNavigationStaleGuard(allJs), 'page-navigation stale response guard missing');
    assert(protocolEntry.includes('_setProtocolFragmentLoading'), 'protocol loading placeholder helper missing');
    assert(!/正在加载\s*Modbus RTU/.test(protocolEntry + protocolFull + modbusRtu), 'old Modbus RTU loading detail text leaked into build');
    assert(!/fb-loading-placeholder-detail/.test(protocolEntry + protocolFull + modbusRtu), 'old loading detail DOM leaked into build');
    assert(/!![A-Za-z_$][\w$]*\.connecting\b/.test(protocolEntry),
        'MQTT status UI connecting-state guard missing');
    assert(allJs.includes('autoStartStarted'), 'MQTT status UI auto-start guard missing');
    assert(allJs.includes('mqtt-status-connecting'), 'MQTT status connecting badge class missing');

    const userIndex = devicePage.indexOf('id="dev-user-id"');
    const descIndex = devicePage.indexOf('id="dev-description"');
    assert(userIndex >= 0, 'device basic user ID field missing');
    assert(descIndex >= 0, 'device basic description field missing');
    assert(userIndex >= 0 && descIndex >= 0 && userIndex < descIndex, 'device basic field order should be user ID before description');
    assert(devicePage.includes('dev-basic-footer-field'), 'device basic footer layout class missing');

    // Capability-selected file picker structure. Check every compiled picker,
    // but do not require controls that the publish transform intentionally cut.
    if (configTransferCompiled) {
        const configPicker = snippetAround(devicePage, 'id="dev-config-import-file"', 300, 300);
        assert(configPicker.includes('class="file-input-wrapper u-mb-10"'),
            'config transfer must use the shared file picker layout');
        assert(configPicker.includes('class="fb-btn fb-btn-secondary fb-btn-compact"'),
            'config transfer picker must use the shared button variant');
        assert(devicePage.includes('dev-config-import-btn'), 'config transfer import button missing');
        assert(devicePage.includes('dev-config-export-btn'), 'config transfer export button missing');
        assert(!/<label[^>]*for="dev-config-import-file"[^>]*style=/.test(devicePage),
            'config transfer picker must not depend on inline styles');
    }
    if (otaCompiled) {
        const inlineOtaPicker = snippetAround(devicePage, 'id="dev-ota-file"', 250, 300);
        assert(inlineOtaPicker.includes('class="file-input-wrapper u-mb-10"'),
            'device OTA fallback must use the shared file picker layout');
        assert(inlineOtaPicker.includes('class="fb-btn fb-btn-secondary fb-btn-compact"'),
            'device OTA fallback picker must use the shared button variant');
        assert(!/<label[^>]*for="dev-ota-file"[^>]*style=/.test(devicePage),
            'device OTA fallback picker must not depend on inline styles');
        assert(otaPage.includes('id="ota-file"'), 'OTA fragment file input missing');
        assert(otaPage.includes('class="file-input-wrapper file-input-wrapper-top"'),
            'OTA fragment must use the shared file picker layout');
        assert(otaPage.includes('class="fb-btn fb-btn-secondary fb-btn-compact"'),
            'OTA fragment picker must use the shared button variant');
    }
    const baseButtonRule = mainCss.match(/\.fb-btn\s*\{(?=[^}]*display:\s*inline-flex)([^}]*)\}/);
    assert(baseButtonRule,
        'published CSS must include the shared .fb-btn base style');
    assert(baseButtonRule
        && /border:\s*1px\s+solid\s+var\(--border\)/.test(baseButtonRule[1])
        && !/border:\s*1px\s+solid\s+transparent/.test(baseButtonRule[1]),
        'published buttons must keep a visible shared border');
    assert(/\.file-input-wrapper\s*>\s*label\.fb-btn/.test(mainCss),
        'published CSS must include the shared file picker trigger style');
    if (configTransferCompiled) {
        const modalsPage = readGzipText('pages/modals.html.gz');
        assert(modalsPage.includes('config-transfer-modal'), 'config transfer modal container missing');
        assert(modalsPage.includes('config-transfer-select-all'), 'config transfer select-all checkbox missing');
        assert(modalsPage.includes('config-transfer-list'), 'config transfer list container missing');
        assert(modalsPage.includes('config-transfer-confirm-btn'), 'config transfer confirm button missing');
        assert(modalsPage.includes('config-transfer-cancel-btn'), 'config transfer cancel button missing');
    }
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

function checkAssetBudgets(manifest) {
    const report = createWebAssetReport({ topN: 999 });
    evaluateWebAssetReport(report, resolveWebAssetBudgets(manifest)).forEach((issue) => assert(false, issue));
}

function checkFirstPaintPath(manifest) {
    const html = readGzipText('index.html.gz');
    if (!html) return;

    // Critical: index.html must inline or reference main.css
    assert(html.includes('main.css') || html.includes('body{'), 'index.html must reference or inline main.css for first paint');

    // Core chunks must be loaded in order
    const coreChunks = ['chunk-1-core', 'chunk-2-app', 'chunk-3-state'];
    const corePresent = coreChunks.filter((name) => html.includes(name));
    assert(corePresent.length > 0, 'index.html must reference at least one core chunk for boot');

    // Dashboard module should be loaded on first page
    assert(html.includes('dashboard'), 'index.html must reference dashboard module for first page');

    // Service worker registration for offline caching
    assert(assetExists('sw.js.gz'), `${manifest.environment} must include service worker for offline caching`);

    // i18n language bundles (e.g. i18n-zh-CN.js) should not be loaded synchronously in index.html
    const i18nLangRefs = (html.match(/i18n-[a-z]{2,3}(-[A-Z]{2,4})?\.js/g) || []).length;
    assert(i18nLangRefs === 0, 'i18n language bundles should be loaded on demand, not in index.html');

    const fullscreenHtml = assetExists('pages/fullscreen.html.gz')
        ? readGzipText('pages/fullscreen.html.gz')
        : '';
    if (fullscreenHtml) {
        assert(fullscreenHtml.includes('../js/modules/dashboard-fullscreen.js'),
            'fullscreen page must load its published runtime module');
        assert(!fullscreenHtml.includes('../js/chunk-1-core.js'),
            'fullscreen page must stay independent from the memory-heavy application core chunk');
        assert(!fullscreenHtml.includes('../js/notification.js'),
            'fullscreen page must not load the unused standalone notification module');
    }
    [
        'chunk-1-core-a.js',
        'chunk-2-core-b.js',
        'chunk-3-i18n-engine.js',
        'chunk-4-i18n-zh-1.js',
        'chunk-5-i18n-zh-2.js',
        'chunk-6-i18n-zh-3.js',
        'chunk-7-i18n-zh-4.js',
        'chunk-8-state-1.js',
        'chunk-9-state-2.js'
    ].forEach((obsoleteChunk) => {
        assert(!fullscreenHtml.includes(obsoleteChunk),
            `fullscreen page must not reference obsolete ${obsoleteChunk}`);
    });
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

function checkFeatureConfigJsonBodyGuard() {
    // 2026-08-15 修复: 功能配置保存误用 apiPost（表单编码），后端 features/* 的
    // onData body 回调不被触发导致 400 Missing JSON body；必须用 apiPostJson
    const featureJs = read('web-src/modules/runtime/feature-config.js');
    assert(featureJs.includes("apiPostJson('/api/features/apply'"), 'features/apply must be called via apiPostJson (JSON body)');
    assert(!featureJs.includes("apiPost('/api/features/apply'"), 'features/apply must NOT use form-encoded apiPost');
}

function checkFeatureListRenderingGuard() {
    const featureJs = read('web-src/modules/runtime/feature-config.js');
    const featureHtml = read('web-src/pages/features.html');
    const mainCss = read('web-src/css/main.css');
    const featureSuite = read('test/browser/suites/22-feature-config.spec.ts');

    assert(featureJs.includes('this.renderGroupTables(this._featureState)'),
        'all features must be rendered through the regular grouped tables');
    assert(!featureJs.includes('data-zero-ram-toggle') && !featureJs.includes('features-zero-ram-body'),
        'feature config must not render a separate zero-DRAM section');
    assert(!featureJs.includes('features-unified-hint'),
        'feature config must not inject a redundant footer hint');
    assert(!featureHtml.includes('灰显项表示功能未编译'),
        'features page must not contain the redundant footer explanation');
    assert(!featureHtml.includes('features-summary') && !featureJs.includes('features-summary'),
        'feature config must not render or update the redundant resource summary');
    assert(!featureJs.includes('feature-name') && !mainCss.includes('.feature-name'),
        'feature names must use plain table-cell text without a dedicated style');
    assert(!mainCss.includes('.feature-summary'),
        'removed feature summary must not leave an unused CSS rule');
    assert(featureSuite.includes('status.features.filter(f => !!f.name)'),
        'FCFG-003 must compare the DOM with named, displayable API features');

    const featureHeaderIndex = featureHtml.indexOf('fb-card-header-toolbar');
    const featureMonitorIndex = featureHtml.indexOf('id="features-ram-monitor"');
    const featureActionsIndex = featureHtml.indexOf('class="fb-card-header-actions"');
    const featureBodyIndex = featureHtml.indexOf('class="fb-card-body"');
    assert(featureHeaderIndex >= 0 && featureHeaderIndex < featureMonitorIndex
        && featureMonitorIndex < featureActionsIndex && featureActionsIndex < featureBodyIndex,
        'feature RAM metrics and actions must share the card header toolbar');
}

function checkFeatureTransferRegressionGuards() {
    // 2026-08-17: 功能配置页的导入/导出与一键精简已移除（与设备配置页配置转移冗余，
    // 且一键精简会误关 webServer/mdns 导致 Web 入口丢失）；
    // 功能配置备份/迁移统一走设备配置页（高级配置）的配置转移模块
    const featuresHtml = read('web-src/pages/features.html');
    assert(!featuresHtml.includes('features-export-btn'), 'features page must NOT have export button (moved to device config transfer)');
    assert(!featuresHtml.includes('features-import-btn'), 'features page must NOT have import button (moved to device config transfer)');
    assert(!featuresHtml.includes('features-lite-btn'), 'features page must NOT have lite-mode button');
    const featureJs = read('web-src/modules/runtime/feature-config.js');
    assert(!featureJs.includes('exportFeatureConfig'), 'feature-config must NOT implement exportFeatureConfig');
    assert(!featureJs.includes('importFeatureConfig'), 'feature-config must NOT implement importFeatureConfig');
    assert(!featureJs.includes('applyLiteMode'), 'feature-config must NOT implement applyLiteMode');
    const deviceJs = read('web-src/modules/runtime/device-config.js');
    // 2026-08: 标签改为 i18n 包装 t('dc-label-features', '功能配置')，键与回退文案均需存在
    assert(deviceJs.includes("'features.json': '功能配置'") ||
        deviceJs.includes("'features.json': t('dc-label-features', '功能配置')"),
        'transfer labels must map features.json to 功能配置');
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
    // Modbus 初始化前应等待 1500ms cooldown，请求间隔 600ms；
    // 延时必须可在页面退出时取消，避免旧页面继续发请。
    const coreJs = read('web-src/modules/runtime/device-control/core.js');
    assert(coreJs.includes('_dcDelay(1500, lifecycleGeneration)'), 'Modbus init should start with cancellable 1500ms cooldown');
    assert(coreJs.includes('_dcDelay(600, lifecycleGeneration)'), 'Modbus inter-request delay should be cancellable and 600ms');
    assert(coreJs.includes('_dcCancelPendingDelays()'), 'device-control teardown should cancel pending Modbus delays');
    assert(!/_dcDelay\((150|300|800),/.test(coreJs), 'Modbus timing must NOT use old values (150/300/800ms)');

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
    // 后续提速优化: chunk 间隔 80ms -> 30ms（由 native test_frontend_loader_timings_locked 锁定）
    const indexHtml = read('web-src/index.html');
    // chunk加载间隔应为30ms（提速后的锁定值）
    assert(indexHtml.includes('delayMs || 30'), 'chunk loading delay should be 30ms (speedup locked)');
    assert(indexHtml.includes("loadSequence(bootChunks, 30)"), 'boot chunk sequence should use 30ms interval');
    // SW注册应延迟5秒
    assert(indexHtml.includes('setTimeout') && indexHtml.includes('5000'), 'SW registration should be delayed 5000ms');
    // SW注册不应使用 window.addEventListener('load') 模式
    assert(!indexHtml.includes("window.addEventListener('load'"), 'SW registration should NOT use load event listener');

    const sessionJs = read('web-src/js/state-session.js');
    // 页面预加载应延迟5000ms，间隔1200ms（降低低内存设备上的并发突发，避免连接超时）
    assert(sessionJs.includes('delayMs: 1200'), 'page preload interval should be 1200ms');
    assert(sessionJs.includes('}, 5000)'), 'page preload should be delayed 5000ms');
    // modals加载应延迟3000ms
    assert(sessionJs.includes('}, 3000)'), 'modals loading should be delayed 3000ms');

    // 页面片段加载必须带退避重试（设备繁忙/堆压力下偶发连接超时）
    const pageLoaderJs = read('web-src/js/page-loader.js');
    assert(pageLoaderJs.includes('_fetchWithRetry'), 'PageLoader must use _fetchWithRetry for page/modals/fragment fetches');
}

function checkSharedStaticAssetQueue() {
    const indexHtml = read('web-src/index.html');
    const mainJs = read('web-src/js/main.js');
    const pageLoaderJs = read('web-src/js/page-loader.js');
    const moduleLoaderJs = read('web-src/js/module-loader.js');

    assert(indexHtml.includes('window.__fastbeeStaticAssetChain = Promise.resolve()'),
        'index.html must define one browser-wide static asset queue');
    assert(indexHtml.includes('setTimeout(resolve, 100)'),
        'static asset queue must leave a short TCP disconnect settle window');
    assert(indexHtml.includes('window.__fastbeeQueueStaticAsset(function()'),
        'boot chunks must use the shared static asset queue');
    assert(indexHtml.includes('window.__fastbeeQueueStaticAsset(registerWorker)'),
        'service worker registration must use the shared static asset queue');
    assert(mainJs.includes('window.__fastbeeQueueStaticAsset(loadStylesheet)'),
        'main stylesheet must use the shared static asset queue');
    assert(pageLoaderJs.includes('window.__fastbeeQueueStaticAsset(op)'),
        'page fragments must use the shared static asset queue');
    assert(moduleLoaderJs.includes('window.__fastbeeQueueStaticAsset(startRequest)'),
        'dynamic modules must use the shared static asset queue');
}

function checkPageLoaderResponseIntegrity() {
    const source = read('web-src/js/page-loader.js');
    const loadStart = source.indexOf('async loadPage(pageId, pageMapping, options)');
    const catchStart = source.indexOf('} catch(e) {', loadStart);
    const finallyStart = source.indexOf('} finally {', catchStart);
    const loadBlock = source.slice(loadStart, finallyStart);
    const catchBlock = source.slice(catchStart, finallyStart);
    const cacheStart = source.indexOf('_cachePageTemplates(moduleName, html)');
    const mountStart = source.indexOf('_mountPage(pageId)');
    const mountEnd = source.indexOf('\n    mountPage(pageId)', mountStart);
    const cacheBlock = source.slice(cacheStart, mountStart);
    const mountBlock = source.slice(mountStart, mountEnd);

    assert(loadStart >= 0 && catchStart > loadStart && finallyStart > catchStart,
        'PageLoader loadPage try/catch structure must remain detectable');
    assert(cacheBlock.includes("wrapper.querySelectorAll('.page[id]')") &&
        cacheBlock.includes('this._pageTemplates[root.id] = root.outerHTML') &&
        cacheBlock.includes('this._loadedPages[moduleName] = true'),
        'PageLoader must validate and cache page roots before marking the module loaded');
    assert(loadBlock.includes('Page response missing expected root'),
        'PageLoader must reject a 200 response that lacks the expected page root');
    assert(loadBlock.includes('this._pageTemplates[pageId]') &&
        !loadBlock.includes("moduleName + '-page'"),
        'PageLoader must validate the mapped page ID, which may differ from the HTML module name');
    assert(mountBlock.includes("existing.querySelector('[data-page-fallback]')") &&
        mountBlock.includes('existing.remove()'),
        'A successful retry must replace an existing fallback root');
    assert(loadBlock.includes("options.mount === false ? null : this._mountPage(pageId)"),
        'PageLoader must support cache-only loads so stale navigations cannot mount DOM');
    assert(catchBlock.includes('delete this._loadedPages[moduleName]'),
        'A failed page load must clear its loaded cache entry');
    assert(!catchBlock.includes('this._loadedPages[moduleName] = true'),
        'A fallback page must remain retryable instead of being cached as loaded');
    assert(catchBlock.includes("if (options.mount !== false) this._renderUnavailablePage(pageId, e)"),
        'A failed module load must render the fallback under the requested page ID');
    assert(source.includes('PAGE_FETCH_TIMEOUT_MS') && source.includes('new AbortController()'),
        'PageLoader fetches must use a bounded abort timeout so the shared asset queue cannot hang forever');
}

function checkNetworkNotificationRegressionGuards() {
    // 2025-06 修复: 网络设置保存提示应使用“网络设置保存成功”而非“WiFi配置保存成功”
    const networkJs = read('web-src/modules/runtime/network.js');
    assert(!networkJs.includes('WiFi配置保存成功'), 'network.js must NOT contain WiFi配置保存成功');
    assert(networkJs.includes('网络设置保存成功'), 'network.js should contain 网络设置保存成功');

    // 2025-06 修复: loadNetworkStatus 应检测DNS解析错误并跳过重试
    assert(networkJs.includes('_isDnsError'), 'network.js should have _isDnsError helper');
    assert(networkJs.includes('DNS resolution failed'), 'loadNetworkStatus should log DNS errors');

    const i18nZhPath = path.join(ROOT_DIR, 'web-src', 'i18n', 'i18n-zh-CN.js');
    if (fs.existsSync(i18nZhPath)) {
        const i18nZh = fs.readFileSync(i18nZhPath, 'utf8');
        assert(!i18nZh.includes("'wifi-save-ok': 'WiFi配置保存成功"), 'i18n zh wifi-save-ok should not say WiFi配置保存成功');
    }

    // 2025-06 修复: 4G 蜂窝面板应包含访问方式提示
    const networkHtml = read('web-src/pages/network.html');
    assert(networkHtml.includes('cellular-access-hint') ||
        (networkHtml.includes('id="cellular-panel"') && networkHtml.includes('访问方式')),
        'cellular panel should have access hint');
    assert(networkHtml.includes('fastbee-ap'), 'cellular hint should mention AP SSID');
    assert(networkHtml.includes('192.168.4.1'), 'cellular hint should mention AP IP');
    // 4G 访问提示不应包含 fastbee.local（AP 无 DNS 服务器，mDNS 不可靠）
    const cellularPanel = networkHtml.match(/id="cellular-panel"[\s\S]*?id="ap-config"/);
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
    assert(networkHtml.includes('data-i18n="wifi-security-wpa2"') ||
        /<option[^>]*value="wpa2"[^>]*>\s*WPA2\s*<\/option>/.test(networkHtml),
        'wifi-security wpa2 option must have i18n key or a visible fallback label');
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
    // 模态窗按需挂载，关闭后销毁；重新创建的 DOM 必须重新绑定事件。

    // 1. state.js 必须包含模态窗事件重绑定机制
    const stateJs = read('web-src/js/state.js');
    assert(stateJs.includes('_registerModalBinder'),
        'state.js must provide _registerModalBinder for modal event rebinding');
    assert(stateJs.includes('_rebindAllModalEvents'),
        'state.js must provide _rebindAllModalEvents to trigger all modal binders');
    assert(stateJs.includes('ensureModal(modalId)'),
        'state.js must mount modal templates on demand');
    assert(stateJs.includes('this._resetModalBindingState(modalId)'),
        'state.js must reset binding guards for recreated modal DOM');
    const pageLoaderJs = read('web-src/js/page-loader.js');
    assert(pageLoaderJs.includes('_modalTemplates'),
        'page-loader.js must cache individual modal templates');
    assert(pageLoaderJs.includes('unmountModal(modalId)'),
        'page-loader.js must support destroying closed modal roots');
    assert(!pageLoaderJs.includes('document.body.appendChild(wrapper.firstChild)'),
        'loadModals must not retain every modal root in body');

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

    // 5. 用户弹窗同样需要在每次按需挂载后重绑
    const usersJs = read('web-src/modules/admin/users.js');
    assert(usersJs.includes('_bindUserModalEvents'),
        'users.js must separate modal event binding from page event binding');
    assert(usersJs.includes("_registerModalBinder('users'"),
        'users.js must register modal binder via _registerModalBinder');

    // 6. 外设配置页面必须有开发环境禁用提示条
    const peripheralHtml = read('web-src/pages/peripheral.html');
    assert(peripheralHtml.includes('peripheral-dev-mode-hint'),
        'peripheral.html must have developer-mode-disabled hint bar');
    ['peripheral-board-resources', 'peripheral-automation', 'peripheral-experiments'].forEach((removedId) => {
        assert(!peripheralHtml.includes(removedId), `peripheral workspace must not expose #${removedId}`);
    });
    assert(!peripheralHtml.includes('class="config-tabs"'),
        'peripheral configuration must open directly on the peripheral list');
    const peripheralPageIndex = peripheralHtml.indexOf('id="peripheral-page"');
    const periphExecPageIndex = peripheralHtml.indexOf('id="periph-exec-page"');
    const deviceControlPageIndex = peripheralHtml.indexOf('id="device-control-page"');
    const peripheralPageHtml = peripheralHtml.slice(peripheralPageIndex, periphExecPageIndex);
    const periphExecPageHtml = peripheralHtml.slice(periphExecPageIndex, deviceControlPageIndex);
    [
        [peripheralPageHtml, 'peripheral-list-summary', 'peripheral'],
        [periphExecPageHtml, 'periph-exec-list-summary', 'periph-exec']
    ].forEach(([pageHtml, summaryId, pageName]) => {
        const headerIndex = pageHtml.indexOf('fb-card-header-toolbar');
        const summaryIndex = pageHtml.indexOf(`id="${summaryId}"`);
        const actionsIndex = pageHtml.indexOf('class="fb-card-header-actions"');
        const bodyIndex = pageHtml.indexOf('class="fb-card-body"');
        assert(headerIndex >= 0 && headerIndex < summaryIndex
            && summaryIndex < actionsIndex && actionsIndex < bodyIndex,
            `${pageName} filter, summary, and actions must share the card header toolbar`);
    });
    assert(peripheralsJs.includes("getElementById('peripheral-list-summary')")
        && peripheralsJs.includes('pagination.hidden = !hasMultiplePages'),
        'peripheral list must update the header summary and hide single-page pagination');
    assert(periphExecJs.includes("getElementById('periph-exec-list-summary')")
        && periphExecJs.includes('pagination.hidden = !hasMultiplePages'),
        'periph-exec list must update the header summary and hide single-page pagination');
    assert(peripheralsJs.includes('peripheral-dev-mode-hint'),
        'peripherals.js must show/hide developer mode hint bar');

    // 7. 开发环境状态管理: applyDeveloperModeState 必须覆盖关键按钮
    assert(stateJs.includes("'#add-peripheral-btn'"),
        'applyDeveloperModeState must target #add-peripheral-btn');
    assert(stateJs.includes("'#periph-exec-page-add-btn'"),
        'applyDeveloperModeState must target #periph-exec-page-add-btn');

    // 8. 角色权限已移除，前端不应有任何权限检查
    assert(!peripheralsJs.includes('hasPermission') && !peripheralsJs.includes('checkPermission'),
        'peripherals.js must NOT contain permission checks (roles removed)');
    assert(!periphExecJs.includes('hasPermission') && !periphExecJs.includes('checkPermission'),
        'periph-exec.js must NOT contain permission checks (roles removed)');

    // 9. CSS 双态: dev-mode-locked 类必须存在
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
    const ruleHeaderIndex = ruleScriptHtml.indexOf('fb-card-header-toolbar');
    const ruleSummaryIndex = ruleScriptHtml.indexOf('id="rule-script-list-summary"');
    const ruleActionsIndex = ruleScriptHtml.indexOf('class="fb-card-header-actions"');
    const ruleBodyIndex = ruleScriptHtml.indexOf('class="fb-card-body"');
    assert(ruleHeaderIndex >= 0 && ruleHeaderIndex < ruleSummaryIndex
        && ruleSummaryIndex < ruleActionsIndex && ruleActionsIndex < ruleBodyIndex,
        'rule-script summary and actions must share the card header toolbar');
    assert(ruleScriptJs.includes("getElementById('rule-script-list-summary')"),
        'rule-script.js must update the header summary');

    // 11. 容量锁定函数存在于各自模块中（非 state.js）
    assert(peripheralsJs.includes('_setPeripheralCapacity'),
        'peripherals.js must provide _setPeripheralCapacity for profile-based button limits');
    assert(periphExecJs.includes('_setPeriphExecCapacity'),
        'periph-exec.js must provide _setPeriphExecCapacity for profile-based button limits');
}

function run() {
    const files = [];
    walkDir(WWW_DIR, files);
    const manifest = readPublishedManifest();

    checkRequiredAssets(manifest);
    checkRetiredAssetsAreAbsent();
    checkCompressedOnly(files);
    checkJavascriptSyntax(files);
    checkUiRegressionGuards(files, manifest);
    checkPaginationRegressionGuards(files);
    checkRoleCaseRegressionGuards();
    checkMenuOrderRegressionGuards();
    checkButtonColorRegressionGuards();
    checkFeatureConfigJsonBodyGuard();
    checkFeatureListRenderingGuard();
    checkFeatureTransferRegressionGuards();
    checkGovernorOverloadRegressionGuards();
    checkPeriphExecResultsRemoved();
    checkModuleLoaderResilience();
    checkBootTimingRegressionGuards();
    checkSharedStaticAssetQueue();
    checkPageLoaderResponseIntegrity();
    checkNetworkNotificationRegressionGuards();
    checkWifiSecurityRegressionGuards();
    checkRoleManagementRemoved();
    checkDeveloperModeAndModalEventBinding();
    checkAssetBudgets(manifest);
    checkFirstPaintPath(manifest);

    if (failures.length > 0) {
        console.error(`\nWeb smoke test failed: ${failures.length} issue(s)`);
        process.exit(1);
    }

    console.log(`Web smoke test passed: environment=${manifest.environment}, ${files.length} published file(s), ${files.filter((file) => file.endsWith('.gz')).length} gzip asset(s)`);
}

run();
