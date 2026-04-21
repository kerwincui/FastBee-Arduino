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
const { spawn } = require('child_process');
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

function getActiveEnvFromPlatformioIni() {
    const iniPath = path.join(__dirname, '..', 'platformio.ini');
    if (!fs.existsSync(iniPath)) {
        return 'esp32dev';
    }
    const content = fs.readFileSync(iniPath, 'utf-8');
    const lines = content.split(/\r?\n/);
    let currentEnv = null;
    let activeEnv = null;
    for (const line of lines) {
        const trimmed = line.trim();
        const envMatch = trimmed.match(/^\[env:([^\]]+)\]$/);
        if (envMatch) {
            currentEnv = envMatch[1];
            continue;
        }
        if (currentEnv && !activeEnv) {
            if (/^upload_port\s*=/.test(trimmed)) {
                activeEnv = currentEnv;
                break;
            }
        }
    }
    return activeEnv || 'esp32dev';
}

function runPioCommand(args, label, allowFailure = false) {
    return new Promise((resolve, reject) => {
        const cmdStr = `pio ${args.join(' ')}`;
        console.log(`[${label}] 执行: ${cmdStr}`);
        const child = spawn('pio', args, { stdio: 'inherit', shell: true });
        child.on('close', (code, signal) => {
            if (code === 0 || allowFailure) {
                resolve();
            } else {
                reject(new Error(`${label} 失败 (退出码: ${code})`));
            }
        });
        child.on('error', (err) => {
            if (allowFailure) {
                resolve();
            } else {
                reject(err);
            }
        });
    });
}

async function main() {
    const args = process.argv.slice(2);
    const noUpload = args.includes('--no-upload');
    const noMonitor = args.includes('--no-monitor');

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
    console.log('========================================');

    if (noUpload) {
        console.log('\n提示: 已跳过上传和监视（--no-upload）');
        return;
    }

    const envName = getActiveEnvFromPlatformioIni();
    console.log(`\n[Step 3] Uploading filesystem (env: ${envName})...`);
    try {
        await runPioCommand(['run', '--target', 'uploadfs', '--environment', envName], 'UploadFS');
        console.log('\n[UploadFS] 文件系统上传成功');
    } catch (err) {
        console.error(`\n[UploadFS] 上传失败: ${err.message}`);
        console.log('提示: 若串口被占用，请先关闭串口监视器再重试');
        process.exit(1);
    }

    if (noMonitor) {
        console.log('\n提示: 已跳过串口监视器（--no-monitor）');
        return;
    }

    console.log('\n[Step 4] Opening serial monitor...');
    console.log('提示: 按 Ctrl+C 退出串口监视器\n');
    await runPioCommand(['device', 'monitor', '--environment', envName], 'Monitor', true);
}

main().catch(() => process.exit(1));

function ROOT_DIR() {
    return path.join(__dirname, '..');
}
