'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { buildWebModules } = require('./build-web-modules');
const { generateManifest } = require('./generate-sw-manifest');
const { createWebAssetReport, printWebAssetReport } = require('./web-asset-report');

const ROOT_DIR = path.join(__dirname, '..');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');
const COMPRESS_EXTENSIONS = new Set(['.html', '.js', '.css']);

function walkDir(dir, callback) {
    if (!fs.existsSync(dir)) return;
    fs.readdirSync(dir).forEach((entry) => {
        const fullPath = path.join(dir, entry);
        const stat = fs.statSync(fullPath);
        if (stat.isDirectory()) {
            walkDir(fullPath, callback);
        } else if (stat.isFile()) {
            callback(fullPath);
        }
    });
}

function relFromWww(filePath) {
    return path.relative(WWW_DIR, filePath).replace(/\\/g, '/');
}

function gzipWebAssets() {
    const stats = {
        files: 0,
        failed: 0,
        raw: 0,
        gzip: 0
    };

    walkDir(WWW_DIR, (filePath) => {
        if (filePath.endsWith('.gz')) return;
        if (!COMPRESS_EXTENSIONS.has(path.extname(filePath).toLowerCase())) return;

        const raw = fs.readFileSync(filePath);
        const compressed = zlib.gzipSync(raw, { level: 9 });
        try {
            fs.writeFileSync(`${filePath}.gz`, compressed);
        } catch (error) {
            stats.failed += 1;
            console.warn(`  gzip skipped: ${relFromWww(filePath)} (${error.code || error.message})`);
            return;
        }

        stats.files += 1;
        stats.raw += raw.length;
        stats.gzip += compressed.length;
    });

    return stats;
}

function deleteCompressedOriginals() {
    const stats = {
        deleted: 0,
        skipped: 0
    };

    walkDir(WWW_DIR, (filePath) => {
        if (filePath.endsWith('.gz')) return;
        if (!COMPRESS_EXTENSIONS.has(path.extname(filePath).toLowerCase())) return;
        if (!fs.existsSync(`${filePath}.gz`)) return;

        try {
            fs.unlinkSync(filePath);
            stats.deleted += 1;
        } catch (error) {
            stats.skipped += 1;
            console.warn(`  original kept: ${relFromWww(filePath)} (${error.code || error.message})`);
        }
    });

    return stats;
}

function runBuild() {
    console.log('Web publish build');
    console.log('=================');
    const buildResult = buildWebModules();

    console.log('\nGenerating service-worker manifest...');
    const manifest = generateManifest();

    console.log('\nRefreshing gzip assets...');
    const gzipStats = gzipWebAssets();
    const ratio = gzipStats.raw > 0
        ? ((1 - gzipStats.gzip / gzipStats.raw) * 100).toFixed(1)
        : '0.0';
    console.log(`  gzipped ${gzipStats.files} file(s): ${gzipStats.raw} -> ${gzipStats.gzip} bytes (-${ratio}%)`);
    if (gzipStats.failed > 0) {
        console.log(`  failed gzip writes: ${gzipStats.failed}`);
    }

    console.log('\nRemoving uncompressed publish copies...');
    const cleanupStats = deleteCompressedOriginals();
    console.log(`  removed ${cleanupStats.deleted} original file(s)`);
    if (cleanupStats.skipped > 0) {
        console.log(`  skipped ${cleanupStats.skipped} locked original file(s)`);
    }

    const report = createWebAssetReport();
    printWebAssetReport(report);

    return { buildResult, manifest, gzipStats, cleanupStats, report };
}

if (require.main === module) {
    try {
        runBuild();
    } catch (error) {
        console.error(error);
        process.exit(1);
    }
}

module.exports = {
    runBuild,
    gzipWebAssets,
    deleteCompressedOriginals
};
