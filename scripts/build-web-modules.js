/**
 * Build publish-time web assets from editable source files.
 *
 * Responsibilities:
 * 1. Sync static assets (HTML, CSS, JS) from web-src/ to data/www/.
 * 2. Sync runtime modules from web-src/modules/runtime to data/www/js/modules.
 * 3. Sync i18n modules from web-src/i18n to data/www/js/modules.
 * 4. Build publish-ready admin module files from web-src/modules/admin source files.
 * 5. Validate that all output modules trace back to web-src/ sources.
 *
 * Usage:
 *   node scripts/build-web-modules.js
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const {
    buildAdminBundle,
    getSourceFiles: getAdminSourceFiles
} = require('./build-admin-bundle');
const {
    MENU_CAPABILITY_REQUIREMENTS,
    MODAL_CAPABILITY_REQUIREMENTS,
    PAGE_ROOT_CAPABILITY_REQUIREMENTS,
    PROTOCOL_TAB_CAPABILITY_REQUIREMENTS,
    capabilityRuleSatisfied,
    getWebProfile,
    hasDeviceControl,
    resolveRequestGovernorLimits,
    resolveWebBuildContext,
    shouldIncludeAsset,
    shouldIncludeMenu,
    writeWebResourceManifest
} = require('./web-profile');
const {
    buildAppBundle,
    BUNDLE_SOURCES: APP_BUNDLE_SOURCES,
    CHUNK_GROUPS: APP_CHUNK_GROUPS,
    extractZhTranslations,
    filterProdI18nTranslations
} = require('./build-app-bundle');
const { minifyJS } = require('./minify-js');
const { minify_sync } = require('terser');
const { withWebPublishLock } = require('./web-publish-lock');

function minifyCSS(css) {
    return css
        .replace(/\/\*[\s\S]*?\*\//g, '')    // 删除注释
        .replace(/\s+/g, ' ')                 // 多空白合并
        .replace(/\s*([{}:;,>~+])\s*/g, '$1') // 符号周围空白
        .replace(/;}/g, '}')                  // 末尾分号
        .replace(/^\s+|\s+$/gm, '')           // 行首尾空白
        .trim();
}

const ROOT_DIR = path.join(__dirname, '..');
const WEB_SRC_DIR = path.join(ROOT_DIR, 'web-src');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');
const RUNTIME_SOURCE_DIR = path.join(WEB_SRC_DIR, 'modules', 'runtime');
const I18N_SOURCE_DIR = path.join(WEB_SRC_DIR, 'i18n');
const PUBLISH_MODULE_DIR = path.join(WWW_DIR, 'js', 'modules');
const BUILD_CONTEXT = resolveWebBuildContext();
const ALL_DIRECT_RUNTIME_MODULES = [
    'protocol.js',
    'periph-exec.js',
    'periph-exec-form.js',
    'periph-exec-sensor.js',
    'periph-exec-effects.js',
    'periph-exec-modbus.js',
    'screen-panel-model.js',
    'screen-panel.js',
    'peripherals.js',
    'peripheral-save.js',
    'peripheral-io.js',
    'ir-remote.js',
    'rf-remote.js',
    'dashboard.js',
    'dashboard-fullscreen.js',
    'device-config.js',
    'device-ota.js',
    'network.js',
    'feature-config.js',
    'device-control.js'
];
const DIRECT_RUNTIME_MODULES = ALL_DIRECT_RUNTIME_MODULES.filter((fileName) => {
    if (fileName === 'device-control.js' && !hasDeviceControl(BUILD_CONTEXT)) return false;
    return shouldIncludeAsset(`js/modules/${fileName}`, BUILD_CONTEXT);
});
const I18N_MODULES = [];  // i18n support removed - all translations are now hardcoded in HTML/JS
const COMPACT_STATIC_SKIP = new Set([
    'pages/admin.html',
    'pages/fullscreen.html',
    'pages/logs.html'
    // 保留 pages/fragments/device-ota.html：OTA 在线升级/本地上传按目标能力提供
    // （分片仅 ~1.7KB gz，device-ota.js 已在 DIRECT_RUNTIME_MODULES 发布）
]);
const LITE_STATIC_SKIP = new Set([
    'pages/fragments/protocol-modbus-rtu.html',
    'pages/rule-script.html'
]);
const PROD_IGNORED_ORPHAN_MODULES = new Set([
    'i18n-en.js',
    'i18n-engine.js',
    'i18n-zh-CN.js',
    'logs.js',
    'users.js',
]);
// notification.js is already part of chunk-2-app.js. No standalone page loads
// it, so publishing a second copy only consumes flash and manifest entries.
const STANDALONE_ROOT_JS = [];
const STANDALONE_ROOT_JS_SET = new Set(STANDALONE_ROOT_JS.map((item) => item.file));
const APP_CHUNK_FILES = new Set(APP_CHUNK_GROUPS.map((chunk) => chunk.outName));

function readUtf8(filePath) {
    return fs.readFileSync(filePath, 'utf8').replace(/^\uFEFF/, '');
}

function writeUtf8(filePath, content) {
    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    fs.writeFileSync(filePath, content, 'utf8');
}

function removeGeneratedFile(filePath) {
    [filePath, `${filePath}.gz`].forEach((targetPath) => {
        if (!fs.existsSync(targetPath)) return;
        try {
            fs.chmodSync(targetPath, 0o666);
        } catch (_) {
            // Best effort on Windows locked/read-only generated files.
        }
        try {
            fs.rmSync(targetPath, { force: true });
        } catch (error) {
            console.warn(`  generated file kept: ${path.relative(ROOT_DIR, targetPath)} (${error.code || error.message})`);
        }
    });
}

