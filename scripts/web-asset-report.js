'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT_DIR = path.join(__dirname, '..');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');
const TRACKED_EXTENSIONS = new Set(['.html', '.js', '.css', '.json', '.png', '.ico']);
const DEFAULT_TOP_N = 18;

const BOOT_URLS = [
    'index.html',
    'css/main.css',
    'js/chunk-1-core-a.js',
    'js/chunk-2-core-b.js',
    'js/chunk-3-i18n-engine.js',
    'js/chunk-8-state-1.js',
    'js/chunk-9-state-2.js',
    'assets/logo.png'
];

const PAGE_BUNDLES = [
    'js/modules/protocol.js',
    'js/modules/protocol-full-config.js',
    'js/modules/protocol-modbus-rtu.js',
    'js/modules/protocol-modbus-control.js',
    'js/modules/periph-exec.js',
    'js/modules/device-control.js',
    'js/modules/device-control-view.js',
    'js/modules/device-control-modbus.js',
    'js/modules/device-config.js',
    'js/modules/peripherals.js',
    'js/modules/network.js'
];
const OBSOLETE_PREFIXES = [
    'js/modules/device-control/',
    'js/modules/protocol/'
];

function normalizeRel(filePath) {
    return path.relative(WWW_DIR, filePath).replace(/\\/g, '/');
}

function walkDir(dir, files) {
    if (!fs.existsSync(dir)) return;
    fs.readdirSync(dir).forEach((entry) => {
        const fullPath = path.join(dir, entry);
        const stat = fs.statSync(fullPath);
        if (stat.isDirectory()) {
            walkDir(fullPath, files);
        } else if (stat.isFile()) {
            files.push(fullPath);
        }
    });
}

function sizeOf(filePath) {
    return fs.existsSync(filePath) ? fs.statSync(filePath).size : 0;
}

function rawSizeFromGzip(gzPath) {
    try {
        return zlib.gunzipSync(fs.readFileSync(gzPath)).length;
    } catch (error) {
        return 0;
    }
}

function fileRecordForRel(rel) {
    const filePath = path.join(WWW_DIR, rel);
    const gzPath = `${filePath}.gz`;
    const hasRaw = fs.existsSync(filePath);
    const hasGzip = fs.existsSync(gzPath);
    return {
        path: rel,
        ext: path.extname(rel).toLowerCase() || '(none)',
        raw: hasRaw ? sizeOf(filePath) : rawSizeFromGzip(gzPath),
        gzip: hasGzip ? sizeOf(gzPath) : 0,
        hasGzip
    };
}

function shouldSkipRel(rel) {
    return OBSOLETE_PREFIXES.some((prefix) => rel.startsWith(prefix));
}

function createWebAssetReport(options = {}) {
    const files = [];
    walkDir(WWW_DIR, files);

    const relAssets = new Set();
    files.forEach((filePath) => {
        var rel = normalizeRel(filePath);
        if (rel.endsWith('.gz')) rel = rel.slice(0, -3);
        if (shouldSkipRel(rel)) return;
        if (TRACKED_EXTENSIONS.has(path.extname(rel).toLowerCase())) relAssets.add(rel);
    });

    const assets = Array.from(relAssets)
        .map(fileRecordForRel)
        .sort((a, b) => a.path.localeCompare(b.path));

    const totals = assets.reduce((acc, item) => {
        acc.raw += item.raw;
        acc.gzip += item.gzip || item.raw;
        acc.files += 1;
        if (!item.hasGzip && ['.html', '.js', '.css'].includes(item.ext)) acc.missingGzip += 1;
        if (!acc.byExt[item.ext]) acc.byExt[item.ext] = { files: 0, raw: 0, gzip: 0 };
        acc.byExt[item.ext].files += 1;
        acc.byExt[item.ext].raw += item.raw;
        acc.byExt[item.ext].gzip += item.gzip || item.raw;
        return acc;
    }, { files: 0, raw: 0, gzip: 0, missingGzip: 0, byExt: {} });

    const byPath = Object.fromEntries(assets.map((item) => [item.path, item]));
    const bootAssets = BOOT_URLS
        .map((rel) => byPath[rel])
        .filter(Boolean);
    const bootGzip = bootAssets.reduce((sum, item) => sum + (item.gzip || item.raw), 0);
    const bootRaw = bootAssets.reduce((sum, item) => sum + item.raw, 0);

    const pageBundles = PAGE_BUNDLES
        .map((rel) => byPath[rel])
        .filter(Boolean)
        .map((item) => ({
            path: item.path,
            raw: item.raw,
            gzip: item.gzip || item.raw,
            hasGzip: item.hasGzip
        }));

    const topN = Number(options.topN || DEFAULT_TOP_N);
    const largest = assets
        .slice()
        .sort((a, b) => (b.gzip || b.raw) - (a.gzip || a.raw))
        .slice(0, topN);

    return {
        generatedAt: new Date().toISOString(),
        wwwDir: WWW_DIR,
        totals,
        boot: {
            raw: bootRaw,
            gzip: bootGzip,
            files: bootAssets.map((item) => item.path)
        },
        pageBundles,
        largest
    };
}

