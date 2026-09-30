'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn, spawnSync } = require('child_process');
const {
    acquireWebPublishLock,
    readOwner,
    withWebPublishLock
} = require('./web-publish-lock');
const { createStagingRunId, findUncompressedTextAssets } = require('./gzip-www');
const { runNodeScriptUnderFirmwareLock } = require('./firmware-build-lock-runner');

const ROOT_DIR = path.resolve(__dirname, '..');
const SELF = __filename;
// Real writers now include build-host Zopfli compression. Six entrypoints
// queue behind one lock; allow their total work, not just one fast build.
// The dedicated bounded-wait tests retain their short deadlines below.
const REAL_WRITER_LOCK_TIMEOUT_MS = 90_000;
const REAL_WRITER_PROCESS_TIMEOUT_MS = 120_000;

function sleepSync(milliseconds) {
    const buffer = new Int32Array(new SharedArrayBuffer(4));
    Atomics.wait(buffer, 0, 0, Math.max(1, milliseconds));
}

function appendEvent(eventFile, event) {
    fs.appendFileSync(eventFile, `${JSON.stringify(event)}\n`, 'utf8');
}

function runWorkerMode() {
    const mode = process.argv[2];
    if (mode === '--worker') {
        const [, , , lockDir, eventFile, name, holdMsText] = process.argv;
        withWebPublishLock(() => {
            appendEvent(eventFile, { name, state: 'enter', at: Date.now() });
            sleepSync(Number.parseInt(holdMsText, 10));
            appendEvent(eventFile, { name, state: 'exit', at: Date.now() });
        }, {
            lockDir,
            label: `test-worker-${name}`,
            timeoutMs: 5_000,
            staleMs: 5_000,
            pollIntervalMs: 10
        });
        return true;
    }

    if (mode === '--abandon') {
        const [, , , lockDir, ownerSnapshot] = process.argv;
        const lock = acquireWebPublishLock({
            lockDir,
            label: 'test-abandoned-owner',
            timeoutMs: 2_000,
            staleMs: 60_000,
            pollIntervalMs: 10
        });
        fs.writeFileSync(ownerSnapshot, JSON.stringify(lock.owner), 'utf8');
        return true;
    }
    return false;
}

function runProcess(command, args, options = {}) {
    return new Promise((resolve, reject) => {
        const child = spawn(command, args, {
            cwd: options.cwd || ROOT_DIR,
            env: options.env || process.env,
            windowsHide: true,
            stdio: ['ignore', 'pipe', 'pipe']
        });
        let stdout = '';
        let stderr = '';
        const timer = setTimeout(() => {
            child.kill();
            reject(new Error(`Timed out running ${command} ${args.join(' ')}`));
        }, options.timeoutMs || 60_000);
        child.stdout.on('data', (chunk) => { stdout += chunk.toString(); if (options.onOutput) options.onOutput(stdout); });
        child.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
        child.on('error', (error) => {
            clearTimeout(timer);
            reject(error);
        });
        child.on('close', (code, signal) => {
            clearTimeout(timer);
            resolve({ code, signal, stdout, stderr });
        });
    });
}

function delay(milliseconds) {
    return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function trackedProcess(command, args, options = {}) {
    let settled = false, output = '';
    const promise = runProcess(command, args, { ...options, onOutput: text => { output = text; } }).finally(() => {
        settled = true;
    });
    return { promise, isSettled: () => settled, output: () => output };
}

async function waitForEvent(eventFile, predicate, timeoutMs = 3_000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (fs.existsSync(eventFile)) {
            const events = fs.readFileSync(eventFile, 'utf8')
                .split(/\r?\n/)
                .filter(Boolean)
                .map((line) => JSON.parse(line));
            if (events.some(predicate)) return;
        }
        await new Promise((resolve) => setTimeout(resolve, 10));
    }
    throw new Error(`Timed out waiting for lock event in ${eventFile}`);
}

function assertSuccessful(result, label) {
    assert.strictEqual(
        result.code,
        0,
        `${label} failed (exit=${result.code}, signal=${result.signal || 'none'})\n${result.stdout}\n${result.stderr}`
    );
}