function cleanGeneratedPublishTree() {
    const removed = [];
    const generatedExtensions = new Set(['.html', '.js', '.css']);
    const walk = (directory) => {
        if (!fs.existsSync(directory)) return;
        fs.readdirSync(directory, { withFileTypes: true }).forEach((entry) => {
            const filePath = path.join(directory, entry.name);
            if (entry.isDirectory()) {
                walk(filePath);
                if (filePath !== WWW_DIR && fs.existsSync(filePath) && fs.readdirSync(filePath).length === 0) {
                    fs.rmdirSync(filePath);
                }
                return;
            }
            const logicalName = entry.name.endsWith('.gz') ? entry.name.slice(0, -3) : entry.name;
            const generated = generatedExtensions.has(path.extname(logicalName).toLowerCase()) ||
                entry.name === 'web-resource-manifest.json';
            if (!generated) return;
            fs.rmSync(filePath, { force: true });
            removed.push(path.relative(WWW_DIR, filePath).replace(/\\/g, '/'));
        });
    };
    walk(WWW_DIR);
    return removed.sort();
}

function shouldSkipStaticAsset(relPath) {
    return !shouldIncludeAsset(relPath, BUILD_CONTEXT);
}

function stripSection(content, startMarker, endMarker) {
    const start = content.indexOf(startMarker);
    if (start < 0) return content;
    if (!endMarker) return content.slice(0, start);

    const end = content.indexOf(endMarker, start + startMarker.length);
    if (end < 0) return content.slice(0, start);
    return content.slice(0, start) + content.slice(end);
}

function escapeRegExp(value) {
    return String(value).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function findElementByAttribute(content, attribute, value, fromIndex = 0) {
    const attr = escapeRegExp(attribute);
    const val = escapeRegExp(value);
    const openingPattern = new RegExp(
        `<([A-Za-z][\\w:-]*)\\b[^>]*\\b${attr}\\s*=\\s*(?:"${val}"|'${val}')[^>]*>`,
        'gi'
    );
    openingPattern.lastIndex = fromIndex;
    const opening = openingPattern.exec(content);
    if (!opening) return null;

    const tagName = opening[1];
    const openStart = opening.index;
    const openEnd = openingPattern.lastIndex;
    if (/\/\s*>$/.test(opening[0])) {
        return { start: openStart, end: openEnd, openStart, openEnd, tagName };
    }

    const tokenPattern = new RegExp(`<!--[\\s\\S]*?-->|<\\/?${escapeRegExp(tagName)}\\b[^>]*>`, 'gi');
    tokenPattern.lastIndex = openEnd;
    let depth = 1;
    let token;
    while ((token = tokenPattern.exec(content))) {
        if (token[0].startsWith('<!--')) continue;
        if (new RegExp(`^<\\/${escapeRegExp(tagName)}\\b`, 'i').test(token[0])) {
            depth -= 1;
            if (depth === 0) {
                return {
                    start: openStart,
                    end: tokenPattern.lastIndex,
                    openStart,
                    openEnd,
                    tagName
                };
            }
        } else if (!/\/\s*>$/.test(token[0])) {
            depth += 1;
        }
    }
    throw new Error(`Unclosed <${tagName}> with ${attribute}="${value}"`);
}

function removeElementsByAttribute(content, attribute, value) {
    let result = content;
    let match = findElementByAttribute(result, attribute, value);
    while (match) {
        result = result.slice(0, match.start) + result.slice(match.end);
        match = findElementByAttribute(result, attribute, value, match.start);
    }
    return result;
}

function setElementActive(content, attribute, value, active) {
    const match = findElementByAttribute(content, attribute, value);
    if (!match) return content;
    const opening = content.slice(match.openStart, match.openEnd);
    let updated;
    if (/\bclass\s*=\s*(?:"[^"]*"|'[^']*')/i.test(opening)) {
        updated = opening.replace(/\bclass\s*=\s*("([^"]*)"|'([^']*)')/i, (whole, quoted, doubleValue, singleValue) => {
            const quote = quoted[0];
            const classes = String(doubleValue === undefined ? singleValue : doubleValue)
                .split(/\s+/)
                .filter(Boolean)
                .filter((name) => name !== 'active');
            if (active) classes.push('active');
            return `class=${quote}${classes.join(' ')}${quote}`;
        });
    } else if (active) {
        updated = opening.replace(/>$/, ' class="active">');
    } else {
        updated = opening;
    }
    return content.slice(0, match.openStart) + updated + content.slice(match.openEnd);
}

function stripSelectById(content, id) {
    const escaped = id.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    return content.replace(
        new RegExp(`\\s*<select\\b[^>]*id="${escaped}"[\\s\\S]*?<\\/select>`, 'g'),
        ''
    );
}

function stripDivByClass(content, className) {
    const escaped = className.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    return content.replace(
        new RegExp(`\\s*<div\\b[^>]*class="[^"]*\\b${escaped}\\b[^"]*"[^>]*>[\\s\\S]*?<\\/div>`, 'g'),
        ''
    );
}

function stripProdOnlyCssRules(content) {
    const dropSelectors = [
        /^\.logs-/,
        /^\.log-content/,
        /^\.log-/,
        /^\.ota-/,
        /^\.ota-inline-/,
        /^body\.fullscreen-page/,
        /^\.fullscreen-container/,
        /^\.fullscreen-toolbar/,
        /^@keyframes\s+fs-/,
        /^\[data-theme="dark"\]\s+\.logs-/,
        /^\[data-theme="dark"\]\s+\.log-/,
        /^\[data-theme="dark"\]\s+\.ota-/
    ];

    // 统一架构：不移除任何样式（保留全部资源）


    return content
        .split(/\r?\n/)
        .filter((line) => {
            const trimmed = line.trim();
            return !dropSelectors.some((pattern) => pattern.test(trimmed));
        })
        .join('\n');
}

