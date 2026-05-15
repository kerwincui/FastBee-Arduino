/**
 * Build the core app chunks from individual JS source files.
 *
 * Splits the legacy single app-bundle.js (~35KB gz) into 9 small chunks
 * (each ≤5KB gz) so each file fits within the ESP32 single-largest-free-block
 * constraint observed in production (~6KB after fragmentation), avoiding
 * OOM-induced timeout / cascading deadlock when one chunk fails to allocate.
 *
 * Chunks (loaded sequentially via <script defer> in index.html):
 *   1. chunk-1-core-a.js        : utils + ui-components + request-governor
 *   2. chunk-2-core-b.js        : notification + page-loader + module-loader
 *   3. chunk-3-i18n-engine.js   : i18n-engine
 *   4. chunk-4-i18n-zh-1.js     : i18n-zh-CN quarter 1
 *   5. chunk-5-i18n-zh-2.js     : i18n-zh-CN quarter 2
 *   6. chunk-6-i18n-zh-3.js     : i18n-zh-CN quarter 3
 *   7. chunk-7-i18n-zh-4.js     : i18n-zh-CN quarter 4 (sets _zhLoaded=true)
 *   8. chunk-8-state-1.js       : state + state-theme + state-session
 *   9. chunk-9-state-2.js       : state-sse + state-ui + main
 *
 * i18n-en.js is NOT bundled (loaded on-demand via i18n-engine).
 *
 * Usage:
 *   node scripts/build-app-bundle.js
 *   node scripts/build-app-bundle.js --no-minify
 */

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { minifyJS } = require('./minify-js');

const ROOT_DIR = path.join(__dirname, '..');
const JS_SRC_DIR = path.join(ROOT_DIR, 'web-src', 'js');
const I18N_SRC_DIR = path.join(ROOT_DIR, 'web-src', 'i18n');
const WWW_JS_DIR = path.join(ROOT_DIR, 'data', 'www', 'js');

// Source files in dependency order (kept for backward-compat / fallback path in index.html)
const BUNDLE_SOURCES = [
    { dir: JS_SRC_DIR, file: 'utils.js' },
    { dir: JS_SRC_DIR, file: 'ui-components.js' },
    { dir: JS_SRC_DIR, file: 'request-governor.js' },
    { dir: JS_SRC_DIR, file: 'notification.js' },
    { dir: JS_SRC_DIR, file: 'page-loader.js' },
    { dir: JS_SRC_DIR, file: 'module-loader.js' },
    { dir: I18N_SRC_DIR, file: 'i18n-engine.js' },
    { dir: I18N_SRC_DIR, file: 'i18n-zh-CN.js' },
    { dir: JS_SRC_DIR, file: 'state.js' },
    { dir: JS_SRC_DIR, file: 'state-theme.js' },
    { dir: JS_SRC_DIR, file: 'state-session.js' },
    { dir: JS_SRC_DIR, file: 'state-sse.js' },
    { dir: JS_SRC_DIR, file: 'state-ui.js' },
    { dir: JS_SRC_DIR, file: 'main.js' }
];

// Chunk groups (must be loaded by index.html in this exact order)
const CHUNK_GROUPS = [
    {
        outName: 'chunk-1-core-a.js',
        files: [
            { dir: JS_SRC_DIR, file: 'utils.js' },
            { dir: JS_SRC_DIR, file: 'ui-components.js' },
            { dir: JS_SRC_DIR, file: 'request-governor.js' }
        ]
    },
    {
        outName: 'chunk-2-core-b.js',
        files: [
            { dir: JS_SRC_DIR, file: 'notification.js' },
            { dir: JS_SRC_DIR, file: 'page-loader.js' },
            { dir: JS_SRC_DIR, file: 'module-loader.js' }
        ]
    },
    {
        outName: 'chunk-3-i18n-engine.js',
        files: [
            { dir: I18N_SRC_DIR, file: 'i18n-engine.js' }
        ]
    },
    { outName: 'chunk-4-i18n-zh-1.js', i18nZhSplit: { quarter: 1, total: 4 } },
    { outName: 'chunk-5-i18n-zh-2.js', i18nZhSplit: { quarter: 2, total: 4 } },
    { outName: 'chunk-6-i18n-zh-3.js', i18nZhSplit: { quarter: 3, total: 4 } },
    { outName: 'chunk-7-i18n-zh-4.js', i18nZhSplit: { quarter: 4, total: 4 } },
    {
        outName: 'chunk-8-state-1.js',
        files: [
            { dir: JS_SRC_DIR, file: 'state.js' },
            { dir: JS_SRC_DIR, file: 'state-theme.js' },
            { dir: JS_SRC_DIR, file: 'state-session.js' }
        ]
    },
    {
        outName: 'chunk-9-state-2.js',
        files: [
            { dir: JS_SRC_DIR, file: 'state-sse.js' },
            { dir: JS_SRC_DIR, file: 'state-ui.js' },
            { dir: JS_SRC_DIR, file: 'main.js' }
        ]
    }
];

