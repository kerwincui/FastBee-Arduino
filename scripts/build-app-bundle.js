/**
 * Build the core app chunks from individual JS source files.
 *
 * Splits the legacy single app-bundle.js into three pressure-aware chunks so
 * each response remains practical on fragmented ESP32 heaps.
 *
 * Chunks (loaded sequentially via <script defer> in index.html):
 *   1. chunk-1-core.js  : utils + ui-components + request-governor
 *   2. chunk-2-app.js   : notification + page/module loaders + API preloader
 *   3. chunk-3-state.js : state modules + main
 *
 * The prod web build keeps a single zh-CN language pack.
 *
 * Usage:
 *   node scripts/build-app-bundle.js
 *   node scripts/build-app-bundle.js --no-minify
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { minifyJS } = require('./minify-js');

const ROOT_DIR = path.join(__dirname, '..');
const JS_SRC_DIR = path.join(ROOT_DIR, 'web-src', 'js');
const WWW_JS_DIR = path.join(ROOT_DIR, 'data', 'www', 'js');

function readUtf8(fp) {
    return fs.readFileSync(fp, 'utf8');
}
const CRITICAL_ZH_TRANSLATIONS = {
    'login-title': 'FastBee Arduino',
    'username-label': '用户名',
    'username-placeholder': '请输入用户名',
    'password-label': '密码',
    'password-placeholder': '请输入密码',
    'remember-label': '记住密码',
    'login-button': '登录系统',
    'login-logging-in': '登录中...',
    'login-logging-in-html': '<i class="fas fa-spinner fa-spin"></i> 登录中...',
    'login-success-msg': '登录成功',
    'login-welcome-title': '欢迎',
    'login-fail-title': '登录失败',
    'login-fail-msg': '用户名或密码错误',
    'login-empty-warning': '请输入用户名和密码'
};

// Source files in dependency order (kept for backward-compat / fallback path in index.html)
const BUNDLE_SOURCES = [
    { dir: JS_SRC_DIR, file: 'utils.js' },
    { dir: JS_SRC_DIR, file: 'ui-components.js' },
    { dir: JS_SRC_DIR, file: 'request-governor.js' },
    { dir: JS_SRC_DIR, file: 'notification.js' },
    { dir: JS_SRC_DIR, file: 'page-loader.js' },
    { dir: JS_SRC_DIR, file: 'module-loader.js' },
    { dir: JS_SRC_DIR, file: 'api-preloader.js' },
    { dir: JS_SRC_DIR, file: 'state.js' },
    { dir: JS_SRC_DIR, file: 'state-theme.js' },
    { dir: JS_SRC_DIR, file: 'state-session.js' },
    { dir: JS_SRC_DIR, file: 'state-sse.js' },
    { dir: JS_SRC_DIR, file: 'state-ui.js' },
    { dir: JS_SRC_DIR, file: 'main.js' }
];

const CHUNK_DEFINITIONS = [
    {
        outName: 'chunk-1-core.js',
        files: [
            { dir: JS_SRC_DIR, file: 'utils.js' },
            { dir: JS_SRC_DIR, file: 'ui-components.js' },
            { dir: JS_SRC_DIR, file: 'request-governor.js' }
        ]
    },
    {
        outName: 'chunk-2-app.js',
        files: [
            { dir: JS_SRC_DIR, file: 'notification.js' },
            { dir: JS_SRC_DIR, file: 'page-loader.js' },
            { dir: JS_SRC_DIR, file: 'module-loader.js' },
            { dir: JS_SRC_DIR, file: 'api-preloader.js' }
        ]
    },
    {
        outName: 'chunk-3-state.js',
        files: [
            { dir: JS_SRC_DIR, file: 'state.js' },
            { dir: JS_SRC_DIR, file: 'state-theme.js' },
            { dir: JS_SRC_DIR, file: 'state-session.js' },
            { dir: JS_SRC_DIR, file: 'state-sse.js' },
            { dir: JS_SRC_DIR, file: 'state-ui.js' },
            { dir: JS_SRC_DIR, file: 'main.js' }
        ]
    }
];

const CHUNK_GROUPS = CHUNK_DEFINITIONS;

// i18n removed - stub for backward compatibility
function extractZhTranslations() {
    return { ...CRITICAL_ZH_TRANSLATIONS };
}

function filterProdI18nTranslations(all) {
    return { ...all };
}

// No-op stub - i18n zh split chunks removed
function buildI18nZhChunk() {
    return '';
}

// Old single-bundle outputs that should be cleaned up
const OBSOLETE_OUTPUTS = [
    'app-bundle.js',
    'chunk-1-core-a.js',
    'chunk-2-core-b.js',
    'chunk-3-i18n-engine.js',
    'chunk-4-i18n-zh-1.js',
    'chunk-5-i18n-zh-2.js',
    'chunk-6-i18n-zh-3.js',
    'chunk-7-i18n-zh-4.js',
    'chunk-8-state-1.js',
    'chunk-9-state-2.js'
];

function readChunkSource(dir, file) {
    return readUtf8(path.join(dir, file)).trim();
}

function buildChunkContent(chunk, options = {}) {
    if (chunk.i18nZhSplit) {
        return buildI18nZhChunk(chunk.i18nZhSplit);
    }
    if (!chunk.files || chunk.files.length === 0) {
        throw new Error(`Chunk ${chunk.outName} has no files configured`);
    }
    const sections = chunk.files.map(({ dir, file }) => {
        const fp = path.join(dir, file);
        if (!fs.existsSync(fp)) {
            throw new Error(`Missing source file: ${fp}`);
        }
        const source = readChunkSource(dir, file);
        const transformed = typeof options.transformSource === 'function'
            ? options.transformSource(file, source)
            : source;
        return [
            `/* ===== ${file} ===== */`,
            transformed
        ].join('\n');
    });
    return sections.join('\n\n') + '\n';
}

function buildAppChunks(options = {}) {
    if (!fs.existsSync(WWW_JS_DIR)) {
        fs.mkdirSync(WWW_JS_DIR, { recursive: true });
    }

    const noMinify = process.argv.includes('--no-minify');
    const results = [];

    CHUNK_GROUPS.forEach((chunk) => {
        const raw = buildChunkContent(chunk, options);
        const finalContent = noMinify ? raw : minifyJS(raw);
        const outPath = path.join(WWW_JS_DIR, chunk.outName);
        fs.writeFileSync(outPath, finalContent, 'utf8');
        results.push({
            outName: chunk.outName,
            outPath,
            size: Buffer.byteLength(finalContent, 'utf8')
        });
    });

    // Remove obsolete single-bundle outputs (and any stale .gz left from previous builds)
    OBSOLETE_OUTPUTS.forEach((fname) => {
        [fname, `${fname}.gz`].forEach((target) => {
            const fp = path.join(WWW_JS_DIR, target);
            if (fs.existsSync(fp)) {
                try { fs.unlinkSync(fp); } catch (_) { /* ignore */ }
            }
        });
    });

    return {
        chunks: results,
        chunkCount: results.length,
        totalSize: results.reduce((s, c) => s + c.size, 0),
        minified: !noMinify
    };
}

// Backward-compat shim: build-web-modules.js still calls buildAppBundle()
// and reads { outputFile, size, minified } from the result for logging.
function buildAppBundle(options = {}) {
    const r = buildAppChunks(options);
    return {
        outputFile: r.chunks[0].outPath, // legacy field — first chunk path for logging
        fileCount: r.chunkCount,
        size: r.totalSize,
        minified: r.minified,
        sources: CHUNK_GROUPS.map((c) => c.outName),
        chunks: r.chunks
    };
}

if (require.main === module) {
    const result = buildAppBundle();
    console.log(`Built ${result.chunks.length} app chunks (replacing legacy app-bundle.js):`);
    result.chunks.forEach((c) => {
        console.log(`  ${c.outName.padEnd(28)} ${String(c.size).padStart(7)} bytes`);
    });
    console.log(`Total: ${result.size} bytes${result.minified ? ' (minified)' : ''}`);
}

module.exports = {
    buildAppBundle,
    buildAppChunks,
    extractZhTranslations,
    filterProdI18nTranslations,
    BUNDLE_SOURCES,
    CHUNK_GROUPS,
    OBSOLETE_OUTPUTS,
    JS_SRC_DIR,
    WWW_JS_DIR
};