function transformStaticAssetContent(relPath, content) {
    const normalized = normalizePath(relPath);
    let transformed = content;

    if (normalized === 'index.html') {
        Object.keys(MENU_CAPABILITY_REQUIREMENTS).forEach((pageName) => {
            if (!shouldIncludeMenu(pageName, BUILD_CONTEXT)) {
                transformed = removeElementsByAttribute(transformed, 'data-page', pageName);
            }
        });
    }

    if (normalized === 'pages/peripheral.html') {
        Object.entries(PAGE_ROOT_CAPABILITY_REQUIREMENTS).forEach(([rootId, rule]) => {
            if (!capabilityRuleSatisfied(rule, BUILD_CONTEXT)) {
                transformed = removeElementsByAttribute(transformed, 'id', rootId);
            }
        });
    }

    if (normalized === 'pages/modals.html') {
        Object.entries(MODAL_CAPABILITY_REQUIREMENTS).forEach(([modalId, rule]) => {
            if (!capabilityRuleSatisfied(rule, BUILD_CONTEXT)) {
                transformed = removeElementsByAttribute(transformed, 'id', modalId);
            }
        });
    }

    if (normalized === 'pages/device.html' && !BUILD_CONTEXT.compiledCapabilities.ota) {
        transformed = removeElementsByAttribute(transformed, 'data-tab', 'dev-ota');
        transformed = removeElementsByAttribute(transformed, 'id', 'dev-ota');
    }

    if (normalized === 'pages/protocol.html') {
        const enabledTabs = Object.entries(PROTOCOL_TAB_CAPABILITY_REQUIREMENTS)
            .filter(([, rule]) => capabilityRuleSatisfied(rule, BUILD_CONTEXT))
            .map(([tabId]) => tabId);
        Object.entries(PROTOCOL_TAB_CAPABILITY_REQUIREMENTS).forEach(([tabId, rule]) => {
            if (capabilityRuleSatisfied(rule, BUILD_CONTEXT)) return;
            transformed = removeElementsByAttribute(transformed, 'data-tab', tabId);
            transformed = removeElementsByAttribute(transformed, 'id', tabId);
        });
        enabledTabs.forEach((tabId, index) => {
            transformed = setElementActive(transformed, 'data-tab', tabId, index === 0);
            transformed = setElementActive(transformed, 'id', tabId, index === 0);
        });
    }

    return transformed;
}

