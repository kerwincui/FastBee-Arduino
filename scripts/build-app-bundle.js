/**
 * Build the core app bundle from individual JS source files.
 *
 * Concatenates all core JS files (loaded by index.html as <script defer>)
 * into a single app-bundle.js, reducing browser concurrent HTTP requests
 * from 14 down to 1 — critical for ESP32-C3 devices with limited TCP connections.
 *
 * i18n-en.js is NOT included in the bundle (89KB, loaded on-demand only).
 *
 * Usage:
 *   node scripts/build-app-bundle.js
 *   node scripts/build-app-bundle.js --no-minify
 */

const fs = require('fs');
const path = require('path');
const { minifyJS } = require('./minify-js');

const ROOT_DIR = path.join(__dirname, '..');
const JS_SRC_DIR = path.join(ROOT_DIR, 'web-src', 'js');
const I18N_SRC_DIR = path.join(ROOT_DIR, 'web-src', 'i18n');
const WWW_JS_DIR = path.join(ROOT_DIR, 'data', 'www', 'js');
const OUTPUT_FILE = path.join(WWW_JS_DIR, 'app-bundle.js');

// Source files in dependency order (must match index.html defer order)
const BUNDLE_SOURCES = [
    // Base utilities (no dependencies)
    { dir: JS_SRC_DIR, file: 'utils.js' },
    { dir: JS_SRC_DIR, file: 'ui-components.js' },
    // Independent modules
    { dir: JS_SRC_DIR, file: 'request-governor.js' },
    { dir: JS_SRC_DIR, file: 'notification.js' },
    // Page & module loaders
    { dir: JS_SRC_DIR, file: 'page-loader.js' },
    { dir: JS_SRC_DIR, file: 'module-loader.js' },
    // i18n core (i18n-engine must load before i18n-zh-CN)
    { dir: I18N_SRC_DIR, file: 'i18n-engine.js' },
    { dir: I18N_SRC_DIR, file: 'i18n-zh-CN.js' },
    // Core state (depends on all above)
    { dir: JS_SRC_DIR, file: 'state.js' },
    // AppState sub-modules (extend AppState object)
    { dir: JS_SRC_DIR, file: 'state-theme.js' },
    { dir: JS_SRC_DIR, file: 'state-session.js' },
    { dir: JS_SRC_DIR, file: 'state-sse.js' },
    { dir: JS_SRC_DIR, file: 'state-ui.js' },
    // Main entry (depends on all above)
    { dir: JS_SRC_DIR, file: 'main.js' }
];

function readUtf8(filePath) {
    return fs.readFileSync(filePath, 'utf8').replace(/^\uFEFF/, '');
}

function buildAppBundle() {
    // Ensure output directory exists
    if (!fs.existsSync(WWW_JS_DIR)) {
        fs.mkdirSync(WWW_JS_DIR, { recursive: true });
    }

    const header = [
        '/**',
        ' * AUTO-GENERATED FILE. DO NOT EDIT DIRECTLY.',
        ' * Source: web-src/js/*.js + web-src/i18n/i18n-engine.js + i18n-zh-CN.js',
        ' * Build: node scripts/build-app-bundle.js',
        ` * Generated: ${new Date().toISOString()}`,
        ' */',
        ''
    ].join('\n');

    const sections = BUNDLE_SOURCES.map(({ dir, file }) => {
        const filePath = path.join(dir, file);
        if (!fs.existsSync(filePath)) {
            throw new Error(`Missing source file: ${filePath}`);
        }
        const content = readUtf8(filePath).trim();
        return [
            `/* ===== ${file} ===== */`,
            content
        ].join('\n');
    });

    const output = `${header}${sections.join('\n\n')}\n`;

    const noMinify = process.argv.includes('--no-minify');
    const finalContent = noMinify ? output : minifyJS(output);

    fs.writeFileSync(OUTPUT_FILE, finalContent, 'utf8');

    return {
        outputFile: OUTPUT_FILE,
        fileCount: BUNDLE_SOURCES.length,
        size: Buffer.byteLength(finalContent, 'utf8'),
        minified: !noMinify,
        sources: BUNDLE_SOURCES.map(s => s.file)
    };
}

if (require.main === module) {
    const result = buildAppBundle();
    console.log(`Built app bundle from ${result.fileCount} source files`);
    console.log(`Output: ${path.relative(ROOT_DIR, result.outputFile)}`);
    console.log(`Size: ${result.size} bytes${result.minified ? ' (minified)' : ''}`);
    console.log(`Sources: ${result.sources.join(', ')}`);
}

module.exports = {
    buildAppBundle,
    BUNDLE_SOURCES,
    OUTPUT_FILE,
    JS_SRC_DIR,
    I18N_SRC_DIR,
    WWW_JS_DIR
};
