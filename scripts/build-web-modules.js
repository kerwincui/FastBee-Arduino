/**
 * Build publish-time web assets from editable source files.
 *
 * Responsibilities:
 * 1. Sync static assets (HTML, CSS, JS) from web-src/ to data/www/.
 * 2. Sync direct runtime modules from web-src/modules/runtime to data/www/js/modules.
 * 3. Sync i18n modules from web-src/i18n to data/www/js/modules.
 * 4. Build admin-bundle.js from web-src/modules/admin source files.
 * 5. Validate that all output modules trace back to web-src/ sources.
 *
 * Usage:
 *   node scripts/build-web-modules.js
 */

const fs = require('fs');
const path = require('path');
const { buildAdminBundle, SOURCE_FILES: ADMIN_SOURCE_FILES } = require('./build-admin-bundle');
const { minifyJS } = require('./minify-js');

const ROOT_DIR = path.join(__dirname, '..');
const WEB_SRC_DIR = path.join(ROOT_DIR, 'web-src');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');
const RUNTIME_SOURCE_DIR = path.join(WEB_SRC_DIR, 'modules', 'runtime');
const I18N_SOURCE_DIR = path.join(WEB_SRC_DIR, 'i18n');
const PUBLISH_MODULE_DIR = path.join(WWW_DIR, 'js', 'modules');
const DIRECT_RUNTIME_MODULES = [
    'protocol.js',
    'periph-exec.js',
    'device-control.js',
    'peripherals.js',
    'dashboard.js',
    'device-config.js',
    'network.js'
];
const I18N_MODULES = [
    { file: 'i18n-engine.js', minify: true },
    { file: 'i18n-zh-CN.js', minify: false },
    { file: 'i18n-en.js', minify: false }
];

function readUtf8(filePath) {
    return fs.readFileSync(filePath, 'utf8').replace(/^\uFEFF/, '');
}

function writeUtf8(filePath, content) {
    fs.writeFileSync(filePath, content, 'utf8');
}

function syncDirectRuntimeModules() {
    const results = [];

    DIRECT_RUNTIME_MODULES.forEach((fileName) => {
        const sourceFile = path.join(RUNTIME_SOURCE_DIR, fileName);
        const publishFile = path.join(PUBLISH_MODULE_DIR, fileName);

        if (!fs.existsSync(sourceFile)) {
            throw new Error(`Missing runtime module source: ${sourceFile}`);
        }

        const raw = readUtf8(sourceFile);
        const noMinify = process.argv.includes('--no-minify');
        const content = noMinify ? raw : minifyJS(raw);
        writeUtf8(publishFile, content);

        results.push({
            fileName,
            sourceFile,
            publishFile,
            size: Buffer.byteLength(content, 'utf8')
        });
    });

    return results;
}

function syncI18nModules() {
    const results = [];

    if (!fs.existsSync(PUBLISH_MODULE_DIR)) {
        fs.mkdirSync(PUBLISH_MODULE_DIR, { recursive: true });
    }

    I18N_MODULES.forEach(({ file, minify: shouldMinify }) => {
        const sourceFile = path.join(I18N_SOURCE_DIR, file);
        const publishFile = path.join(PUBLISH_MODULE_DIR, file);

        if (!fs.existsSync(sourceFile)) {
            throw new Error(`Missing i18n module source: ${sourceFile}`);
        }

        const raw = readUtf8(sourceFile);
        const noMinify = process.argv.includes('--no-minify');
        const content = (shouldMinify && !noMinify) ? minifyJS(raw) : raw;
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

// Sync static assets (HTML pages, CSS, root-level JS) from web-src/ to data/www/.
// These files are copied as-is (no minification); gzip-www.js handles .gz generation.
function syncStaticAssets() {
    const results = [];

    // Mapping: source sub-path under web-src/ -> destination sub-path under data/www/
    const syncMappings = [
        // Root-level files
        { src: 'index.html', dest: 'index.html' },
        { src: 'setup.html', dest: 'setup.html' },
        { src: '404.html', dest: '404.html' },
        { src: 'sw.js', dest: 'sw.js' },
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

        fs.copyFileSync(sourceFile, destFile);
        const size = fs.statSync(destFile).size;
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

        fs.readdirSync(srcDir).forEach((file) => {
            const srcFile = path.join(srcDir, file);
            const dstFile = path.join(destDir, file);
            if (fs.statSync(srcFile).isFile()) {
                fs.copyFileSync(srcFile, dstFile);
                const size = fs.statSync(dstFile).size;
                results.push({ src: `${dirName}/${file}`, dest: `${dirName}/${file}`, size });
            }
        });
    });

    // Sync root-level JS files (state.js, main.js) from web-src/js/ to data/www/js/
    const jsSrcDir = path.join(WEB_SRC_DIR, 'js');
    const jsDestDir = path.join(WWW_DIR, 'js');
    if (fs.existsSync(jsSrcDir)) {
        if (!fs.existsSync(jsDestDir)) {
            fs.mkdirSync(jsDestDir, { recursive: true });
        }
        fs.readdirSync(jsSrcDir).forEach((file) => {
            if (!file.endsWith('.js')) return;
            const srcFile = path.join(jsSrcDir, file);
            const dstFile = path.join(jsDestDir, file);
            if (fs.statSync(srcFile).isFile()) {
                fs.copyFileSync(srcFile, dstFile);
                const size = fs.statSync(dstFile).size;
                results.push({ src: `js/${file}`, dest: `js/${file}`, size });
            }
        });
    }

    return results;
}

// Quality gate: ensure every .js file in data/www/js/modules/ traces back to web-src/
function validateSourceCoverage() {
    if (!fs.existsSync(PUBLISH_MODULE_DIR)) return;

    // Build the set of expected output files
    const expectedFiles = new Set();
    DIRECT_RUNTIME_MODULES.forEach((f) => expectedFiles.add(f));
    I18N_MODULES.forEach(({ file }) => expectedFiles.add(file));
    expectedFiles.add('admin-bundle.js');
    ADMIN_SOURCE_FILES.forEach((f) => expectedFiles.add(f)); // admin stubs

    const actualFiles = fs.readdirSync(PUBLISH_MODULE_DIR)
        .filter((f) => f.endsWith('.js') && !f.endsWith('.gz'));

    const orphaned = actualFiles.filter((f) => !expectedFiles.has(f));
    if (orphaned.length > 0) {
        console.warn(`\n  WARNING: ${orphaned.length} orphaned file(s) in data/www/js/modules/:`);
        orphaned.forEach((f) => console.warn(`    - ${f} (not traced to web-src/)`));
        console.warn('  These files should be moved to web-src/ or removed.\n');
    } else {
        console.log(`  Source coverage: all ${actualFiles.length} output modules traced to web-src/`);
    }
}

function buildWebModules() {
    const staticAssets = syncStaticAssets();
    const syncedModules = syncDirectRuntimeModules();
    const i18nModules = syncI18nModules();
    const adminBundle = buildAdminBundle();

    validateSourceCoverage();

    return {
        staticAssets,
        syncedModules,
        i18nModules,
        adminBundle
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
}

module.exports = {
    buildWebModules,
    syncStaticAssets,
    syncDirectRuntimeModules,
    syncI18nModules,
    validateSourceCoverage,
    DIRECT_RUNTIME_MODULES,
    I18N_MODULES,
    RUNTIME_SOURCE_DIR,
    I18N_SOURCE_DIR,
    PUBLISH_MODULE_DIR,
    WEB_SRC_DIR,
    WWW_DIR
};
