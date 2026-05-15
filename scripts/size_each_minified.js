const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { minifyJS } = require('./minify-js');

const ROOT = path.join(__dirname, '..');
const targets = [
    'web-src/js/utils.js',
    'web-src/js/ui-components.js',
    'web-src/js/request-governor.js',
    'web-src/js/notification.js',
    'web-src/js/page-loader.js',
    'web-src/js/module-loader.js',
    'web-src/i18n/i18n-engine.js',
    'web-src/i18n/i18n-zh-CN.js',
    'web-src/js/state.js',
    'web-src/js/state-theme.js',
    'web-src/js/state-session.js',
    'web-src/js/state-sse.js',
    'web-src/js/state-ui.js',
    'web-src/js/main.js'
];

console.log('file | raw | minified | gz(level9)');
console.log('-----|-----|----------|-----------');
let totalMin = 0, totalGz = 0;
for (const rel of targets) {
    const fp = path.join(ROOT, rel);
    const raw = fs.readFileSync(fp, 'utf8').replace(/^\uFEFF/, '');
    const min = minifyJS(raw);
    const gz = zlib.gzipSync(Buffer.from(min), { level: 9 });
    totalMin += min.length;
    totalGz += gz.length;
    console.log(`${path.basename(rel).padEnd(28)} ${String(raw.length).padStart(7)} | ${String(min.length).padStart(7)} | ${String(gz.length).padStart(7)}`);
}
console.log(`${'TOTAL'.padEnd(28)} ${''.padStart(7)} | ${String(totalMin).padStart(7)} | ${String(totalGz).padStart(7)}`);
