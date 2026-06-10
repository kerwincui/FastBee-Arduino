/**
 * Gzip static web assets under data/www.
 *
 * Steps:
 * 1. Remove old .gz files.
 * 2. Compress all .html/.js/.css files into fresh .gz files.
 * 3. Delete uncompressed originals (only .gz + assets/ remain).
 *
 * Usage:
 *   node scripts/gzip-www.js
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { spawn } = require('child_process');
const { buildWebModules } = require('./build-web-modules');
const { generateManifest } = require('./generate-sw-manifest');
const {
    getWebProfile,
    isCompactWebProfile,
    isLiteWebProfile,
    isStandardWebProfile
} = require('./web-profile');

const DATA_DIR = path.join(__dirname, '..', 'data');
const WWW_DIR = path.join(DATA_DIR, 'www');
const STAGING_ROOT_DIR = path.join(ROOT_DIR(), '.pio', 'fs-staging');
const STAGING_RUN_ID = `run-${Date.now()}-${process.pid}`;
const STAGING_DATA_DIR = resolveStageDir();
const STAGING_WWW_DIR = path.join(STAGING_DATA_DIR, 'www');
const COMPRESS_EXTENSIONS = new Set(['.html', '.js', '.css']);
let activeWwwWhitelist = null;

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

function normalizeRelativePath(relPath) {
    return relPath.replace(/\\/g, '/').replace(/^\.\//, '');
}

function readArgValue(prefixes) {
    for (const arg of process.argv.slice(2)) {
        for (const prefix of prefixes) {
            if (arg.startsWith(prefix)) {
                return arg.slice(prefix.length);
            }
        }
    }
    return '';
}

function resolveStageDir() {
    const fromArg = readArgValue(['--stage-dir=', '--staging-dir=']);
    const fromEnv = process.env.FASTBEE_FS_STAGE_DIR || process.env.PLATFORMIO_DATA_DIR || '';
    const selected = fromArg || fromEnv;
    return selected
        ? path.resolve(selected)
        : path.join(STAGING_ROOT_DIR, STAGING_RUN_ID);
}

function shouldSkipProdDataPath(relPath) {
    if (!isCompactWebProfile()) return false;
    const normalized = normalizeRelativePath(relPath);
    return normalized === 'config/users.json'
        || normalized === 'config/auth.json'
        || normalized === 'config/roles.json'
        || normalized.startsWith('logs/');
}

function relativeFromWww(filePath) {
    return normalizeRelativePath(path.relative(WWW_DIR, filePath));
}

function ensureCleanDir(dir) {
    forceRemovePath(dir);
    fs.mkdirSync(dir, { recursive: true });
}

function forceRemovePath(targetPath) {
    if (!fs.existsSync(targetPath)) return;
    const stat = fs.lstatSync(targetPath);
    if (stat.isDirectory()) {
        fs.readdirSync(targetPath).forEach((entry) => {
            forceRemovePath(path.join(targetPath, entry));
        });
        try {
            fs.rmdirSync(targetPath);
        } catch (error) {
            fs.rmSync(targetPath, { recursive: true, force: true });
        }
        return;
    }
    try {
        fs.chmodSync(targetPath, 0o666);
    } catch (_) {
        // Best effort for Windows EPERM cleanup.
    }
    try {
        fs.unlinkSync(targetPath);
    } catch (error) {
        fs.rmSync(targetPath, { force: true });
    }
}

function isCompressiblePath(relPath) {
    return COMPRESS_EXTENSIONS.has(path.extname(relPath).toLowerCase());
}

function shouldTouchWwwFile(filePath) {
    if (!activeWwwWhitelist) return true;
    const relPath = relativeFromWww(filePath);
    const sourceRelPath = relPath.endsWith('.gz') ? relPath.slice(0, -3) : relPath;
    return activeWwwWhitelist.has(sourceRelPath);
}

function addWhitelistPath(files, relPath) {
    if (!relPath) return;
    files.add(normalizeRelativePath(relPath));
}

function addWhitelistAbsolute(files, absPath) {
    if (!absPath) return;
    addWhitelistPath(files, relativeFromWww(absPath));
}

function shouldKeepProfilePeripheral(item) {
    const id = String((item && item.id) || '');
    if (id === 'modbus_tcp') return false;
    if (isLiteWebProfile() && (id === 'modbus_rtu' || id === 'ethernet')) return false;
    if (id === 'stepper' || id === 'adc' || id === 'ws2812b' || id === 'uart_debug') return true;
    return item && item.enabled !== false;
}

function getProfilePeripheralIds() {
    const configPath = path.join(DATA_DIR, 'config', 'peripherals.json');
    if (!fs.existsSync(configPath)) return new Set();
    const doc = JSON.parse(fs.readFileSync(configPath, 'utf8'));
    const peripherals = Array.isArray(doc.peripherals) ? doc.peripherals : [];
    return new Set(
        peripherals
            .filter(shouldKeepProfilePeripheral)
            .map((item) => String(item.id || ''))
            .filter(Boolean)
    );
}

function actionTargetRequiresPeripheral(action) {
    if (!action || !action.targetPeriphId) return false;
    const targetId = String(action.targetPeriphId || '');
    if (!targetId || targetId.startsWith('modbus-task:') || targetId.startsWith('modbus:')) return false;

    const actionType = Number(action.actionType);
    const nonPeripheralTargetActions = new Set([
        6,  // system restart
        7,  // factory reset
        8,  // NTP sync
        9,  // OTA
        15, // command script; peripheral refs live in actionValue
        21, // trigger event
        22, // enable rule
        23  // disable rule
    ]);
    return !nonPeripheralTargetActions.has(actionType);
}

function getScriptPeripheralRefs(actionValue) {
    const refs = [];
    String(actionValue || '').split(/\r?\n/).forEach((line) => {
        const match = line.match(/^\s*PERIPH\s+([^\s#]+)/i);
        if (match && match[1]) refs.push(match[1]);
    });
    return refs;
}

function scriptPeripheralRefsAreKept(action, profilePeripheralIds) {
    if (Number(action && action.actionType) !== 15) return true;
    const refs = getScriptPeripheralRefs(action.actionValue);
    return refs.every((id) => profilePeripheralIds.has(String(id)));
}

function transformProdConfigFile(sourcePath, normalizedRel) {
    if (!isCompactWebProfile()) return null;

    if (normalizedRel === 'config/protocol.json') {
        const doc = JSON.parse(fs.readFileSync(sourcePath, 'utf8'));
        ['modbusTcp', 'http', 'coap', 'tcp'].forEach((key) => {
            if (Object.prototype.hasOwnProperty.call(doc, key)) delete doc[key];
        });
        if (isLiteWebProfile() && Object.prototype.hasOwnProperty.call(doc, 'modbusRtu')) {
            delete doc.modbusRtu;
        }
        if (doc.mqtt) {
            const keepTopic = (topic) => {
                const topicType = Number(topic && topic.topicType);
                const topicText = String((topic && topic.topic) || '').toLowerCase();
                return topicType !== 5 && topicType !== 6 && topicText.indexOf('upgrade') < 0;
            };
            if (Array.isArray(doc.mqtt.publishTopics)) {
                doc.mqtt.publishTopics = doc.mqtt.publishTopics.filter(keepTopic);
            }
            if (Array.isArray(doc.mqtt.subscribeTopics)) {
                doc.mqtt.subscribeTopics = doc.mqtt.subscribeTopics.filter(keepTopic);
            }
        }
        return JSON.stringify(doc, null, 2) + '\n';
    }

    if (normalizedRel === 'config/periph_exec.json') {
        const doc = JSON.parse(fs.readFileSync(sourcePath, 'utf8'));
        const unsupportedActions = new Set([9, 20]); // OTA, legacy reserved action
        const unsupportedProtocols = new Set([2, 3, 4, 5]); // Modbus TCP, HTTP, CoAP, TCP
        const profilePeripheralIds = getProfilePeripheralIds();
        if (isLiteWebProfile()) {
            [16, 17, 18].forEach((actionType) => unsupportedActions.add(actionType)); // Modbus control/poll
            unsupportedProtocols.add(1); // Modbus RTU
        }
        const rules = Array.isArray(doc.rules) ? doc.rules : [];
        doc.rules = rules
            .map((rule) => {
                const ruleId = String((rule && rule.id) || '');
                const keepDisabledTemplate = ruleId.startsWith('exec_stepper_') ||
                    ruleId.startsWith('exec_adc_voltage_') ||
                    ruleId.startsWith('exec_ws2812b_') ||
                    ruleId.startsWith('exec_uart_debug_');
                if (rule && rule.enabled === false && !keepDisabledTemplate) return null;
                if (unsupportedProtocols.has(Number(rule.protocolType))) return null;
                const actions = Array.isArray(rule.actions)
                    ? rule.actions.filter((action) => {
                        if (unsupportedActions.has(Number(action.actionType))) return false;
                        if (isLiteWebProfile() && String(action.targetPeriphId || '').indexOf('modbus') === 0) return false;
                        if (!scriptPeripheralRefsAreKept(action, profilePeripheralIds)) return false;
                        if (actionTargetRequiresPeripheral(action) &&
                            !profilePeripheralIds.has(String(action.targetPeriphId || ''))) {
                            return false;
                        }
                        return true;
                    })
                    : [];
                if (actions.length === 0) return null;
                const next = { ...rule, actions };
                delete next.scriptContent;
                return next;
            })
            .filter(Boolean);
        return JSON.stringify(doc, null, 2) + '\n';
    }

    if (normalizedRel === 'config/peripherals.json') {
        const doc = JSON.parse(fs.readFileSync(sourcePath, 'utf8'));
        if (Array.isArray(doc.peripherals)) {
            doc.peripherals = doc.peripherals.filter(shouldKeepProfilePeripheral);
        }
        return JSON.stringify(doc, null, 2) + '\n';
    }

    if (normalizedRel === 'config/network.json') {
        const doc = JSON.parse(fs.readFileSync(sourcePath, 'utf8'));
        if (isLiteWebProfile()) {
            doc.networkType = 0;
            delete doc.ethernet;
            delete doc.cellular;
            delete doc.lora;
        } else if (isStandardWebProfile()) {
            delete doc.lora;
        }
        return JSON.stringify(doc, null, 2) + '\n';
    }

    return null;
}

function collectStageWhitelist(buildResult) {
    const files = new Set();

    buildResult.staticAssets.forEach((item) => addWhitelistPath(files, item.dest));
    buildResult.syncedModules.forEach((item) => addWhitelistAbsolute(files, item.publishFile));
    buildResult.i18nModules.forEach((item) => addWhitelistAbsolute(files, item.publishFile));
    (buildResult.subdirBundles || []).forEach((item) => addWhitelistAbsolute(files, item.outPath));

    if (buildResult.adminBundle) {
        addWhitelistAbsolute(files, buildResult.adminBundle.outputFile);
        (buildResult.adminBundle.modules || []).forEach((item) => addWhitelistAbsolute(files, item.publishFile));
        (buildResult.adminBundle.stubs || []).forEach((item) => addWhitelistAbsolute(files, item.stubFile));
    }

    if (buildResult.appBundle && Array.isArray(buildResult.appBundle.chunks)) {
        buildResult.appBundle.chunks.forEach((item) => addWhitelistAbsolute(files, item.outPath));
    }

    return files;
}

function copyFileToStage(sourcePath, relPath, stageState) {
    const normalizedRel = normalizeRelativePath(relPath);
    if (shouldSkipProdDataPath(normalizedRel)) return;
    if (stageState.copied.has(normalizedRel)) return;

    const destPath = path.join(STAGING_DATA_DIR, normalizedRel);
    fs.mkdirSync(path.dirname(destPath), { recursive: true });
    const transformed = transformProdConfigFile(sourcePath, normalizedRel);
    if (transformed === null) {
        fs.copyFileSync(sourcePath, destPath);
    } else {
        fs.writeFileSync(destPath, transformed, 'utf8');
        stageState.transformed.push(normalizedRel);
    }

    stageState.copied.add(normalizedRel);
    stageState.fileCount += 1;
    stageState.totalBytes += fs.statSync(destPath).size;
}

function copyDirectoryRecursiveToStage(sourceDir, relPrefix, stageState) {
    if (!fs.existsSync(sourceDir)) return;

    fs.readdirSync(sourceDir).forEach((entry) => {
        const sourcePath = path.join(sourceDir, entry);
        const relPath = relPrefix ? `${relPrefix}/${entry}` : entry;
        const stat = fs.statSync(sourcePath);
        if (stat.isDirectory()) {
            copyDirectoryRecursiveToStage(sourcePath, relPath, stageState);
            return;
        }
        copyFileToStage(sourcePath, relPath, stageState);
    });
}

function stagePublishArtifacts(buildResult) {
    console.log('\n[Step 4] Assembling clean staging publish directory...');
    ensureCleanDir(STAGING_DATA_DIR);

    const stageState = {
        copied: new Set(),
        fileCount: 0,
        totalBytes: 0,
        transformed: [],
        missing: []
    };

    const whitelist = collectStageWhitelist(buildResult);
    whitelist.forEach((relPath) => {
        const sourcePath = path.join(WWW_DIR, relPath);
        const compressedPath = `${sourcePath}.gz`;
        const stageRelPath = `www/${relPath}`;

        if (isCompressiblePath(relPath) && fs.existsSync(compressedPath)) {
            copyFileToStage(compressedPath, `${stageRelPath}.gz`, stageState);
            return;
        }
        if (fs.existsSync(sourcePath)) {
            copyFileToStage(sourcePath, stageRelPath, stageState);
            return;
        }

        stageState.missing.push(stageRelPath);
    });

    fs.readdirSync(WWW_DIR).forEach((entry) => {
        const sourcePath = path.join(WWW_DIR, entry);
        const stat = fs.statSync(sourcePath);

        if (stat.isDirectory()) {
            if (entry === 'css' || entry === 'js' || entry === 'pages') return;
            copyDirectoryRecursiveToStage(sourcePath, `www/${entry}`, stageState);
            return;
        }

        const ext = path.extname(entry).toLowerCase();
        if (entry.endsWith('.gz') || COMPRESS_EXTENSIONS.has(ext)) return;
        copyFileToStage(sourcePath, `www/${entry}`, stageState);
    });

    fs.readdirSync(DATA_DIR).forEach((entry) => {
        if (entry === 'www') return;

        const sourcePath = path.join(DATA_DIR, entry);
        const stat = fs.statSync(sourcePath);

        if (stat.isDirectory()) {
            copyDirectoryRecursiveToStage(sourcePath, entry, stageState);
            return;
        }

        copyFileToStage(sourcePath, entry, stageState);
    });

    console.log(`  Staging data dir: ${STAGING_DATA_DIR}`);
    console.log(`  Staged files: ${stageState.fileCount}`);
    console.log(`  Staged size: ${stageState.totalBytes} bytes`);
    if (stageState.transformed.length > 0) {
        console.log(`  ${getWebProfile()} transformed config(s): ${stageState.transformed.length}`);
        stageState.transformed.forEach((item) => console.log(`    - ${item}`));
    }
    if (stageState.missing.length > 0) {
        console.log(`  Missing staged source(s): ${stageState.missing.length}`);
        stageState.missing.forEach((item) => console.log(`    Skip: ${item}`));
    }

    return {
        dataDir: STAGING_DATA_DIR,
        wwwDir: STAGING_WWW_DIR,
        fileCount: stageState.fileCount,
        totalBytes: stageState.totalBytes,
        transformed: stageState.transformed,
        missing: stageState.missing
    };
}

function deleteOldGzFiles() {
    console.log('\n[Step 1] Removing old .gz files...');

    walkDir(WWW_DIR, (filePath) => {
        if (!filePath.endsWith('.gz')) return;
        if (!shouldTouchWwwFile(filePath)) return;
        try {
            fs.unlinkSync(filePath);
            stats.deleted += 1;
            console.log(`  Deleted: ${path.relative(WWW_DIR, filePath)}`);
        } catch (error) {
            stats.skippedDeletes += 1;
            console.log(`  Skip delete: ${path.relative(WWW_DIR, filePath)} (${error.code || error.message})`);
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
    if (!shouldTouchWwwFile(filePath)) return;

    const content = fs.readFileSync(filePath);
    const compressed = zlib.gzipSync(content, { level: 9 });
    const gzPath = `${filePath}.gz`;

    try {
        fs.writeFileSync(gzPath, compressed);
    } catch (error) {
        stats.failedCompress += 1;
        console.log(`  Skip compress: ${path.relative(WWW_DIR, filePath)} (${error.code || error.message})`);
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

function deleteOriginals() {
    const ASSETS_DIR = path.join(WWW_DIR, 'assets');
    console.log('\n[Step 3] Cleaning up uncompressed originals...');

    let deleted = 0;
    walkDir(WWW_DIR, (filePath) => {
        if (filePath.startsWith(ASSETS_DIR + path.sep) || filePath.startsWith(ASSETS_DIR)) return;
        const ext = path.extname(filePath).toLowerCase();
        if (!COMPRESS_EXTENSIONS.has(ext)) return;
        if (!shouldTouchWwwFile(filePath)) return;
        if (fs.existsSync(filePath + '.gz')) {
            try {
                fs.unlinkSync(filePath);
                deleted++;
            } catch (error) {
                console.log(`  Skip cleanup: ${path.relative(WWW_DIR, filePath)} (${error.code || error.message})`);
            }
        }
    });

    console.log(`  Deleted ${deleted} uncompressed originals`);
}

function getActiveEnvFromPlatformioIni() {
    const iniPath = path.join(__dirname, '..', 'platformio.ini');
    if (!fs.existsSync(iniPath)) {
        return 'esp32';
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
    return activeEnv || 'esp32';
}

function runPioCommand(args, label, options = {}) {
    const { allowFailure = false, env: extraEnv = null } = options;
    return new Promise((resolve, reject) => {
        const cmdStr = `pio ${args.join(' ')}`;
        console.log(`[${label}] 执行: ${cmdStr}`);
        const child = spawn('pio', args, {
            stdio: 'inherit',
            shell: true,
            env: extraEnv ? { ...process.env, ...extraEnv } : process.env
        });
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
    console.log(`Staging directory: ${STAGING_WWW_DIR}`);

    console.log('\n[Step 0] Building web modules...');
    const buildResult = buildWebModules();
    buildResult.syncedModules.forEach((item) => {
        console.log(`  Synced: ${path.relative(ROOT_DIR(), item.publishFile)} (${item.size} bytes)`);
    });
    buildResult.i18nModules.forEach((item) => {
        console.log(`  Synced: ${path.relative(ROOT_DIR(), item.publishFile)} (${item.size} bytes${item.minified ? ', minified' : ''})`);
    });
    console.log(`  Built: ${path.relative(ROOT_DIR(), buildResult.adminBundle.outputFile)} (${buildResult.adminBundle.size} bytes)`);
    if (buildResult.adminBundle.modules) {
        console.log(`  Generated ${buildResult.adminBundle.modules.length} admin module files`);
    } else if (buildResult.adminBundle.stubs) {
        console.log(`  Generated ${buildResult.adminBundle.stubs.length} admin stubs`);
    }

    console.log('\n[Step 1] Generating SW manifest...');
    generateManifest();

    // 精简版不部署 SW，移除 generateManifest 产生的 sw.js
    if (isLiteWebProfile()) {
        const swFile = path.join(WWW_DIR, 'sw.js');
        if (fs.existsSync(swFile)) fs.unlinkSync(swFile);
    }

    if (isCompactWebProfile()) {
        activeWwwWhitelist = collectStageWhitelist(buildResult);
        console.log(`  ${getWebProfile()} compression whitelist: ${activeWwwWhitelist.size} web file(s)`);
    }

    deleteOldGzFiles();
    compressAllFiles();
    deleteOriginals();
    const stageResult = stagePublishArtifacts(buildResult);

    console.log('\n========================================');
    console.log('  Compression completed');
    console.log('========================================');

    if (noUpload) {
        console.log(`\nStaging package ready: ${stageResult.wwwDir}`);
        console.log('\n提示: 已跳过上传和监视（--no-upload）');
        return;
    }

    const envName = getActiveEnvFromPlatformioIni();
    console.log(`\n[Step 5] Uploading filesystem from staging dir (env: ${envName})...`);
    try {
        await runPioCommand(
            ['run', '--target', 'uploadfs', '--environment', envName],
            'UploadFS',
            { env: { PLATFORMIO_DATA_DIR: stageResult.dataDir } }
        );
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

    console.log('\n[Step 6] Opening serial monitor...');
    console.log('提示: 按 Ctrl+C 退出串口监视器\n');
    await runPioCommand(['device', 'monitor', '--environment', envName], 'Monitor', { allowFailure: true });
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});

function ROOT_DIR() {
    return path.join(__dirname, '..');
}