function createSourceAssetReport() {
    const sourceTargets = [
        path.join(ROOT_DIR, 'web-src', 'js'),
        path.join(ROOT_DIR, 'web-src', 'i18n'),
        path.join(ROOT_DIR, 'web-src', 'modules'),
        path.join(ROOT_DIR, 'web-src', 'pages'),
        path.join(ROOT_DIR, 'web-src', 'css')
    ];
    const files = [];
    sourceTargets.forEach((dir) => walkDir(dir, files));
    const assets = files
        .filter((filePath) => ['.html', '.js', '.css', '.json'].includes(path.extname(filePath).toLowerCase()))
        .map((filePath) => ({
            path: path.relative(ROOT_DIR, filePath).replace(/\\/g, '/'),
            ext: path.extname(filePath).toLowerCase(),
            raw: sizeOf(filePath)
        }))
        .sort((a, b) => a.path.localeCompare(b.path));
    const totals = assets.reduce((acc, item) => {
        acc.raw += item.raw;
        acc.files += 1;
        if (!acc.byExt[item.ext]) acc.byExt[item.ext] = { files: 0, raw: 0 };
        acc.byExt[item.ext].files += 1;
        acc.byExt[item.ext].raw += item.raw;
        return acc;
    }, { files: 0, raw: 0, byExt: {} });
    return { generatedAt: new Date().toISOString(), totals, assets };
}

function formatBytes(value) {
    if (value >= 1024 * 1024) return (value / 1024 / 1024).toFixed(2) + ' MB';
    if (value >= 1024) return (value / 1024).toFixed(1) + ' KB';
    return String(value) + ' B';
}

function printTable(rows, columns) {
    const widths = columns.map((column) => {
        return Math.max(column.label.length, ...rows.map((row) => String(column.value(row)).length));
    });
    console.log(columns.map((column, index) => column.label.padEnd(widths[index])).join('  '));
    console.log(widths.map((width) => '-'.repeat(width)).join('  '));
    rows.forEach((row) => {
        console.log(columns.map((column, index) => String(column.value(row)).padEnd(widths[index])).join('  '));
    });
}

function printWebAssetReport(report) {
    console.log('\nWeb asset report');
    console.log('================');
    console.log(`Files: ${report.totals.files}`);
    console.log(`Total raw: ${formatBytes(report.totals.raw)}`);
    console.log(`Total gzip/effective: ${formatBytes(report.totals.gzip)}`);
    console.log(`Boot effective: ${formatBytes(report.boot.gzip)} (${report.boot.files.length} files)`);
    if (report.totals.missingGzip > 0) {
        console.log(`Missing gzip: ${report.totals.missingGzip} compressible file(s)`);
    }

    console.log('\nBy extension');
    const byExtRows = Object.keys(report.totals.byExt).sort().map((ext) => {
        const item = report.totals.byExt[ext];
        return { ext, files: item.files, raw: item.raw, gzip: item.gzip };
    });
    printTable(byExtRows, [
        { label: 'Ext', value: (r) => r.ext },
        { label: 'Files', value: (r) => r.files },
        { label: 'Raw', value: (r) => formatBytes(r.raw) },
        { label: 'Effective', value: (r) => formatBytes(r.gzip) }
    ]);

    if (report.pageBundles.length > 0) {
        console.log('\nRuntime bundles');
        printTable(report.pageBundles, [
            { label: 'File', value: (r) => r.path },
            { label: 'Raw', value: (r) => formatBytes(r.raw) },
            { label: 'Gzip', value: (r) => formatBytes(r.gzip) }
        ]);
    }

    console.log('\nLargest effective assets');
    printTable(report.largest, [
        { label: 'File', value: (r) => r.path },
        { label: 'Raw', value: (r) => formatBytes(r.raw) },
        { label: 'Effective', value: (r) => formatBytes(r.gzip || r.raw) }
    ]);
}

function printSourceAssetReport(report, topN = DEFAULT_TOP_N) {
    console.log('\nWeb source asset report');
    console.log('=======================');
    console.log(`Files: ${report.totals.files}`);
    console.log(`Total raw: ${formatBytes(report.totals.raw)}`);

    console.log('\nBy extension');
    const byExtRows = Object.keys(report.totals.byExt).sort().map((ext) => {
        const item = report.totals.byExt[ext];
        return { ext, files: item.files, raw: item.raw };
    });
    printTable(byExtRows, [
        { label: 'Ext', value: (r) => r.ext },
        { label: 'Files', value: (r) => r.files },
        { label: 'Raw', value: (r) => formatBytes(r.raw) }
    ]);

    console.log('\nLargest source assets');
    const largest = report.assets
        .slice()
        .sort((a, b) => b.raw - a.raw)
        .slice(0, topN);
    printTable(largest, [
        { label: 'File', value: (r) => r.path },
        { label: 'Raw', value: (r) => formatBytes(r.raw) }
    ]);
}

function parseArgs(argv) {
    const options = {
        source: false,
        json: false,
        topN: DEFAULT_TOP_N
    };
    argv.forEach((arg, index) => {
        if (arg === '--source') options.source = true;
        if (arg === '--json') options.json = true;
        if (arg === '--top' && argv[index + 1]) options.topN = Number(argv[index + 1]);
        if (arg.startsWith('--top=')) options.topN = Number(arg.slice('--top='.length));
    });
    if (!Number.isFinite(options.topN) || options.topN <= 0) {
        options.topN = DEFAULT_TOP_N;
    }
    return options;
}

if (require.main === module) {
    const options = parseArgs(process.argv.slice(2));
    const report = options.source ? createSourceAssetReport() : createWebAssetReport(options);
    if (options.json) {
        console.log(JSON.stringify(report, null, 2));
    } else if (options.source) {
        printSourceAssetReport(report, options.topN);
    } else {
        printWebAssetReport(report);
    }
}

module.exports = {
    createWebAssetReport,
    createSourceAssetReport,
    printWebAssetReport,
    printSourceAssetReport,
    formatBytes
};
