/**
 * Build publish-time web modules from editable source files.
 *
 * Responsibilities:
 * 1. Sync direct runtime modules from web-src/modules/runtime to data/www/js/modules.
 * 2. Build admin-bundle.js from web-src/modules/admin source files.
 *
 * Usage:
 *   node scripts/build-web-modules.js
 */

const fs = require('fs');
const path = require('path');
const { buildAdminBundle } = require('./build-admin-bundle');
const { minifyJS } = require('./minify-js');

const ROOT_DIR = path.join(__dirname, '..');
const RUNTIME_SOURCE_DIR = path.join(ROOT_DIR, 'web-src', 'modules', 'runtime');
const PUBLISH_MODULE_DIR = path.join(ROOT_DIR, 'data', 'www', 'js', 'modules');
const DIRECT_RUNTIME_MODULES = [
    'protocol.js',
    'periph-exec.js',
    'device-control.js',
    'peripherals.js'
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

function buildWebModules() {
    const syncedModules = syncDirectRuntimeModules();
    const adminBundle = buildAdminBundle();

    return {
        syncedModules,
        adminBundle
    };
}

if (require.main === module) {
    const result = buildWebModules();
    console.log(`Synced ${result.syncedModules.length} direct runtime modules`);
    result.syncedModules.forEach((item) => {
        console.log(`  Synced: ${path.relative(ROOT_DIR, item.publishFile)} (${item.size} bytes)`);
    });
    console.log(`Built admin bundle: ${path.relative(ROOT_DIR, result.adminBundle.outputFile)} (${result.adminBundle.size} bytes)`);
}

module.exports = {
    buildWebModules,
    syncDirectRuntimeModules,
    DIRECT_RUNTIME_MODULES,
    RUNTIME_SOURCE_DIR,
    PUBLISH_MODULE_DIR
};
