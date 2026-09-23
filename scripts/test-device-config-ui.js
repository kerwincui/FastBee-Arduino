'use strict';

// Device configuration UI regression checks. These checks intentionally avoid a
// browser or a live device so they can run in CI before a LittleFS upload.
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const zlib = require('zlib');

const ROOT = path.join(__dirname, '..');
const failures = [];

function read(relPath) {
    return fs.readFileSync(path.join(ROOT, relPath), 'utf8');
}

function assert(condition, message) {
    if (!condition) failures.push(message);
}

function parseJavaScript(label, source) {
    try {
        new vm.Script(source, { filename: label });
    } catch (error) {
        failures.push(`${label} syntax error: ${error.message}`);
    }
}

function readPublished(relPath) {
    const filePath = path.join(ROOT, 'data', 'www', relPath);
    if (!fs.existsSync(filePath)) return null;
    const compressed = fs.readFileSync(filePath);
    try {
        return zlib.gunzipSync(compressed).toString('utf8');
    } catch (error) {
        failures.push(`${relPath} is not valid gzip: ${error.message}`);
        return null;
    }
}

const deviceJs = read('web-src/modules/runtime/device-config.js');
const otaJs = read('web-src/modules/runtime/device-ota.js');
const sessionJs = read('web-src/js/state-session.js');
const stateJs = read('web-src/js/state.js');
const indexHtml = read('web-src/index.html');
const deviceHtml = read('web-src/pages/device.html');
const otaHtml = read('web-src/pages/fragments/device-ota.html');
const deployPs1 = read('scripts/deploy.ps1');
const browserRunnerPs1 = read('scripts/run-browser-tests.ps1');

parseJavaScript('web-src/modules/runtime/device-config.js', deviceJs);
parseJavaScript('web-src/modules/runtime/device-ota.js', otaJs);
parseJavaScript('web-src/js/state-session.js', sessionJs);
assert(!deviceJs.includes('\uFFFD'), 'device-config.js contains replacement characters');
assert(!otaJs.includes('\uFFFD'), 'device-ota.js contains replacement characters');
assert(sessionJs.includes('function clearLegacyAuthArtifacts()'),
    'session management must define a legacy auth cleanup helper');
assert(sessionJs.indexOf('clearLegacyAuthArtifacts();') < sessionJs.indexOf("apiGet('/api/auth/session')"),
    'session refresh must clear legacy bearer artifacts before probing the cookie session');
const loginSuccessIndex = sessionJs.indexOf('if (res && res.success) {');
const loginCleanupIndex = sessionJs.indexOf('clearLegacyAuthArtifacts();', loginSuccessIndex);
assert(loginSuccessIndex >= 0 && loginCleanupIndex > loginSuccessIndex,
    'successful cookie login must clear legacy bearer artifacts even when remember is enabled');
assert(!indexHtml.includes('id="login-language-select"') && !indexHtml.includes('id="language-select"'),
    'single-language UI must not expose language selectors');
assert(!deviceHtml.includes('data-i18n') && !otaHtml.includes('data-i18n'),
    'device configuration pages must use hardcoded Chinese text');
['设备配置', '基本信息', 'NTP时间', 'OTA升级', '高级配置'].forEach((text) => {
    assert(deviceHtml.includes(text), `device.html is missing hardcoded Chinese text: ${text}`);
});
['固件信息', '在线升级', '本地上传升级', '升级注意事项'].forEach((text) => {
    assert(otaHtml.includes(text), `OTA fragment is missing hardcoded Chinese text: ${text}`);
});

assert(deviceJs.includes("const DEFAULT_NTP_SERVER1 = 'http://iot.fastbee.cn/prod-api/iot/tool/ntp'"),
    'NTP primary default URL is missing');
assert(deviceJs.includes("const DEFAULT_NTP_SERVER2 = 'cn.pool.ntp.org'"),
    'NTP secondary default host is missing');
assert(deviceJs.includes("apiPost('/api/device/time/sync', {})"),
    'NTP refresh must call the sync endpoint');
assert(deviceJs.includes('syncDeviceTime({ silent: true })'),
    'saving NTP settings must trigger an automatic sync');
assert(stateJs.includes('this.syncDeviceTime({ silent: true })'),
    'opening the NTP tab must trigger an automatic sync');
assert(stateJs.includes("this._loadModule('device-config', syncNtp)"),
    'NTP tab must defer automatic sync until the device module is loaded');
const ntpSaveStart = deviceJs.indexOf('        saveDeviceNTP() {');
const ntpSaveEnd = deviceJs.indexOf('saveSecurityPolicy()', ntpSaveStart);
assert(ntpSaveStart >= 0 && ntpSaveEnd > ntpSaveStart
    && deviceJs.slice(ntpSaveStart, ntpSaveEnd).includes("apiInvalidateCache('/api/device/config')"),
    'saving NTP settings must invalidate the device config cache');
assert(deviceJs.includes('btn.disabled = false;'),
    'NTP refresh button must be restored to an enabled state');
assert(deviceJs.includes('dev-sys-chip') && deviceJs.includes("/api/system/info"),
    'hardware information loader is missing');
assert(deviceJs.includes('d.device || d.hardware || d'),
    'hardware loader must support compact/legacy response shapes');
assert(deviceJs.includes('return fallback || key') && otaJs.includes('return fallback || key'),
    'runtime notification helpers must use Chinese fallback text');