// Files left over from earlier layouts that must be removed
const OBSOLETE_OUTPUTS = [
    'app-bundle.js', 'app-bundle.js.gz',
    'chunk-5-state.js', 'chunk-5-state.js.gz',
    'chunk-1-core.js', 'chunk-1-core.js.gz',
    'chunk-2-i18n-engine.js', 'chunk-2-i18n-engine.js.gz',
    'chunk-3-i18n-zh-1.js', 'chunk-3-i18n-zh-1.js.gz',
    'chunk-4-i18n-zh-2.js', 'chunk-4-i18n-zh-2.js.gz',
    'chunk-5-i18n-zh-3.js', 'chunk-5-i18n-zh-3.js.gz',
    'chunk-6-i18n-zh-4.js', 'chunk-6-i18n-zh-4.js.gz',
    'chunk-7-state-1.js', 'chunk-7-state-1.js.gz',
    'chunk-8-state-2.js', 'chunk-8-state-2.js.gz'
];

function readUtf8(filePath) {
    return fs.readFileSync(filePath, 'utf8').replace(/^\uFEFF/, '');
}

// Run i18n-zh-CN.js in a sandbox to extract its translation object,
// avoiding fragile regex/line-based splitting of the source file.
function extractZhTranslations() {
    const fp = path.join(I18N_SRC_DIR, 'i18n-zh-CN.js');
    if (!fs.existsSync(fp)) {
        throw new Error(`Missing i18n source: ${fp}`);
    }
    const content = readUtf8(fp);
    const sandbox = {
        i18n: {
            _captured: null,
            addTranslations(lang, data) {
                this._captured = data;
            }
        }
    };
    vm.createContext(sandbox);
    vm.runInContext(content, sandbox, { filename: 'i18n-zh-CN.js' });
    if (!sandbox.i18n._captured) {
        throw new Error('Failed to extract zhTranslations from i18n-zh-CN.js (sandbox executed but no addTranslations called)');
    }
    return sandbox.i18n._captured;
}

function buildI18nZhChunk(spec) {
    // spec: { quarter: 1..N, total: N }
    const { quarter, total } = spec;
    if (!Number.isInteger(quarter) || !Number.isInteger(total) || quarter < 1 || quarter > total) {
        throw new Error(`Invalid i18nZhSplit spec: ${JSON.stringify(spec)}`);
    }
    const all = extractZhTranslations();
    const keys = Object.keys(all);
    const N = keys.length;
    const start = Math.floor((N * (quarter - 1)) / total);
    const end = (quarter === total) ? N : Math.floor((N * quarter) / total);
    const sliceKeys = keys.slice(start, end);
    const obj = {};
    for (const k of sliceKeys) {
        obj[k] = all[k];
    }
    const isLastChunk = (quarter === total);
    const tail = isLastChunk ? "i18n._zhLoaded=true;" : "";
    // Compact JSON keeps Chinese characters as raw UTF-8 (no escaping).
    const jsonStr = JSON.stringify(obj);
    return `(function(){if(typeof i18n==='undefined')return;i18n.addTranslations('zh-CN',${jsonStr});${tail}})();\n`;
}

function buildChunkContent(chunk) {
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
        return [
            `/* ===== ${file} ===== */`,
            readUtf8(fp).trim()
        ].join('\n');
    });
    return sections.join('\n\n') + '\n';
}

function buildAppChunks() {
    if (!fs.existsSync(WWW_JS_DIR)) {
        fs.mkdirSync(WWW_JS_DIR, { recursive: true });
    }

    const noMinify = process.argv.includes('--no-minify');
    const results = [];

    CHUNK_GROUPS.forEach((chunk) => {
        const raw = buildChunkContent(chunk);
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
        const fp = path.join(WWW_JS_DIR, fname);
        if (fs.existsSync(fp)) {
            try { fs.unlinkSync(fp); } catch (_) { /* ignore */ }
        }
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
function buildAppBundle() {
    const r = buildAppChunks();
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
    BUNDLE_SOURCES,
    CHUNK_GROUPS,
    OBSOLETE_OUTPUTS,
    JS_SRC_DIR,
    I18N_SRC_DIR,
    WWW_JS_DIR
};