async function testMutualExclusion(tempRoot) {
    const lockDir = path.join(tempRoot, 'serial.lock');
    const eventFile = path.join(tempRoot, 'events.jsonl');
    const first = runProcess(process.execPath, [SELF, '--worker', lockDir, eventFile, 'first', '250']);
    await waitForEvent(eventFile, (event) => event.name === 'first' && event.state === 'enter');
    const second = runProcess(process.execPath, [SELF, '--worker', lockDir, eventFile, 'second', '10']);
    const [firstResult, secondResult] = await Promise.all([first, second]);
    assertSuccessful(firstResult, 'first lock worker');
    assertSuccessful(secondResult, 'second lock worker');

    const events = fs.readFileSync(eventFile, 'utf8')
        .trim()
        .split(/\r?\n/)
        .map((line) => JSON.parse(line));
    assert.deepStrictEqual(
        events.map((event) => `${event.name}:${event.state}`),
        ['first:enter', 'first:exit', 'second:enter', 'second:exit']
    );
    assert.ok(events[2].at >= events[1].at, 'second writer must enter after the first writer exits');
    assert.ok(!fs.existsSync(lockDir), 'lock directory must be removed after both writers finish');
}

function testBoundedWaitAndExceptionRelease(tempRoot) {
    const timeoutLockDir = path.join(tempRoot, 'timeout.lock');
    const held = acquireWebPublishLock({ lockDir: timeoutLockDir, label: 'timeout-holder' });
    assert.ok(fs.statSync(timeoutLockDir).isFile(), 'the published lock must be an atomic owner file');
    assert.strictEqual(readOwner(timeoutLockDir).token, held.owner.token);
    try {
        assert.throws(() => acquireWebPublishLock({
            lockDir: timeoutLockDir,
            label: 'timeout-contender',
            timeoutMs: 80,
            staleMs: 60_000,
            pollIntervalMs: 10
        }), /Timed out after 80ms waiting for Web publish lock/);
    } finally {
        held.release();
    }

    const exceptionLockDir = path.join(tempRoot, 'exception.lock');
    assert.throws(() => withWebPublishLock(() => {
        throw new Error('intentional lock task failure');
    }, { lockDir: exceptionLockDir, label: 'exception-test' }), /intentional lock task failure/);
    assert.ok(!fs.existsSync(exceptionLockDir), 'exception path must release the lock in finally behavior');

    const retry = acquireWebPublishLock({ lockDir: exceptionLockDir, label: 'exception-retry' });
    assert.ok(retry.release(), 'lock must be acquirable after an exception');
}

async function testPromiseRelease(tempRoot) {
    const resolveLockDir = path.join(tempRoot, 'promise-resolve.lock');
    const resolved = await withWebPublishLock(async () => {
        await delay(10);
        return 'resolved-value';
    }, { lockDir: resolveLockDir, label: 'promise-resolve' });
    assert.strictEqual(resolved, 'resolved-value');
    assert.ok(!fs.existsSync(resolveLockDir), 'resolved Promise must release the lock');

    const rejectLockDir = path.join(tempRoot, 'promise-reject.lock');
    await assert.rejects(withWebPublishLock(async () => {
        await delay(10);
        throw new Error('intentional Promise rejection');
    }, { lockDir: rejectLockDir, label: 'promise-reject' }), /intentional Promise rejection/);
    assert.ok(!fs.existsSync(rejectLockDir), 'rejected Promise must release the lock');

    const retry = acquireWebPublishLock({ lockDir: rejectLockDir, label: 'promise-retry' });
    assert.ok(retry.release(), 'lock must be acquirable after a rejected Promise');
}

function testRetryableRelease(tempRoot) {
    const lockDir = path.join(tempRoot, 'retryable-release.lock');
    const lock = acquireWebPublishLock({ lockDir, label: 'retryable-release' });
    const originalRmSync = fs.rmSync;
    let injectedFailures = 0;
    fs.rmSync = (targetPath, options) => {
        if (path.resolve(targetPath) === path.resolve(lockDir)) {
            injectedFailures += 1;
            const error = new Error('intentional release failure');
            error.code = 'EPERM';
            throw error;
        }
        return originalRmSync(targetPath, options);
    };
    try {
        assert.strictEqual(lock.release(), false, 'failed delete must report an unreleased lock');
    } finally {
        fs.rmSync = originalRmSync;
    }
    assert.ok(injectedFailures >= 2, 'release must perform bounded internal retries');
    assert.ok(fs.existsSync(lockDir), 'failed release must leave the owned lock visible');
    assert.ok(lock.release(), 'the same handle must allow a later release retry');
}

