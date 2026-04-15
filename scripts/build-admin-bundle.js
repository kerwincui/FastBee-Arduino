/**
 * Build the runtime admin bundle from source modules outside data/www.
 *
 * Source modules live under web-src/modules/admin so we can keep publish
 * assets lean while still editing smaller focused files in development.
 *
 * Usage:
 *   node scripts/build-admin-bundle.js
 */

const fs = require('fs');
const path = require('path');
const { minifyJS } = require('./minify-js');

const ROOT_DIR = path.join(__dirname, '..');
const SOURCE_DIR = path.join(ROOT_DIR, 'web-src', 'modules', 'admin');
const OUTPUT_FILE = path.join(ROOT_DIR, 'data', 'www', 'js', 'modules', 'admin-bundle.js');
const SOURCE_FILES = [
    'users.js',
    'roles.js',
    'logs.js',
    'files.js',
    'rule-script.js'
];

function readSourceFile(fileName) {
    const filePath = path.join(SOURCE_DIR, fileName);
    if (!fs.existsSync(filePath)) {
        throw new Error(`Missing admin module source: ${filePath}`);
    }

    return fs.readFileSync(filePath, 'utf8').replace(/^\uFEFF/, '').trim();
}

function buildAdminBundle() {
    const header = [
        '/**',
        ' * AUTO-GENERATED FILE. DO NOT EDIT DIRECTLY.',
        ' * Source files: web-src/modules/admin/*.js',
        ' * Build command: node scripts/build-admin-bundle.js',
        ' */',
        ''
    ].join('\n');

    const sections = SOURCE_FILES.map((fileName) => {
        return [
            `/* ===== ${fileName} ===== */`,
            readSourceFile(fileName)
        ].join('\n');
    });

    const output = `${header}${sections.join('\n\n')}\n`;
    const noMinify = process.argv.includes('--no-minify');
    const finalContent = noMinify ? output : minifyJS(output);
    fs.writeFileSync(OUTPUT_FILE, finalContent, 'utf8');

    return {
        outputFile: OUTPUT_FILE,
        sourceDir: SOURCE_DIR,
        fileCount: SOURCE_FILES.length,
        size: Buffer.byteLength(finalContent, 'utf8')
    };
}

if (require.main === module) {
    const result = buildAdminBundle();
    console.log(`Built admin bundle from ${result.fileCount} source files`);
    console.log(`Source: ${result.sourceDir}`);
    console.log(`Output: ${result.outputFile}`);
    console.log(`Size: ${result.size} bytes`);
}

module.exports = {
    buildAdminBundle,
    SOURCE_DIR,
    OUTPUT_FILE,
    SOURCE_FILES
};
