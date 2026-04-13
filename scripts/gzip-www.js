/**
 * Gzip static web assets under data/www.
 *
 * Steps:
 * 1. Remove old .gz files.
 * 2. Compress all .html/.js/.css files into fresh .gz files.
 *
 * Usage:
 *   node scripts/gzip-www.js
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { buildWebModules } = require('./build-web-modules');

const WWW_DIR = path.join(__dirname, '..', 'data', 'www');
const COMPRESS_EXTENSIONS = new Set(['.html', '.js', '.css']);

const stats = {
    deleted: 0,
    skippedDeletes: 0,
    compressed: 0,
    failedCompress: 0,
    totalOriginalSize: 0,
    totalCompressedSize: 0
};

function walkDir(dir, callback) {
    if (!fs.existsSync(dir)) {
        console.error(`Directory not found: ${dir}`);
        return;
    }

    const files = fs.readdirSync(dir);
    files.forEach((file) => {
        const filePath = path.join(dir, file);
        const stat = fs.statSync(filePath);

        if (stat.isDirectory()) {
            walkDir(filePath, callback);
            return;
        }

        callback(filePath);
    });
}

function deleteOldGzFiles() {
    console.log('\n[Step 1] Removing old .gz files...');

    walkDir(WWW_DIR, (filePath) => {
        if (!filePath.endsWith('.gz')) return;
        try {
            fs.unlinkSync(filePath);
            stats.deleted += 1;
            console.log(`  Deleted: ${path.relative(WWW_DIR, filePath)}`);
        } catch (error) {
            stats.skippedDeletes += 1;
            console.warn(`  Skip delete: ${path.relative(WWW_DIR, filePath)} (${error.code || error.message})`);
        }
    });

    console.log(`  Removed ${stats.deleted} old .gz files`);
    if (stats.skippedDeletes > 0) {
        console.log(`  Skipped ${stats.skippedDeletes} locked .gz files`);
    }
}

function compressFile(filePath) {
    const ext = path.extname(filePath).toLowerCase();
    if (!COMPRESS_EXTENSIONS.has(ext)) return;

    const content = fs.readFileSync(filePath);
    const compressed = zlib.gzipSync(content, { level: 9 });
    const gzPath = `${filePath}.gz`;

    try {
        fs.writeFileSync(gzPath, compressed);
    } catch (error) {
        stats.failedCompress += 1;
        console.warn(`  Skip compress: ${path.relative(WWW_DIR, filePath)} (${error.code || error.message})`);
        return;
    }

    const originalSize = content.length;
    const compressedSize = compressed.length;
    const ratio = originalSize > 0
        ? ((1 - compressedSize / originalSize) * 100).toFixed(1)
        : '0.0';

    stats.compressed += 1;
    stats.totalOriginalSize += originalSize;
    stats.totalCompressedSize += compressedSize;

    console.log(`  Compressed: ${path.relative(WWW_DIR, filePath)} (${originalSize} -> ${compressedSize}, -${ratio}%)`);
}

function compressAllFiles() {
    console.log('\n[Step 2] Compressing HTML/JS/CSS files...');

    walkDir(WWW_DIR, compressFile);

    const totalRatio = stats.totalOriginalSize > 0
        ? ((1 - stats.totalCompressedSize / stats.totalOriginalSize) * 100).toFixed(1)
        : '0.0';

    console.log(`\n  Total compressed files: ${stats.compressed}`);
    if (stats.failedCompress > 0) {
        console.log(`  Failed compressions: ${stats.failedCompress}`);
    }
    console.log(`  Total size: ${stats.totalOriginalSize} -> ${stats.totalCompressedSize} bytes (-${totalRatio}%)`);
}

function main() {
    console.log('========================================');
    console.log('  ESP32 Web Asset Gzip Tool');
    console.log('========================================');
    console.log(`Target directory: ${WWW_DIR}`);

    console.log('\n[Step 0] Building web modules...');
    const buildResult = buildWebModules();
    buildResult.syncedModules.forEach((item) => {
        console.log(`  Synced: ${path.relative(ROOT_DIR(), item.publishFile)} (${item.size} bytes)`);
    });
    console.log(`  Built: ${path.relative(ROOT_DIR(), buildResult.adminBundle.outputFile)} (${buildResult.adminBundle.size} bytes)`);

    deleteOldGzFiles();
    compressAllFiles();

    console.log('\n========================================');
    console.log('  Compression completed');
    console.log('========================================\n');
}

main();

function ROOT_DIR() {
    return path.join(__dirname, '..');
}
