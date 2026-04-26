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
const { buildAppBundle, BUNDLE_SOURCES: APP_BUNDLE_SOURCES } = require('./build-app-bundle');
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

// Sub-directories under runtime/ that contain split sub-modules
const RUNTIME_SUB_DIRS = ['protocol', 'device-control'];

function syncDirectRuntimeModules() {
    const results = [];
    const noMinify = process.argv.includes('--no-minify');

    // 1. Sync top-level entry files
    DIRECT_RUNTIME_MODULES.forEach((fileName) => {
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
                    // Apply CSS minify for non-.min.css files
                    const noMinify = process.argv.includes('--no-minify');
                    if (!noMinify && entry.endsWith('.css') && !entry.endsWith('.min.css')) {
                        const raw = readUtf8(srcFile);
                        const minified = minifyCSS(raw);
                        writeUtf8(dstFile, minified);
                        const size = Buffer.byteLength(minified, 'utf8');
                        results.push({ src: relPath, dest: relPath, size, cssMinified: true });
                    } else {
                        fs.copyFileSync(srcFile, dstFile);
                        const size = fs.statSync(dstFile).size;
                        results.push({ src: relPath, dest: relPath, size });
                    }
                }
            });
        };
        syncDirRecursive(srcDir, destDir, dirName);
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
    const appBundle = buildAppBundle();

    validateSourceCoverage();

    return {
        staticAssets,
        syncedModules,
        i18nModules,
        adminBundle,
        appBundle
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