function normalizePath(relPath) {
    return relPath.replace(/\\/g, '/').replace(/^\.\//, '');
}

function compiledModuleSequences(context = BUILD_CONTEXT) {
    const capabilities = context.compiledCapabilities;
    const sequences = {};
    if (capabilities.mqtt || capabilities.modbus) {
        sequences.protocol = ['protocol'];
    }
    if (capabilities.modbus) {
        sequences['protocol-full-config'] = ['protocol-full-config'];
        sequences['protocol-modbus-rtu'] = ['protocol-full-config', 'protocol-modbus-rtu'];
        sequences['protocol-modbus-control'] = ['protocol-modbus-control'];
    }
    if (capabilities.deviceControl) {
        sequences['device-control'] = ['device-control'];
        sequences['device-control-runtime'] = ['device-control-view'];
        if (capabilities.modbus) sequences['device-control-runtime'].push('device-control-modbus');
    }
    if (capabilities.periphExec) {
        sequences['periph-exec'] = ['periph-exec'];
        sequences['periph-exec-editor'] = ['periph-exec'];
        if (capabilities.modbus) sequences['periph-exec-editor'].push('periph-exec-modbus');
        sequences['periph-exec-editor'].push('periph-exec-form');
    }
    if (capabilities.screenPanel) {
        sequences['screen-panel'] = ['screen-panel-model', 'screen-panel'];
    }
    return sequences;
}

function replaceRequiredBlock(content, startMarker, endMarker, replacement, label) {
    const start = content.indexOf(startMarker);
    const end = start < 0 ? -1 : content.indexOf(endMarker, start + startMarker.length);
    if (start < 0 || end < 0) {
        throw new Error(`Unable to apply ${label} build transform`);
    }
    return content.slice(0, start) + replacement + content.slice(end);
}

function transformAppSourceContent(fileName, content) {
    if (fileName === 'request-governor.js') {
        const limits = resolveRequestGovernorLimits(BUILD_CONTEXT);
        const replacement = [
            '    // Generated from the firmware memory profile by build-web-modules.js.',
            '    const WEB_PROFILE_REQUEST_LIMITS = Object.freeze({',
            `        maxConcurrent: ${limits.maxConcurrent},`,
            `        maxWeight: ${limits.maxWeight}`,
            '    });',
            '    '
        ].join('\n');
        return replaceRequiredBlock(
            content,
            '    // FASTBEE_WEB_PROFILE_REQUEST_LIMITS_START',
            '// FASTBEE_WEB_PROFILE_REQUEST_LIMITS_END',
            replacement,
            'request governor memory profile'
        );
    }

    if (fileName === 'module-loader.js') {
        const sequenceMap = JSON.stringify(compiledModuleSequences(BUILD_CONTEXT), null, 4)
            .split('\n')
            .map((line, index) => index === 0 ? line : `    ${line}`)
            .join('\n');
        return replaceRequiredBlock(
            content,
            '    _sequenceMap: {',
            '\n\n    _resolveAssetUrl(path) {',
            `    _sequenceMap: ${sequenceMap},`,
            'ModuleLoader sequence map'
        );
    }

    if (fileName === 'state-session.js') {
        const preloadPages = ['device-page', 'network-page', 'peripheral-page'];
        if (BUILD_CONTEXT.compiledCapabilities.mqtt || BUILD_CONTEXT.compiledCapabilities.modbus) {
            preloadPages.push('protocol-page');
        }
        const replacement = `PageLoader.preloadPages(${JSON.stringify(preloadPages)}, self._pageMapping, { delayMs: 1200 })`;
        const pattern = /PageLoader\.preloadPages\(\[[\s\S]*?\], self\._pageMapping, \{ delayMs: 1200 \}\)/;
        if (!pattern.test(content)) throw new Error('Unable to apply page preload build transform');
        return content.replace(pattern, replacement);
    }

    if (fileName === 'state.js' && !BUILD_CONTEXT.compiledCapabilities.mqtt &&
        BUILD_CONTEXT.compiledCapabilities.modbus) {
        const mqttDefault = "this.loadProtocolConfig('mqtt');";
        if (!content.includes(mqttDefault)) {
            throw new Error('Unable to select the protocol default tab');
        }
        const defaultTab = BUILD_CONTEXT.compiledCapabilities.modbus ? 'modbus-rtu' : 'mqtt';
        return content.replace(mqttDefault, `this.loadProtocolConfig('${defaultTab}');`);
    }

    return content;
}

// Sub-directories under runtime/ that contain split sub-modules
const RUNTIME_SUB_DIRS = ['protocol', 'device-control'];

function syncDirectRuntimeModules() {
    const results = [];
    const noMinify = process.argv.includes('--no-minify');
    const panelNameCache = {};
    const panelPropertyRegex = /^(?:_sp|_screenPanel|savedConfig$|historyConfig$|historyStatus$|choiceCapabilities$|previewPageId$|selectedPageId$|selectedCardId$|draftTimer$|stateTimer$|dirty$|pointer$|undo$|redo$|histories$|startX$|startY$|startRect$|startDisplayRect$|snapshot$|moved$)/;

    // 1. Sync top-level entry files
    // 统一架构：始终发布所有顶层模块

    DIRECT_RUNTIME_MODULES.forEach((fileName) => {
        if (BUNDLED_ENTRY_FILES.has(fileName) || BUNDLED_SIBLING_FILES.has(fileName)) {
            return;
        }

        const sourceFile = path.join(RUNTIME_SOURCE_DIR, fileName);
        const publishFile = path.join(PUBLISH_MODULE_DIR, fileName);

        if (!fs.existsSync(sourceFile)) {
            throw new Error(`Missing runtime module source: ${sourceFile}`);
        }

        const raw = readUtf8(sourceFile);
        const panelModule = fileName === 'screen-panel-model.js' || fileName === 'screen-panel.js';
        // Panel modules share a cache so private editor properties keep the same
        // names across the model and view. Cross-module/public entry points stay stable.
        const content = noMinify ? raw : ([
            'periph-exec-form.js', 'peripherals.js',
            'screen-panel-model.js', 'screen-panel.js'
        ].includes(fileName)
            ? minify_sync(raw, panelModule ? {
                compress: { passes: 3 },
                mangle: { properties: {
                    regex: panelPropertyRegex,
                    reserved: ['_spModel', '_screenPanelModelHelpers', '_spSetupEvents', '_spSwitchTopTab']
                } },
                nameCache: panelNameCache
            } : { compress: { passes: 2 }, mangle: true }).code
            : minifyJS(raw));
        writeUtf8(publishFile, content);

        results.push({
            fileName,
            sourceFile,
            publishFile,
            size: Buffer.byteLength(content, 'utf8')
        });
    });

    // 2. Sync sub-directory modules (protocol/, device-control/)
    RUNTIME_SUB_DIRS.forEach((subDir) => {
        if (BUNDLED_SUBDIRS.has(subDir)) return;

        const srcDir = path.join(RUNTIME_SOURCE_DIR, subDir);
        if (!fs.existsSync(srcDir)) return;

        const destDir = path.join(PUBLISH_MODULE_DIR, subDir);
        if (!fs.existsSync(destDir)) {
            fs.mkdirSync(destDir, { recursive: true });
        }

        fs.readdirSync(srcDir).forEach((file) => {
            if (!file.endsWith('.js')) return;
            const sourceFile = path.join(srcDir, file);
            if (!fs.statSync(sourceFile).isFile()) return;

            const publishFile = path.join(destDir, file);
            const raw = readUtf8(sourceFile);
            const content = noMinify ? raw : minifyJS(raw);
            writeUtf8(publishFile, content);

            const relName = subDir + '/' + file;
            results.push({
                fileName: relName,
                sourceFile,
                publishFile,
                size: Buffer.byteLength(content, 'utf8')
            });
        });
    });

    return results;
}

function syncI18nModules() {
    const results = [];

    if (!fs.existsSync(PUBLISH_MODULE_DIR)) {
        fs.mkdirSync(PUBLISH_MODULE_DIR, { recursive: true });
    }

    I18N_MODULES.forEach(({ file, minify: shouldMinify, optional, fallbackPublish }) => {
        let sourceFile = path.join(I18N_SOURCE_DIR, file);
        const publishFile = path.join(PUBLISH_MODULE_DIR, file);
        const fallbackGzipFile = publishFile + '.gz';
        let fallbackContent = null;

        if (!fs.existsSync(sourceFile)) {
            if (optional && fallbackPublish && fs.existsSync(publishFile)) {
                sourceFile = publishFile;
            } else if (optional && fallbackPublish && fs.existsSync(fallbackGzipFile)) {
                fallbackContent = zlib.gunzipSync(fs.readFileSync(fallbackGzipFile)).toString('utf8');
            } else if (optional) {
                console.warn(`  Skip optional i18n module: ${file}`);
                return;
            } else {
                throw new Error(`Missing i18n module source: ${sourceFile}`);
            }
        }

        const noMinify = process.argv.includes('--no-minify');
        // 统一架构：所有 i18n 模块都直接发布
        let raw = fallbackContent !== null ? fallbackContent : readUtf8(sourceFile);
        let content = (shouldMinify && !noMinify) ? minifyJS(raw) : raw;

        writeUtf8(publishFile, content);

        results.push({
            fileName: file,
            sourceFile,
            publishFile,
            size: Buffer.byteLength(content, 'utf8'),
            minified: shouldMinify && !noMinify
        });
    });

    return results;
}

// Sync static assets (HTML pages, CSS, fullscreen-required root JS) from web-src/ to data/www/.
// Root JS is kept to the minimal standalone subset that is still referenced directly.
function syncStaticAssets() {
    const results = [];

    // Mapping: source sub-path under web-src/ -> destination sub-path under data/www/
    const syncMappings = [
        // Root-level files
        { src: 'index.html', dest: 'index.html' },
        { src: 'setup.html', dest: 'setup.html' },
        { src: '404.html', dest: '404.html' },
        { src: 'sw.js', dest: 'sw.js' },  // 统一架构：始终包含 SW
    ];

    // Copy individual root-level files
    syncMappings.forEach(({ src, dest }) => {
        const sourceFile = path.join(WEB_SRC_DIR, src);
        const destFile = path.join(WWW_DIR, dest);

        if (!fs.existsSync(sourceFile)) {
            console.warn(`  Skip: ${src} not found in web-src/`);
            return;
        }

        const dir = path.dirname(destFile);
        if (!fs.existsSync(dir)) {
            fs.mkdirSync(dir, { recursive: true });
        }

        const raw = readUtf8(sourceFile);
        const transformed = transformStaticAssetContent(src, raw);
        writeUtf8(destFile, transformed);
        const size = Buffer.byteLength(transformed, 'utf8');
        results.push({ src, dest, size });
    });

    // Sync entire directories (pages/, css/)
    const syncDirs = ['pages', 'css'];
    syncDirs.forEach((dirName) => {
        const srcDir = path.join(WEB_SRC_DIR, dirName);
        const destDir = path.join(WWW_DIR, dirName);

        if (!fs.existsSync(srcDir)) {
            console.warn(`  Skip: ${dirName}/ not found in web-src/`);
            return;
        }

        if (!fs.existsSync(destDir)) {
            fs.mkdirSync(destDir, { recursive: true });
        }

        // Recursive sync helper for pages/ subdirectories (e.g. fragments/)
        const syncDirRecursive = (src, dest, relPrefix) => {
            fs.readdirSync(src).forEach((entry) => {
                const srcFile = path.join(src, entry);
                const dstFile = path.join(dest, entry);
                const relPath = `${relPrefix}/${entry}`;
                if (fs.statSync(srcFile).isDirectory()) {
                    if (!fs.existsSync(dstFile)) {
                        fs.mkdirSync(dstFile, { recursive: true });
                    }
                    syncDirRecursive(srcFile, dstFile, relPath);
                } else {
                    if (shouldSkipStaticAsset(relPath)) {
                        removeGeneratedFile(dstFile);
                        return;
                    }
                    // Apply CSS minify for non-.min.css files
                    const noMinify = process.argv.includes('--no-minify');
                    const raw = readUtf8(srcFile);
                    const transformed = transformStaticAssetContent(relPath, raw);
                    if (!noMinify && entry.endsWith('.css') && !entry.endsWith('.min.css')) {
                        const minified = minifyCSS(transformed);
                        writeUtf8(dstFile, minified);
                        const size = Buffer.byteLength(minified, 'utf8');
                        results.push({ src: relPath, dest: relPath, size, cssMinified: true });
                    } else {
                        writeUtf8(dstFile, transformed);
                        const size = Buffer.byteLength(transformed, 'utf8');
                        results.push({ src: relPath, dest: relPath, size });
                    }
                }
            });
        };
        syncDirRecursive(srcDir, destDir, dirName);
    });

    // Sync only the standalone root JS files that are still directly referenced by pages such as fullscreen.html.
    const jsSrcDir = path.join(WEB_SRC_DIR, 'js');
    const jsDestDir = path.join(WWW_DIR, 'js');
    if (fs.existsSync(jsSrcDir)) {
        if (!fs.existsSync(jsDestDir)) {
            fs.mkdirSync(jsDestDir, { recursive: true });
        }
        // Only scripts referenced directly by standalone pages belong here.
        const noMinify = process.argv.includes('--no-minify');
        STANDALONE_ROOT_JS.forEach(({ file, minify: shouldMinify }) => {
            const srcFile = path.join(jsSrcDir, file);
            const dstFile = path.join(jsDestDir, file);
            if (!fs.existsSync(srcFile) || !fs.statSync(srcFile).isFile()) return;

            const raw = readUtf8(srcFile);
            const content = (shouldMinify && !noMinify) ? minifyJS(raw) : raw;
            writeUtf8(dstFile, content);
            const size = Buffer.byteLength(content, 'utf8');
            results.push({
                src: `js/${file}`,
                dest: `js/${file}`,
                size,
                jsMinified: shouldMinify && !noMinify
            });
        });
    }

    return results;
}

// Quality gate: ensure every .js file in data/www/js/modules/ traces back to web-src/
function validateSourceCoverage() {
    if (!fs.existsSync(PUBLISH_MODULE_DIR)) {
        throw new Error(`Publish module directory is missing: ${PUBLISH_MODULE_DIR}`);
    }

    // Build the set of expected output files
    const expectedFiles = new Set();
    DIRECT_RUNTIME_MODULES.forEach((f) => {
        expectedFiles.add(f);
    });
    activeSubdirBundles().forEach((bundle) => {
        expectedFiles.add(bundle.outName);
    });
    I18N_MODULES.forEach(({ file }) => expectedFiles.add(file));
    expectedFiles.add('admin-bundle.js');
    getAdminSourceFiles(BUILD_CONTEXT).forEach((f) => expectedFiles.add(f));

    // Entry points and sub-modules merged into an active bundle are represented
    // by that bundle's output, not by separate publish files.
    BUNDLED_ENTRY_FILES.forEach((fileName) => expectedFiles.delete(fileName));
    BUNDLED_SIBLING_FILES.forEach((fileName) => expectedFiles.delete(fileName));
    activeSubdirBundles().forEach((bundle) => expectedFiles.add(bundle.outName));

    const actualFiles = [];
    const collect = (directory, prefix = '') => {
        fs.readdirSync(directory, { withFileTypes: true }).forEach((entry) => {
            const relPath = prefix ? `${prefix}/${entry.name}` : entry.name;
            const fullPath = path.join(directory, entry.name);
            if (entry.isDirectory()) {
                collect(fullPath, relPath);
            } else if (entry.isFile() && entry.name.endsWith('.js') && !entry.name.endsWith('.gz')) {
                actualFiles.push(relPath);
            }
        });
    };
    collect(PUBLISH_MODULE_DIR);
    actualFiles.sort();

    const missing = Array.from(expectedFiles).filter((fileName) => !actualFiles.includes(fileName)).sort();
    const orphaned = actualFiles.filter((fileName) => !expectedFiles.has(fileName));
    if (missing.length > 0 || orphaned.length > 0) {
        const details = [];
        if (missing.length > 0) details.push(`missing: ${missing.join(', ')}`);
        if (orphaned.length > 0) details.push(`unexpected: ${orphaned.join(', ')}`);
        throw new Error(`Web module source coverage failed (${details.join('; ')})`);
    }

    console.log(`  Source coverage: ${actualFiles.length} capability-selected module(s) traced to web-src/`);
    return { expectedFiles: Array.from(expectedFiles).sort(), actualFiles };
}

// Sub-directory bundles: 把入口 + 子模块合并为单文件 bundle，避免 ESP32 多次 HTTP 连接。
// 拼接顺序：[按 order 拼接的子模块] + [入口文件内容]，
// 入口文件已重写为合并模式（不再运行时动态加载子模块）。
const SUBDIR_BUNDLES = [
    {
        outName: 'protocol.js',
        entry: 'protocol.js',
        subDir: 'protocol',
        order: [
            'common.js',
            { file: 'mqtt-config.js', requirements: ['mqtt'] },
            'protocol-lite-config.js'
        ]
    },
    {
        outName: 'protocol-full-config.js',
        entry: null,
        subDir: 'protocol',
        order: ['protocol-config.js']
    },
    {
        outName: 'protocol-modbus-rtu.js',
        entry: null,
        subDir: 'protocol',
        order: ['modbus-config.js']
    },
    {
        outName: 'protocol-modbus-control.js',
        entry: null,
        subDir: 'protocol',
        order: ['modbus-control.js', 'modbus-relay-motor.js']
    },
    {
        outName: 'device-control.js',
        entry: 'device-control.js',
        subDir: 'device-control',
        order: ['core.js'],
        skipWithoutDC: true
    },
    {
        outName: 'device-control-view.js',
        entry: null,
        subDir: 'device-control',
        order: ['render.js', 'layout.js'],
        skipWithoutDC: true
    },
    {
        outName: 'device-control-modbus.js',
        entry: null,
        subDir: 'device-control',
        order: ['modbus-ops.js', 'modbus-ctrl.js'],
        skipWithoutDC: true
    },
    {
        outName: 'periph-exec.js',
        entry: 'periph-exec.js',
        subDir: null
    }
];

function activeSubdirBundles() {
    return SUBDIR_BUNDLES.filter((bundle) => {
        if (bundle.skipWithoutDC && !hasDeviceControl(BUILD_CONTEXT)) return false;
        return shouldIncludeAsset(`js/modules/${bundle.outName}`, BUILD_CONTEXT);
    });
}

// 在独立同步、子目录同步中需要跳过的文件（已被 bundle 包含）
const BUNDLED_ENTRY_FILES = new Set(activeSubdirBundles().map(b => b.entry).filter(Boolean));
const BUNDLED_SIBLING_FILES = new Set();
activeSubdirBundles().forEach(b => (b.siblings || []).forEach(s => BUNDLED_SIBLING_FILES.add(s)));
const BUNDLED_SUBDIRS = new Set(SUBDIR_BUNDLES.filter(b => b.subDir).map(b => b.subDir));

function buildSubdirBundles() {
    const noMinify = process.argv.includes('--no-minify');
    const results = [];

    activeSubdirBundles().forEach((bundle) => {
        const parts = [];

        // 1. 子目录下的按顺序子模块
        if (bundle.subDir) {
            const srcDir = path.join(RUNTIME_SOURCE_DIR, bundle.subDir);
            if (!fs.existsSync(srcDir)) {
                throw new Error(`Missing subdir for bundle ${bundle.outName}: ${srcDir}`);
            }
            bundle.order.forEach((source) => {
                const f = typeof source === 'string' ? source : source.file;
                const requirements = typeof source === 'string' ? [] : source.requirements;
                if (!capabilityRuleSatisfied(requirements, BUILD_CONTEXT)) return;
                const fp = path.join(srcDir, f);
                if (!fs.existsSync(fp)) {
                    throw new Error(`Missing sub-module ${bundle.subDir}/${f} for bundle ${bundle.outName}`);
                }
                parts.push(`/* ===== ${bundle.subDir}/${f} ===== */`);
                parts.push(readUtf8(fp).trim());
            });
        }

        // 2. 同级 sibling 子模块（如 periph-exec-modbus.js / periph-exec-form.js）
        if (bundle.siblings) {
            bundle.siblings.forEach((f) => {
                const fp = path.join(RUNTIME_SOURCE_DIR, f);
                if (!fs.existsSync(fp)) {
                    throw new Error(`Missing sibling ${f} for bundle ${bundle.outName}`);
                }
                parts.push(`/* ===== ${f} ===== */`);
                parts.push(readUtf8(fp).trim());
            });
        }

        // 3. 入口文件（已重写为合并模式，不再动态加载子模块）
        if (bundle.entry) {
            const entryPath = path.join(RUNTIME_SOURCE_DIR, bundle.entry);
            if (!fs.existsSync(entryPath)) {
                throw new Error(`Missing entry ${bundle.entry} for bundle ${bundle.outName}`);
            }
            parts.push(`/* ===== ${bundle.entry} (entry) ===== */`);
            parts.push(readUtf8(entryPath).trim());
        }

        const header = `/* AUTO-GENERATED bundle: ${bundle.outName} (entry + sub-modules merged). DO NOT EDIT. */\n`;
        const raw = header + parts.join('\n\n') + '\n';
        // The protocol entry bundles MQTT, Modbus tab orchestration and the
        // optional remote-resource editor. Mangle only local bindings so the
        // public AppState/API property names remain stable while the full S3
        // profile stays inside its per-bundle Flash budget.
        const content = noMinify ? raw : (bundle.outName === 'protocol.js'
            ? minify_sync(raw, { compress: false, mangle: true }).code
            : minifyJS(raw));

        const outPath = path.join(PUBLISH_MODULE_DIR, bundle.outName);
        writeUtf8(outPath, content);

        results.push({
            outName: bundle.outName,
            outPath,
            size: Buffer.byteLength(content, 'utf8'),
            sourceCount: parts.filter(p => !p.startsWith('/*')).length
        });
    });

    return results;
}

// 清理已被 bundle 取代的遗留产物：子目录 + 独立 sibling 文件（含 .gz）
function cleanBundledArtifacts() {
    const removed = [];
    const skipped = [];

    function forceRemovePath(targetPath) {
        if (!fs.existsSync(targetPath)) return;
        const stat = fs.lstatSync(targetPath);
        if (stat.isDirectory()) {
            fs.readdirSync(targetPath).forEach((entry) => {
                forceRemovePath(path.join(targetPath, entry));
            });
            fs.rmdirSync(targetPath);
            return;
        }
        try {
            fs.chmodSync(targetPath, 0o666);
        } catch (error) {
            // Best effort for Windows EPERM cleanup.
        }
        fs.unlinkSync(targetPath);
    }

    function tryRemove(targetPath, label, options) {
        try {
            fs.rmSync(targetPath, options || { force: true });
            removed.push(label);
        } catch (error) {
            try {
                forceRemovePath(targetPath);
                removed.push(label);
            } catch (fallbackError) {
                skipped.push(`${label} (${fallbackError.code || fallbackError.message || error.code || error.message})`);
            }
        }
    }

    // 删除子目录
BUNDLED_SUBDIRS.forEach((dirName) => {
        const dirPath = path.join(PUBLISH_MODULE_DIR, dirName);
        if (fs.existsSync(dirPath)) {
            tryRemove(dirPath, `modules/${dirName}/ (recursive)`, { recursive: true, force: true });
        }
    });

    // 删除独立 sibling 文件 + .gz
    BUNDLED_SIBLING_FILES.forEach((f) => {
        [path.join(PUBLISH_MODULE_DIR, f), path.join(PUBLISH_MODULE_DIR, f + '.gz')].forEach((fp) => {
            if (fs.existsSync(fp)) {
                tryRemove(fp, `modules/${path.basename(fp)}`, { force: true });
            }
        });
    });

    // 删除 entry bundle 的过时 .gz（入口文件已被 buildSubdirBundles 重生成，
    // 旧 .gz 会被 ESP32 .gz 优先路径返回造成代码不一致）
    BUNDLED_ENTRY_FILES.forEach((f) => {
        const gzPath = path.join(PUBLISH_MODULE_DIR, f + '.gz');
        if (fs.existsSync(gzPath)) {
            tryRemove(gzPath, `modules/${path.basename(gzPath)} (stale)`, { force: true });
        }
    });

    return { removed, skipped };
}

function cleanObsoleteRootJs() {
    const removed = [];
    const skipped = [];
    const jsDir = path.join(WWW_DIR, 'js');
    if (!fs.existsSync(jsDir)) return { removed, skipped };

    const allowed = new Set([
        ...Array.from(APP_CHUNK_FILES),
        ...Array.from(STANDALONE_ROOT_JS_SET)
    ]);

    fs.readdirSync(jsDir).forEach((entry) => {
        const fullPath = path.join(jsDir, entry);
        if (!fs.statSync(fullPath).isFile()) return;

        const isGz = entry.endsWith('.gz');
        const baseName = isGz ? entry.slice(0, -3) : entry;
        if (!baseName.endsWith('.js')) return;
        if (allowed.has(baseName)) return;

        try {
            fs.unlinkSync(fullPath);
            removed.push(entry);
        } catch (error) {
            skipped.push(`${entry} (${error.code || error.message})`);
        }
    });

    return { removed, skipped };
}

function cleanProdObsoleteModules() {
    // 统一架构：不再清理任何模块
    return [];
}

function buildWebModules() {
    console.log(`Web profile: ${getWebProfile(BUILD_CONTEXT)}`);
    console.log(`Compiled capability source: ${BUILD_CONTEXT.source}`);
    const removedGeneratedAssets = cleanGeneratedPublishTree();
    fs.mkdirSync(PUBLISH_MODULE_DIR, { recursive: true });
    const staticAssets = syncStaticAssets();
    const syncedModules = syncDirectRuntimeModules();
    const i18nModules = syncI18nModules();
    const adminBundle = buildAdminBundle(BUILD_CONTEXT);
    const appBundle = buildAppBundle({ transformSource: transformAppSourceContent });
    const subdirBundles = buildSubdirBundles();
    const bundledCleanup = cleanBundledArtifacts();
    const removedRootJs = cleanObsoleteRootJs();
    const removedProdModules = cleanProdObsoleteModules();

    validateSourceCoverage();
    const resourceManifest = writeWebResourceManifest(WWW_DIR, BUILD_CONTEXT);

    return {
        context: BUILD_CONTEXT,
        removedGeneratedAssets,
        staticAssets,
        syncedModules,
        i18nModules,
        adminBundle,
        appBundle,
        subdirBundles,
        bundledCleanup,
        removedRootJs,
        removedProdModules,
        resourceManifest
    };
}

if (require.main === module) {
    const result = withWebPublishLock(buildWebModules, { label: 'build-web-modules' });
    console.log(`Synced ${result.staticAssets.length} static assets from web-src/`);
    result.staticAssets.forEach((item) => {
        console.log(`  Copied: ${item.src} -> data/www/${item.dest} (${item.size} bytes)`);
    });
    console.log(`Synced ${result.syncedModules.length} direct runtime modules`);
    result.syncedModules.forEach((item) => {
        console.log(`  Synced: ${path.relative(ROOT_DIR, item.publishFile)} (${item.size} bytes)`);
    });
    console.log(`Synced ${result.i18nModules.length} i18n modules`);
    result.i18nModules.forEach((item) => {
        console.log(`  Synced: ${path.relative(ROOT_DIR, item.publishFile)} (${item.size} bytes${item.minified ? ', minified' : ''})`);
    });
    console.log(`Built admin bundle: ${path.relative(ROOT_DIR, result.adminBundle.outputFile)} (${result.adminBundle.size} bytes)`);
    console.log(`Built app bundle: ${path.relative(ROOT_DIR, result.appBundle.outputFile)} (${result.appBundle.size} bytes${result.appBundle.minified ? ', minified' : ''})`);
    if (result.subdirBundles.length > 0) {
        console.log(`Built ${result.subdirBundles.length} merged runtime bundle(s):`);
        result.subdirBundles.forEach((item) => {
            console.log(`  Built: ${path.relative(ROOT_DIR, item.outPath)} (${item.size} bytes, ${item.sourceCount} source files)`);
        });
    }
    if (result.bundledCleanup.removed.length > 0) {
        console.log(`Cleaned ${result.bundledCleanup.removed.length} bundled runtime artifact(s):`);
        result.bundledCleanup.removed.forEach((p) => console.log(`  Removed: ${p}`));
    }
    if (result.bundledCleanup.skipped.length > 0) {
        console.log(`Skipped ${result.bundledCleanup.skipped.length} locked bundled runtime artifact(s):`);
        result.bundledCleanup.skipped.forEach((p) => console.log(`  Skip: ${p}`));
    }
    if (result.removedRootJs.removed.length > 0) {
        console.log(`Cleaned ${result.removedRootJs.removed.length} obsolete root JS artifact(s):`);
        result.removedRootJs.removed.forEach((p) => console.log(`  Removed: data/www/js/${p}`));
    }
    if (result.removedRootJs.skipped.length > 0) {
        console.log(`Skipped ${result.removedRootJs.skipped.length} locked root JS artifact(s):`);
        result.removedRootJs.skipped.forEach((p) => console.log(`  Skip: data/www/js/${p}`));
    }
}

module.exports = {
    buildWebModules,
    compiledModuleSequences,
    syncStaticAssets,
    syncDirectRuntimeModules,
    syncI18nModules,
    validateSourceCoverage,
    minifyCSS,
    transformAppSourceContent,
    transformStaticAssetContent,
    ALL_DIRECT_RUNTIME_MODULES,
    DIRECT_RUNTIME_MODULES,
    RUNTIME_SUB_DIRS,
    I18N_MODULES,
    RUNTIME_SOURCE_DIR,
    I18N_SOURCE_DIR,
    PUBLISH_MODULE_DIR,
    WEB_SRC_DIR,
    WWW_DIR
};