async function testWrapperFailsClosedOnReleaseFailure(tempRoot) {
    const originalRmSync = fs.rmSync;
    const injectReleaseFailure = (lockDir) => {
        fs.rmSync = (targetPath, options) => {
            if (path.resolve(targetPath) === path.resolve(lockDir)) {
                const error = new Error('intentional wrapper release failure');
                error.code = 'EPERM';
                throw error;
            }
            return originalRmSync(targetPath, options);
        };
    };

    const syncLockDir = path.join(tempRoot, 'wrapper-sync-release.lock');
    try {
        assert.throws(() => withWebPublishLock(() => {
            injectReleaseFailure(syncLockDir);
            return 'must-not-succeed';
        }, { lockDir: syncLockDir, label: 'wrapper-sync-release' }),
        /Failed to release Web publish lock/);
    } finally {
        fs.rmSync = originalRmSync;
        originalRmSync(syncLockDir, { force: true });
    }

    const asyncLockDir = path.join(tempRoot, 'wrapper-async-release.lock');
    try {
        await assert.rejects(withWebPublishLock(async () => {
            injectReleaseFailure(asyncLockDir);
            return 'must-not-resolve';
        }, { lockDir: asyncLockDir, label: 'wrapper-async-release' }),
        /Failed to release Web publish lock/);
    } finally {
        fs.rmSync = originalRmSync;
        originalRmSync(asyncLockDir, { force: true });
    }
}

function testTransientMetadataRetry(tempRoot) {
    const rootDir = path.join(tempRoot, 'metadata-retry');
    const sourcePath = path.join(rootDir, 'transient.js');
    fs.mkdirSync(rootDir);
    fs.writeFileSync(sourcePath, 'const ready = true;\n', 'utf8');

    const originalStatSync = fs.statSync;
    let failuresRemaining = 3;
    fs.statSync = (targetPath, options) => {
        if (path.resolve(targetPath) === path.resolve(sourcePath) && failuresRemaining > 0) {
            failuresRemaining -= 1;
            const error = new Error('intentional transient metadata failure');
            error.code = 'EPERM';
            throw error;
        }
        return originalStatSync(targetPath, options);
    };
    try {
        assert.deepStrictEqual(findUncompressedTextAssets(rootDir), ['transient.js']);
    } finally {
        fs.statSync = originalStatSync;
    }
    assert.strictEqual(failuresRemaining, 0, 'metadata traversal must retry transient EPERM failures');
}

function testDeadAndExpiredRecovery(tempRoot) {
    const deadLockDir = path.join(tempRoot, 'dead.lock');
    const ownerSnapshot = path.join(tempRoot, 'dead-owner.json');
    const abandoned = spawnSync(process.execPath, [SELF, '--abandon', deadLockDir, ownerSnapshot], {
        cwd: ROOT_DIR,
        encoding: 'utf8',
        windowsHide: true
    });
    assert.strictEqual(abandoned.status, 0, abandoned.stderr || abandoned.stdout);
    const abandonedOwner = JSON.parse(fs.readFileSync(ownerSnapshot, 'utf8'));
    const onDiskOwner = readOwner(deadLockDir);
    assert.strictEqual(onDiskOwner.token, abandonedOwner.token);
    assert.strictEqual(typeof onDiskOwner.pid, 'number');
    assert.strictEqual(typeof onDiskOwner.startedAt, 'string');
    assert.strictEqual(typeof onDiskOwner.startedAtMs, 'number');
    assert.strictEqual(onDiskOwner.label, 'test-abandoned-owner');

    const recovered = acquireWebPublishLock({
        lockDir: deadLockDir,
        label: 'dead-owner-recovery',
        timeoutMs: 2_000,
        staleMs: 60_000,
        pollIntervalMs: 10
    });
    assert.notStrictEqual(recovered.owner.token, abandonedOwner.token);
    assert.ok(recovered.release(), 'recovered dead-owner lock must release cleanly');

    const expiredLockDir = path.join(tempRoot, 'expired.lock');
    fs.mkdirSync(expiredLockDir);
    const old = new Date(Date.now() - 60_000);
    fs.utimesSync(expiredLockDir, old, old);
    const expiredRecovery = acquireWebPublishLock({
        lockDir: expiredLockDir,
        label: 'expired-owner-recovery',
        timeoutMs: 2_000,
        staleMs: 60_000,
        pollIntervalMs: 10
    });
    assert.ok(expiredRecovery.release(), 'expired ownerless lock must be recovered and released');
}

