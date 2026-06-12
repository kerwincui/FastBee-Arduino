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
const { buildAdminBundle, SOURCE_FILES: ADMIN_SOURCE_FILES } = require('./build-admin-bundle');
const {
    getWebProfile,
    isCompactWebProfile,
    isLiteWebProfile,
    isFullWebProfile
} = require('./web-profile');
const {
    buildAppBundle,
    BUNDLE_SOURCES: APP_BUNDLE_SOURCES,
    CHUNK_GROUPS: APP_CHUNK_GROUPS,
    extractZhTranslations,
    filterProdI18nTranslations
} = require('./build-app-bundle');
const { minifyJS } = require('./minify-js');

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
const DIRECT_RUNTIME_MODULES = [
    'protocol.js',
    'periph-exec.js',
    'periph-exec-form.js',
    'periph-exec-modbus.js',
    'peripherals.js',
    'dashboard.js',
    'device-config.js',
    'network.js'
].concat(isLiteWebProfile() ? [] : ['device-control.js'])
 .concat(isFullWebProfile() ? ['dashboard-fullscreen.js'] : []);
const I18N_MODULES = isCompactWebProfile()
    ? []
    : [
        { file: 'i18n-zh-CN.js', minify: false },
        { file: 'i18n-en.js', minify: false, optional: true, fallbackPublish: true }
    ];
const COMPACT_STATIC_SKIP = new Set([
    'pages/admin.html',
    'pages/fullscreen.html',
    'pages/logs.html',
    'pages/rule-script.html',
    'pages/fragments/device-ota.html',
    'pages/fragments/protocol-coap.html',
    'pages/fragments/protocol-http.html',
    'pages/fragments/protocol-tcp.html'
]);
const LITE_STATIC_SKIP = new Set([
    'pages/fragments/protocol-modbus-rtu.html'
]);
const PROD_IGNORED_ORPHAN_MODULES = new Set([
    'files.js',
    'i18n-en.js',
    'i18n-engine.js',
    'i18n-zh-CN.js',
    'logs.js',
    'roles.js',
    'rule-script.js',
    'users.js',
    'dashboard-fullscreen.js'
]);
const STANDALONE_ROOT_JS = isFullWebProfile()
    ? [{ file: 'notification.js', minify: true }]
    : [];
const STANDALONE_ROOT_JS_SET = new Set(STANDALONE_ROOT_JS.map((item) => item.file));
const APP_CHUNK_FILES = new Set(APP_CHUNK_GROUPS.map((chunk) => chunk.outName));

function readUtf8(filePath) {
    return fs.readFileSync(filePath, 'utf8').replace(/^\uFEFF/, '');
}

function writeUtf8(filePath, content) {
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

function shouldSkipStaticAsset(relPath) {
    const normalized = normalizePath(relPath);
    return (isCompactWebProfile() && COMPACT_STATIC_SKIP.has(normalized))
        || (isLiteWebProfile() && LITE_STATIC_SKIP.has(normalized));
}

function stripSection(content, startMarker, endMarker) {
    const start = content.indexOf(startMarker);
    if (start < 0) return content;
    if (!endMarker) return content.slice(0, start);

    const end = content.indexOf(endMarker, start + startMarker.length);
    if (end < 0) return content.slice(0, start);
    return content.slice(0, start) + content.slice(end);
}

function stripMenuItemByPage(content, pageName) {
    const escaped = pageName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    return content.replace(
        new RegExp(`\\s*<a\\s+href="#"\\s+class="menu-item"\\s+data-page="${escaped}"[\\s\\S]*?<\\/a>`, 'g'),
        ''
    );
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
        /^\.fs-/,
        /^\.logs-/,
        /^\.log-content/,
        /^\.log-/,
        /^\.file-tree-/,
        /^\.role-/,
        /^\.rule-script-/,
        /^\.ota-/,
        /^\.ota-inline-/,
        /^body\.fullscreen-page/,
        /^\.fullscreen-container/,
        /^\.fullscreen-toolbar/,
        /^@keyframes\s+fs-/,
        /^\[data-theme="dark"\]\s+\.fs-/,
        /^\[data-theme="dark"\]\s+\.logs-/,
        /^\[data-theme="dark"\]\s+\.log-/,
        /^\[data-theme="dark"\]\s+\.file-tree-/,
        /^\[data-theme="dark"\]\s+\.role-/,
        /^\[data-theme="dark"\]\s+\.rule-script-/,
        /^\[data-theme="dark"\]\s+\.ota-/
    ];

    // 精简版额外移除：暗黑主题 + device-control + Modbus UI 相关样式
    if (isLiteWebProfile()) {
        dropSelectors.push(
            /^\[data-theme="dark"\]/,           // 所有暗黑主题规则
            /^\/\*\s*Dark\s/,                    // Dark Theme 注释行
            /^\.dc-/,                             // device-control 样式
            /^\.fullscreen-mode\s+\.dc-/,        // fullscreen 下的 dc 样式
            /^\.coil-/,                            // 继电器线圈卡片
            /^\.pwm-/,                             // PWM 通道
            /^\.sensor-grid/,                      // 传感器网格
            /^\.sensor-card/,                      // 传感器卡片
            /^\.sensor-ch-/,                       // 传感器通道配置
            /^\.pid-/,                             // PID 控制器
            /^\.motor-/,                           // 电机控制面板
            /^#master-data-grid/,                   // Modbus 采集数据网格
            /^\.master-data-/,                     // Modbus 采集数据项
            /^\.modbus-/,                          // Modbus UI 样式
            /^#dc-zoom-reset-btn/                   // device-control 缩放按钮
        );
    }

    return content
        .split(/\r?\n/)
        .filter((line) => {
            const trimmed = line.trim();
            return !dropSelectors.some((pattern) => pattern.test(trimmed));
        })
        .join('\n');
}

