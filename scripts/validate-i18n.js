'use strict';

// The unified web build keeps translations in the bundled HTML/JS modules.
// This guard catches stale references to removed locale files without making
// CI depend on generated data/www artifacts.
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const buildScript = fs.readFileSync(path.join(root, 'scripts', 'build-web-modules.js'), 'utf8') +
    fs.readFileSync(path.join(root, 'scripts', 'build-app-bundle.js'), 'utf8');
const index = fs.readFileSync(path.join(root, 'web-src', 'index.html'), 'utf8');
const fullscreen = fs.readFileSync(path.join(root, 'web-src', 'pages', 'fullscreen.html'), 'utf8');
const currentChunks = ['chunk-1-core.js', 'chunk-2-app.js', 'chunk-3-state.js'];
const missing = currentChunks.filter((file) => !buildScript.includes(file) || !index.includes(file));
if (missing.length > 0) {
    console.error(`i18n validation failed: missing current bundle references ${missing.join(', ')}`);
    process.exit(1);
}

const obsoleteChunks = [
    'chunk-1-core-a.js', 'chunk-2-core-b.js', 'chunk-3-i18n-engine.js',
    'chunk-4-i18n-zh-1.js', 'chunk-5-i18n-zh-2.js', 'chunk-6-i18n-zh-3.js',
    'chunk-7-i18n-zh-4.js', 'chunk-8-state-1.js', 'chunk-9-state-2.js'
];
const staleActiveReferences = obsoleteChunks.filter((file) => index.includes(file) || fullscreen.includes(file));
if (staleActiveReferences.length > 0) {
    console.error(`i18n validation failed: stale active chunk references ${staleActiveReferences.join(', ')}`);
    process.exit(1);
}

const i18nDir = path.join(root, 'web-src', 'i18n');
const sourceFiles = fs.existsSync(i18nDir)
    ? fs.readdirSync(i18nDir).filter((file) => /.js$/.test(file))
    : [];
if (sourceFiles.length > 0) {
    console.error(`i18n validation failed: locale sources reappeared: ${sourceFiles.join(', ')}`);
    process.exit(1);
}
if (/data-i18n|__fastbeeLoadZhChunks/.test(index) || /data-i18n/.test(fullscreen)) {
    console.error('i18n validation failed: i18n residue attributes remain in active pages');
    process.exit(1);
}
console.log('i18n validation passed: embedded=true, localeSources=0');
