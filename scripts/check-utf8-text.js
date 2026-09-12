'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.join(__dirname, '..');
const DEFAULT_DIRS = [
    'README.md',
    'README.en.md',
    '.editorconfig',
    'include',
    'src',
    'scripts',
    'web-src',
    'skills'
];
const TEXT_EXTENSIONS = new Set([
    '.c',
    '.cpp',
    '.css',
    '.h',
    '.hpp',
    '.html',
    '.ini',
    '.js',
    '.json',
    '.md',
    '.mjs',
    '.py',
    '.ps1',
    '.sh',
    '.ts',
    '.tsx',
    '.txt',
    '.yaml',
    '.yml'
]);
// Windows PowerShell 5.1 needs a BOM to reliably decode non-ASCII UTF-8
// scripts. TypeScript sources do not, so keep the repository rule strict there.
const BOM_ALLOWED_EXTENSIONS = new Set(['.ps1']);
const SKIP_DIRS = new Set([
    '.git',
    '.pio',
    '.qoder',
    '.codex',
    'data',
    'lib',
    'node_modules'
]);

function shouldScanFile(filePath) {
    return TEXT_EXTENSIONS.has(path.extname(filePath).toLowerCase());
}

function walk(target, files) {
    if (!fs.existsSync(target)) return;
    const stat = fs.statSync(target);
    if (stat.isDirectory()) {
        const name = path.basename(target);
        if (SKIP_DIRS.has(name)) return;
        fs.readdirSync(target).forEach((entry) => walk(path.join(target, entry), files));
        return;
    }
    if (stat.isFile() && shouldScanFile(target)) files.push(target);
}

function checkFile(filePath) {
    const bytes = fs.readFileSync(filePath);
    const text = bytes.toString('utf8');
    const issues = [];
    if (bytes.length >= 3 && bytes[0] === 0xEF && bytes[1] === 0xBB && bytes[2] === 0xBF &&
        !BOM_ALLOWED_EXTENSIONS.has(path.extname(filePath).toLowerCase())) {
        issues.push('UTF-8 BOM');
    }
    if (text.indexOf('\uFFFD') !== -1) {
        issues.push('replacement character');
    }
    return issues;
}

function main() {
    const inputs = process.argv.slice(2);
    const targets = inputs.length > 0 ? inputs : DEFAULT_DIRS;
    const files = [];
    targets.forEach((rel) => walk(path.resolve(ROOT_DIR, rel), files));

    const failed = [];
    files.forEach((filePath) => {
        const issues = checkFile(filePath);
        if (issues.length > 0) {
            failed.push({ filePath, issues });
        }
    });

    if (failed.length > 0) {
        console.error('UTF-8 text check failed:');
        failed.forEach((item) => {
            console.error('  ' + path.relative(ROOT_DIR, item.filePath) + ': ' + item.issues.join(', '));
        });
        process.exit(1);
    }

    console.log('UTF-8 text check passed: ' + files.length + ' file(s)');
}

if (require.main === module) {
    main();
}

module.exports = { checkFile, shouldScanFile };