function testSourceIntegrationAndStageIds() {
    const buildAssets = fs.readFileSync(path.join(ROOT_DIR, 'scripts', 'build-web-assets.js'), 'utf8');
    const buildModules = fs.readFileSync(path.join(ROOT_DIR, 'scripts', 'build-web-modules.js'), 'utf8');
    const buildAdmin = fs.readFileSync(path.join(ROOT_DIR, 'scripts', 'build-admin-bundle.js'), 'utf8');
    const buildApp = fs.readFileSync(path.join(ROOT_DIR, 'scripts', 'build-app-bundle.js'), 'utf8');
    const generateManifest = fs.readFileSync(path.join(ROOT_DIR, 'scripts', 'generate-sw-manifest.js'), 'utf8');
    const gzip = fs.readFileSync(path.join(ROOT_DIR, 'scripts', 'gzip-www.js'), 'utf8');
    const artifactHook = fs.readFileSync(path.join(ROOT_DIR, 'scripts', 'fastbee-artifacts.py'), 'utf8');
    assert.ok(buildAssets.includes("require('./web-publish-lock')"));
    assert.ok(buildAssets.includes('withWebPublishLock(runBuildUnlocked'));
    assert.ok(gzip.includes("require('./web-publish-lock')"));
    assert.ok(gzip.includes('withWebPublishLock(buildPublishStage'));
    assert.ok(buildModules.includes('withWebPublishLock(buildWebModules'));
    assert.ok(buildAdmin.includes('withWebPublishLock(() => buildAdminBundle()'));
    assert.ok(buildApp.includes('withWebPublishLock(() => buildAppBundle()'));
    assert.ok(generateManifest.includes('withWebPublishLock(generateManifest'));
    assert.ok(artifactHook.includes('os.getpid()'));
    assert.ok(artifactHook.includes('secrets.token_hex(4)'));

    const ids = new Set(Array.from({ length: 32 }, () => createStagingRunId()));
    assert.strictEqual(ids.size, 32, 'staging run IDs must remain unique within one process and millisecond');
    ids.forEach((id) => {
        assert.match(id, new RegExp(`^run-\\d+-${process.pid}-[0-9a-f]{8}$`));
    });
}

async function testPublicWriterEntrypoints(tempRoot) {
    const lockDir = path.join(tempRoot, 'public-writers.lock');
    const stageDir = path.join(tempRoot, 'public-writer-stage');
    const env = {
        ...process.env,
        FASTBEE_WEB_LOCK_DIR: lockDir,
        FASTBEE_WEB_LOCK_TIMEOUT_MS: String(REAL_WRITER_LOCK_TIMEOUT_MS),
        FASTBEE_WEB_LOCK_STALE_MS: '600000',
        FASTBEE_WEB_LOCK_POLL_MS: '10'
    };
    const holder = acquireWebPublishLock({ lockDir, label: 'public-writer-test-holder' });
    const writers = [
        ['build-web-assets', 'build-web-assets.js', ['--env=esp32s3-F16R8']],
        ['gzip-www', 'gzip-www.js', [
            '--no-upload',
            '--no-monitor',
            '--env=esp32s3-F16R8',
            `--stage-dir=${stageDir}`
        ]],
        ['build-web-modules', 'build-web-modules.js', ['--env=esp32s3-F16R8']],
        ['build-admin-bundle', 'build-admin-bundle.js', ['--env=esp32s3-F16R8']],
        ['build-app-bundle', 'build-app-bundle.js', ['--env=esp32s3-F16R8']],
        ['generate-sw-manifest', 'generate-sw-manifest.js', []]
    ].map(([label, script, args]) => ({
        label,
        tracked: trackedProcess(process.execPath, [path.join(ROOT_DIR, 'scripts', script), ...args], {
            env,
            timeoutMs: REAL_WRITER_PROCESS_TIMEOUT_MS
        })
    }));

    let contentionError;
    try {
        const deadline = Date.now() + 20_000;
        while (!writers.every(({ tracked }) => /\[WebPublishLock\] Waiting for /.test(tracked.output()))) {
            const exited = writers.find(({ tracked }) => tracked.isSettled());
            assert.ok(!exited, `${exited && exited.label} exited before announcing lock contention`);
            assert.ok(Date.now() < deadline, 'public writers must announce contention within 20 seconds');
            await delay(10);
        }
        writers.forEach(({ label, tracked }) => {
            assert.ok(!tracked.isSettled(), `${label} must wait while another publisher owns the lock`);
        });
    } catch (error) { contentionError = error; }
    finally { assert.ok(holder.release(), 'public writer test holder must release cleanly'); }
    const settled = await Promise.allSettled(writers.map(({ tracked }) => tracked.promise));
    if (contentionError) throw contentionError;
    const results = settled.map(result => {
        if (result.status === 'rejected') throw result.reason;
        return result.value;
    });
    results.forEach((result, index) => {
        const label = writers[index].label;
        assertSuccessful(result, `${label} public writer`);
        assert.match(result.stdout, /\[WebPublishLock\] Waiting for /, `${label} must announce lock contention`);
    });
}