function transformStaticAssetContent(relPath, content) {
    if (!isCompactWebProfile()) return content;

    const normalized = normalizePath(relPath);
    if (normalized === 'pages/modals.html') {
        let next = content;
        next = stripSection(next, '<!-- 规则脚本模态窗 -->', '<!-- 兼容旧版：GPIO配置模态窗 -->');
        next = stripSection(next, '<!-- 添加用户模态窗 -->', '');
        if (isLiteWebProfile()) {
            // 精简版移除兼容旧版GPIO配置模态窗（已被新外设配置系统替代）
            next = stripSection(next, '<!-- 兼容旧版：GPIO配置模态窗 -->', '');
        }
        return next;
    }

    if (normalized === 'index.html') {
        let next = ['logs', 'data', 'users', 'roles', 'rule-script']
            .reduce((acc, page) => stripMenuItemByPage(acc, page), content);
        if (isLiteWebProfile()) {
            next = stripMenuItemByPage(next, 'device-control');
            // 移除主题切换按钮（精简版无暗黑主题）
            next = next.replace(/\s*<button\s+class="dropdown-item"\s+id="theme-toggle-item"[\s\S]*?<\/button>/g, '');
            // 移除 Service Worker 注册代码块
            next = next.replace(/\s*<!-- Service Worker 注册 -->\s*<script>[\s\S]*?serviceWorker[\s\S]*?<\/script>/g, '');
        }
        next = stripDivByClass(next, 'login-lang-switch');
        if (!isFullWebProfile()) {
            next = stripSelectById(next, 'language-select');
            next = next.replace(/\.login-lang-switch\{[^}]*\}\.login-lang-switch select,\s*\.header-actions select\{[^}]*\}/g, '');
        } else {
            // Full版本保留 .header-actions select 样式，仅移除 .login-lang-switch
            next = next.replace(/\.login-lang-switch\{[^}]*\}/g, '');
            next = next.replace(/\.login-lang-switch select,\s*/g, '');
        }
        return next;
    }

    if (normalized === 'pages/protocol.html' && isLiteWebProfile()) {
        let next = content;
        next = next.replace(/\s*<div class="config-tab" data-tab="modbus-rtu">[\s\S]*?<\/div>/g, '');
        next = next.replace(
            /\s*<!-- Modbus RTU 配置[\s\S]*?-->\s*<div id="modbus-rtu" class="config-content">[\s\S]*?<div id="modbus-rtu-fragment-container">[\s\S]*?<\/div>\s*<\/div>/,
            ''
        );
        return next;
    }

    if (normalized === 'css/main.css') {
        let css = stripProdOnlyCssRules(content)
            .replace(/@media\s*\(max-width:\s*768px\)\s*\{[^{}]*\.ota-inline-action-row[^{}]*\}/g, '')
            .replace(/@media\s*\(max-width:\s*768px\)\s*\{[^{}]*\.fs-[^{}]*\}/g, '')
            .replace(/\.ota-inline-action-row\s*\{[^}]*\}/g, '')
            .replace(/\.ota-inline-input-wrap\s*\{[^}]*\}/g, '')
            .replace(/\.ota-inline-action-row\s+\.fb-btn\s*\{[^}]*\}/g, '')
            .replace(/\.log-content\s*\{[^}]*\}/g, '')
            .replace(/\.rule-script-stats-cell\s*\{[^}]*\}/g, '');
        if (isLiteWebProfile()) {
            // 移除暗黑主题变量声明块（多行: [data-theme="dark"] { ... }）
            css = css.replace(/\/\*\s*Dark Theme[\s\S]*?\n\}/g, '');
            // 移除 fullscreen-mode 内的 dc 样式块
            css = css.replace(/\.fullscreen-mode\s+\.dc-[^{]*\{[^}]*\}/g, '');
            // 移除 @media 内 device-control / modbus 相关规则
            css = css.replace(/@media\s*\(max-width:\s*\d+px\)\s*\{[^{}]*\.motor-[^{}]*\}/g, '');
            css = css.replace(/@media\s*\(max-width:\s*\d+px\)\s*\{[^{}]*\.modbus-[^{}]*\}/g, '');
        }
        return css;
    }

    return content;
}