assert(deviceJs.includes('function isExpectedAbort') && deviceJs.includes('isExpectedAbort(err) || isTransientNetworkError(err)'),
    'device time requests must ignore expected page-abort errors');
assert(deviceJs.includes('function isTransientNetworkError') && deviceJs.includes('restoreTimeRefreshButton'),
    'device time requests must handle transient network timeouts without duplicate retries');
assert(deviceJs.includes("apiGet('/api/system/capabilities')")
    && deviceJs.includes('dev-ota-capability-hint')
    && deviceJs.includes('当前分区不支持 OTA 升级'),
    'inline OTA fallback must disable unsupported 4MB partitions');
assert(otaJs.includes("getter('/api/system/capabilities')")
    && otaJs.includes('_otaSupported')
    && otaJs.includes('ota-capability-hint')
    && otaJs.includes('当前分区不支持 OTA 升级'),
    'full OTA page must gate actions on the firmware capability');
assert(otaJs.includes('const capabilityReady =')
    && otaJs.includes('caps.ota === true')
    && otaJs.includes('.catch(() => false)')
    && otaJs.includes('capabilityReady.then(supported =>')
    && otaJs.includes('if (supported) this._loadOtaStatus();'),
    'OTA status must be requested only after the firmware capability is known');

['device-ota-container', 'dev-sys-chip', 'dev-sys-flash', 'dev-time-refresh-btn',
    'dev-ntp-server1', 'dev-ntp-server2'].forEach((id) => {
    assert(deviceHtml.includes(`id="${id}"`), `device.html is missing #${id}`);
});
['ota-current-version', 'ota-url-form', 'ota-upload-form', 'ota-file', 'ota-status-badge']
    .forEach((id) => assert(otaHtml.includes(`id="${id}"`), `OTA fragment is missing #${id}`));
assert(otaHtml.includes('id="ota-capability-hint"'),
    'OTA fragment is missing the unsupported-partition hint');
assert((otaHtml.match(/class="fb-btn fb-btn-primary ota-action-btn"/g) || []).length === 2,
    'OTA online/upload actions must use the compact ota-action-btn class');
const mainCss = read('web-src/css/main.css');
assert(mainCss.includes('.ota-inline-action-row .ota-action-btn')
    && mainCss.includes('max-width: 132px')
    && mainCss.includes('align-self: flex-start'),
    'OTA action buttons must have compact responsive sizing');
const baseButtonRules = Array.from(mainCss.matchAll(/^\.fb-btn\s*\{([^}]*)\}/gm));
assert(baseButtonRules.length === 1,
    'the shared .fb-btn base style must be defined exactly once');
assert(baseButtonRules.length === 1
    && /border:\s*1px solid var\(--border\)/.test(baseButtonRules[0][1])
    && !/border:\s*1px solid transparent/.test(baseButtonRules[0][1]),
    'the shared .fb-btn base style must keep a visible neutral border');
assert(mainCss.includes('.file-input-wrapper > label.fb-btn')
    && mainCss.includes('.file-name-display'),
    'file selectors must use the shared file picker styles');
assert((deviceHtml.match(/class="file-input-wrapper u-mb-10"/g) || []).length === 2,
    'inline OTA and config transfer must use the shared file picker layout');
assert(otaHtml.includes('class="file-input-wrapper file-input-wrapper-top"'),
    'the OTA fragment must use the shared file picker layout');
assert((deviceHtml + otaHtml).match(/class="fb-btn fb-btn-secondary fb-btn-compact"/g).length >= 3,
    'all file picker triggers must use the same shared button variant');
['dev-ota-file', 'dev-config-import-file'].forEach((id) => {
    const labelPattern = new RegExp(`<label[^>]*for="${id}"[^>]*>`);
    const label = deviceHtml.match(labelPattern);
    assert(label && !label[0].includes('style='),
        `#${id} file picker must not depend on inline styles`);
});
assert(deployPs1.includes('[switch]$VerifyWeb')
    && deployPs1.includes('Test-DeviceWebAssets')
    && deployPs1.includes('for ($attempt = 1; $attempt -le 8; $attempt++)'),
    'deployment script must support post-upload Web verification with startup retries');
assert(browserRunnerPs1.includes('[string]$Grep')
    && browserRunnerPs1.includes('"--grep", $Grep')
    && browserRunnerPs1.includes('node $playwrightCli @playwrightArgs')
    && browserRunnerPs1.includes('node_modules\\@playwright\\test\\cli.js'),
    'browser runner must support safe targeted grep execution');

const publishedDeviceJs = readPublished('js/modules/device-config.js.gz');
const publishedOtaJs = readPublished('js/modules/device-ota.js.gz');
if (publishedDeviceJs) {
    parseJavaScript('data/www/js/modules/device-config.js.gz', publishedDeviceJs);
    assert(!publishedDeviceJs.includes('\uFFFD'), 'published device-config.js contains replacement characters');
}
if (publishedOtaJs) parseJavaScript('data/www/js/modules/device-ota.js.gz', publishedOtaJs);

if (failures.length) {
    console.error(`Device configuration UI regression failed (${failures.length}):`);
    failures.forEach((failure) => console.error(`- ${failure}`));
    process.exitCode = 1;
} else {
    console.log('Device configuration UI regression passed');
}