async function testRealWriterSerialization(tempRoot) {
    const stageDir = path.join(tempRoot, 'gzip-stage');
    const env = {
        ...process.env,
        FASTBEE_WEB_LOCK_TIMEOUT_MS: String(REAL_WRITER_LOCK_TIMEOUT_MS),
        FASTBEE_WEB_LOCK_STALE_MS: '600000',
        FASTBEE_WEB_LOCK_POLL_MS: '10'
    };
    const build = runProcess(process.execPath, [
        path.join(ROOT_DIR, 'scripts', 'build-web-assets.js'),
        '--env=esp32s3-F16R8'
    ], { env, timeoutMs: REAL_WRITER_PROCESS_TIMEOUT_MS });
    const gzip = runProcess(process.execPath, [
        path.join(ROOT_DIR, 'scripts', 'gzip-www.js'),
        '--no-upload',
        '--no-monitor',
        '--env=esp32s3-F16R8',
        `--stage-dir=${stageDir}`
    ], { env, timeoutMs: REAL_WRITER_PROCESS_TIMEOUT_MS });
    const [buildResult, gzipResult] = await Promise.all([build, gzip]);
    assertSuccessful(buildResult, 'concurrent build-web-assets writer');
    assertSuccessful(gzipResult, 'concurrent gzip-www writer');

    const manifest = JSON.parse(fs.readFileSync(
        path.join(ROOT_DIR, 'data', 'www', 'web-resource-manifest.json'),
        'utf8'
    ));
    assert.strictEqual(manifest.environment, 'esp32s3-F16R8');
    assert.ok(fs.existsSync(path.join(ROOT_DIR, 'data', 'www', 'setup.html.gz')));
    assert.ok(fs.existsSync(path.join(stageDir, 'www', 'setup.html.gz')));
}

async function main() {
    const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-web-lock-'));
    assert.ok(path.resolve(tempRoot).startsWith(path.resolve(os.tmpdir())), 'test cleanup must stay in the OS temp directory');
    try {
        testSourceIntegrationAndStageIds();
        await testMutualExclusion(tempRoot);
        testBoundedWaitAndExceptionRelease(tempRoot);
        await testPromiseRelease(tempRoot);
        testRetryableRelease(tempRoot);
        await testWrapperFailsClosedOnReleaseFailure(tempRoot);
        testTransientMetadataRetry(tempRoot);
        testDeadAndExpiredRecovery(tempRoot);
        await testPublicWriterEntrypoints(tempRoot);
        await testRealWriterSerialization(tempRoot);
    } finally {
        fs.rmSync(tempRoot, { recursive: true, force: true });
    }
    console.log('Web publish lock contract OK: all public writers serialized, bounded wait, recovery, and unique staging');
}

if (runWorkerMode()) {
    // Worker modes intentionally stop here.
} else if (require.main === module) {
    const lockExitCode = runNodeScriptUnderFirmwareLock({ label: 'test-web-publish-lock', environment: 'esp32s3-F16R8' });
    if (lockExitCode !== null) process.exitCode = lockExitCode;
    else main().catch((error) => { console.error(error); process.exitCode = 1; });
}