function normalizePath(relPath) {
    return relPath.replace(/\\/g, '/').replace(/^\.\//, '');
}

// Sub-directories under runtime/ that contain split sub-modules
const RUNTIME_SUB_DIRS = ['protocol', 'device-control'];

function syncDirectRuntimeModules() {
    const results = [];
    const noMinify = process.argv.includes('--no-minify');

    // 1. Sync top-level entry files
    if (isCompactWebProfile()) {
        removeGeneratedFile(path.join(PUBLISH_MODULE_DIR, 'dashboard-fullscreen.js'));
    }

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
        const content = noMinify ? raw : minifyJS(raw);
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
        let content;

        if (isCompactWebProfile() && file === 'i18n-zh-CN.js') {
            const filtered = filterProdI18nTranslations(extractZhTranslations());
            content = `(function(){if(typeof i18n==='undefined')return;i18n.addTranslations('zh-CN',${JSON.stringify(filtered)});i18n._zhLoaded=true;})();\n`;
            if (shouldMinify && !noMinify) {
                content = minifyJS(content);
            }
        } else {
            const raw = fallbackContent !== null ? fallbackContent : readUtf8(sourceFile);
            content = (shouldMinify && !noMinify) ? minifyJS(raw) : raw;
        }

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
    ].concat(isLiteWebProfile() ? [] : [{ src: 'sw.js', dest: 'sw.js' }]);

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
        if (isCompactWebProfile()) {
            removeGeneratedFile(path.join(jsDestDir, 'notification.js'));
        }
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
    if (!fs.existsSync(PUBLISH_MODULE_DIR)) return;

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
    ADMIN_SOURCE_FILES.forEach((f) => expectedFiles.add(f)); // admin stubs

    // Add sub-directory files to expected set
    RUNTIME_SUB_DIRS.forEach((subDir) => {
        const srcDir = path.join(RUNTIME_SOURCE_DIR, subDir);
        if (!fs.existsSync(srcDir)) return;
        fs.readdirSync(srcDir).forEach((file) => {
            if (file.endsWith('.js')) expectedFiles.add(subDir + '/' + file);
        });
    });

    const actualFiles = fs.readdirSync(PUBLISH_MODULE_DIR)
        .filter((f) => f.endsWith('.js') && !f.endsWith('.gz'));

    // Also check sub-directories
    RUNTIME_SUB_DIRS.forEach((subDir) => {
        const pubSubDir = path.join(PUBLISH_MODULE_DIR, subDir);
        if (fs.existsSync(pubSubDir)) {
            fs.readdirSync(pubSubDir)
                .filter((f) => f.endsWith('.js') && !f.endsWith('.gz'))
                .forEach((f) => actualFiles.push(subDir + '/' + f));
        }
    });

    const ignoredProdOrphans = [];
    const orphaned = actualFiles.filter((f) => {
        if (f === 'i18n-engine.js' || expectedFiles.has(f)) return false;
        if (isCompactWebProfile() && PROD_IGNORED_ORPHAN_MODULES.has(f)) {
            ignoredProdOrphans.push(f);
            return false;
        }
        return true;
    });
    if (orphaned.length > 0) {
        console.log(`\n  WARNING: ${orphaned.length} orphaned file(s) in data/www/js/modules/:`);
        orphaned.forEach((f) => console.log(`    - ${f} (not traced to web-src/)`));
        console.log('  These files should be moved to web-src/ or removed.\n');
    } else {
        console.log(`  Source coverage: all ${actualFiles.length} output modules traced to web-src/`);
        if (ignoredProdOrphans.length > 0) {
            console.log(`  Slim ignored stale module(s): ${ignoredProdOrphans.length}`);
        }
    }
}

