'use strict';

const fs = require('fs');
const path = require('path');
const { gzipWebAsset } = require('./web-gzip');
const { buildWebModules } = require('./build-web-modules');
const { generateManifest } = require('./generate-sw-manifest');
const { createWebAssetReport, printWebAssetReport } = require('./web-asset-report');
const { runWebQualityGate } = require('./web-quality-gate');
const { withWebPublishLock } = require('./web-publish-lock');

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

function findUncompressedPublishAssets() {
    const files = [];
    walkDir(WWW_DIR, (filePath) => {
        if (filePath.endsWith('.gz')) return;
        if (!COMPRESS_EXTENSIONS.has(path.extname(filePath).toLowerCase())) return;
        files.push(relFromWww(filePath));
    });
    return files.sort();
}

function deleteOldGzipAssets() {
    const stats = {
        deleted: 0,
        skipped: 0
    };
    walkDir(WWW_DIR, (filePath) => {
        if (!filePath.endsWith('.gz')) return;
        try {
            fs.unlinkSync(filePath);
            stats.deleted += 1;
        } catch (error) {
            stats.skipped += 1;
            console.warn(`  stale gzip kept: ${relFromWww(filePath)} (${error.code || error.message})`);
        }
    });
    return stats;
}

async function gzipWebAssets() {
    const stats = {
        files: 0,
        failed: 0,
        raw: 0,
        gzip: 0
    };

    const files = [];
    walkDir(WWW_DIR, (filePath) => {
        if (filePath.endsWith('.gz')) return;
        if (!COMPRESS_EXTENSIONS.has(path.extname(filePath).toLowerCase())) return;
        files.push(filePath);
    });

    for (const filePath of files) {
        const raw = fs.readFileSync(filePath);
        const compressed = await gzipWebAsset(raw);
        try {
            fs.writeFileSync(`${filePath}.gz`, compressed);
        } catch (error) {
            stats.failed += 1;
            console.warn(`  gzip skipped: ${relFromWww(filePath)} (${error.code || error.message})`);
            continue;
        }

        stats.files += 1;
        stats.raw += raw.length;
        stats.gzip += compressed.length;
    }

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

async function runBuildUnlocked() {
    console.log('Web publish build');
    console.log('=================');
    const buildResult = buildWebModules();

    console.log('\nGenerating service-worker manifest...');
    const manifest = generateManifest();

    console.log('\nRemoving stale gzip assets...');
    const staleGzipStats = deleteOldGzipAssets();
    console.log(`  removed ${staleGzipStats.deleted} stale gzip file(s)`);
    if (staleGzipStats.skipped > 0) {
        throw new Error(`Failed to remove ${staleGzipStats.skipped} stale gzip artifact(s)`);
    }

    console.log('\nRefreshing gzip assets...');
    const gzipStats = await gzipWebAssets();
    const ratio = gzipStats.raw > 0
        ? ((1 - gzipStats.gzip / gzipStats.raw) * 100).toFixed(1)
        : '0.0';
    console.log(`  gzipped ${gzipStats.files} file(s): ${gzipStats.raw} -> ${gzipStats.gzip} bytes (-${ratio}%)`);
    if (gzipStats.failed > 0) {
        throw new Error(`Failed to create ${gzipStats.failed} gzip publish artifact(s)`);
    }

    console.log('\nRemoving uncompressed publish copies...');
    const cleanupStats = deleteCompressedOriginals();
    console.log(`  removed ${cleanupStats.deleted} original file(s)`);
    if (cleanupStats.skipped > 0) {
        throw new Error(`Failed to remove ${cleanupStats.skipped} uncompressed publish artifact(s)`);
    }

    const rawPublishAssets = findUncompressedPublishAssets();
    if (rawPublishAssets.length > 0) {
        throw new Error(`data/www contains uncompressed HTML/JS/CSS: ${rawPublishAssets.join(', ')}`);
    }

    const report = createWebAssetReport();
    printWebAssetReport(report);
    runWebQualityGate({ report, context: buildResult.context });

    return { buildResult, manifest, staleGzipStats, gzipStats, cleanupStats, report };
}

function runBuild() {
    return withWebPublishLock(runBuildUnlocked, { label: 'build-web-assets' });
}

if (require.main === module) {
    Promise.resolve().then(runBuild).catch((error) => {
        console.error(error);
        process.exitCode = 1;
    });
}

module.exports = {
    runBuild,
    deleteOldGzipAssets,
    gzipWebAssets,
    deleteCompressedOriginals,
    findUncompressedPublishAssets
};
