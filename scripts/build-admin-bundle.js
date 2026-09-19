/**
 * Build publish-time admin modules from source files outside data/www.
 *
 * Source modules live under web-src/modules/admin so we can keep publish
 * assets lean while still editing smaller focused files in development.
 * Individual module files are emitted for runtime loading, while
 * admin-bundle.js is kept as a tiny compatibility stub.
 *
 * Usage:
 *   node scripts/build-admin-bundle.js
 */

const fs = require('fs');
const path = require('path');
const { minifyJS } = require('./minify-js');
const { resolveWebBuildContext, shouldIncludeAsset } = require('./web-profile');
const { withWebPublishLock } = require('./web-publish-lock');

const ROOT_DIR = path.join(__dirname, '..');
const SOURCE_DIR = path.join(ROOT_DIR, 'web-src', 'modules', 'admin');
const PUBLISH_MODULE_DIR = path.join(ROOT_DIR, 'data', 'www', 'js', 'modules');
const OUTPUT_FILE = path.join(PUBLISH_MODULE_DIR, 'admin-bundle.js');
// Unified full profile: keep only existing source files
const UNIFIED_SOURCE_FILES = [
    'logs.js',
    'users.js',
    'rule-script.js'
];

function getSourceFiles(context = resolveWebBuildContext()) {
    return UNIFIED_SOURCE_FILES.filter((fileName) =>
        shouldIncludeAsset(`js/modules/${fileName}`, context));
}

function readSourceFile(fileName) {
    const filePath = path.join(SOURCE_DIR, fileName);
    if (!fs.existsSync(filePath)) {
        throw new Error(`Missing admin module source: ${filePath}`);
    }

    return fs.readFileSync(filePath, 'utf8').replace(/^\uFEFF/, '').trim();
}

function buildAdminBundle(context = resolveWebBuildContext()) {
    const noMinify = process.argv.includes('--no-minify');
    const modules = [];
    fs.mkdirSync(PUBLISH_MODULE_DIR, { recursive: true });

    const sourceFiles = getSourceFiles(context);
    sourceFiles.forEach((fileName) => {
        const publishFile = path.join(PUBLISH_MODULE_DIR, fileName);
        const raw = readSourceFile(fileName);
        const content = noMinify ? raw : minifyJS(raw);
        fs.writeFileSync(publishFile, content, 'utf8');
        modules.push({
            fileName,
            publishFile,
            size: Buffer.byteLength(content, 'utf8')
        });
    });

    const compatContent = [
        '/**',
        ' * Compatibility stub kept intentionally small.',
        ' * Admin pages now load individual modules to avoid large one-shot downloads on ESP32.',
        ' */',
        '(function(){window.__fastbeeAdminBundleCompat=true;})();',
        ''
    ].join('\n');
    fs.writeFileSync(OUTPUT_FILE, compatContent, 'utf8');

    return {
        outputFile: OUTPUT_FILE,
        sourceDir: SOURCE_DIR,
        fileCount: sourceFiles.length,
        size: Buffer.byteLength(compatContent, 'utf8'),
        modules
    };
}

if (require.main === module) {
    const result = withWebPublishLock(() => buildAdminBundle(), { label: 'build-admin-bundle' });
    console.log(`Built admin publish assets from ${result.fileCount} source files`);
    console.log(`Source: ${result.sourceDir}`);
    console.log(`Compat output: ${result.outputFile}`);
    console.log(`Compat size: ${result.size} bytes`);
    console.log(`Generated ${result.modules.length} individual admin module files`);
}

module.exports = {
    buildAdminBundle,
    SOURCE_DIR,
    PUBLISH_MODULE_DIR,
    OUTPUT_FILE,
    SOURCE_FILES: UNIFIED_SOURCE_FILES,
    UNIFIED_SOURCE_FILES,
    getSourceFiles
};