// Sub-directory bundles: 把入口 + 子模块合并为单文件 bundle，避免 ESP32 多次 HTTP 连接。
// 拼接顺序：[按 order 拼接的子模块] + [入口文件内容]，
// 入口文件已重写为合并模式（不再运行时动态加载子模块）。
const SUBDIR_BUNDLES = [
    {
        outName: 'protocol.js',
        entry: 'protocol.js',
        subDir: 'protocol',
        order: ['common.js', 'mqtt-config.js', 'protocol-lite-config.js']
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
        order: ['modbus-config.js'],
        skipProfiles: ['lite']
    },
    {
        outName: 'protocol-modbus-control.js',
        entry: null,
        subDir: 'protocol',
        order: ['modbus-control.js', 'modbus-relay-motor.js'],
        skipProfiles: ['lite']
    },
    {
        outName: 'device-control.js',
        entry: 'device-control.js',
        subDir: 'device-control',
        order: ['core.js'],
        skipProfiles: ['lite']
    },
    {
        outName: 'device-control-view.js',
        entry: null,
        subDir: 'device-control',
        order: ['render.js', 'layout.js'],
        skipProfiles: ['lite']
    },
    {
        outName: 'device-control-modbus.js',
        entry: null,
        subDir: 'device-control',
        order: ['modbus-ops.js', 'modbus-ctrl.js'],
        skipProfiles: ['lite']
    },
    {
        outName: 'periph-exec.js',
        entry: 'periph-exec.js',
        subDir: null
    }
];

function activeSubdirBundles() {
    const profile = getWebProfile();
    return SUBDIR_BUNDLES.filter((bundle) => !(bundle.skipProfiles || []).includes(profile));
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
            bundle.order.forEach((f) => {
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
        const content = noMinify ? raw : minifyJS(raw);

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
    const removed = [];
    if (!isCompactWebProfile()) return removed;
    const fileNames = new Set(PROD_IGNORED_ORPHAN_MODULES);
    if (isLiteWebProfile()) {
        fileNames.add('protocol-modbus-rtu.js');
        fileNames.add('protocol-modbus-control.js');
        fileNames.add('device-control.js');
        fileNames.add('device-control-view.js');
        fileNames.add('device-control-modbus.js');
    }
    fileNames.forEach((fileName) => {
        const targetPath = path.join(PUBLISH_MODULE_DIR, fileName);
        removeGeneratedFile(targetPath);
        removed.push(fileName);
    });
    return removed;
}

function buildWebModules() {
    console.log(`Web profile: ${getWebProfile()}`);
    const staticAssets = syncStaticAssets();
    const syncedModules = syncDirectRuntimeModules();
    const i18nModules = syncI18nModules();
    const adminBundle = buildAdminBundle();
    const appBundle = buildAppBundle();
    const subdirBundles = buildSubdirBundles();
    const bundledCleanup = cleanBundledArtifacts();
    const removedRootJs = cleanObsoleteRootJs();
    const removedProdModules = cleanProdObsoleteModules();

    validateSourceCoverage();

    return {
        staticAssets,
        syncedModules,
        i18nModules,
        adminBundle,
        appBundle,
        subdirBundles,
        bundledCleanup,
        removedRootJs,
        removedProdModules
    };
}

if (require.main === module) {
    const result = buildWebModules();
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
    syncStaticAssets,
    syncDirectRuntimeModules,
    syncI18nModules,
    validateSourceCoverage,
    minifyCSS,
    DIRECT_RUNTIME_MODULES,
    RUNTIME_SUB_DIRS,
    I18N_MODULES,
    RUNTIME_SOURCE_DIR,
    I18N_SOURCE_DIR,
    PUBLISH_MODULE_DIR,
    WEB_SRC_DIR,
    WWW_DIR
};
