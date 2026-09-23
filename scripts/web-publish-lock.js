'use strict';

const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const DEFAULT_LOCK_DIR = path.join(ROOT_DIR, '.pio', 'locks', 'web-publish.lock');
const OWNER_FILE = 'owner.json';
const DEFAULT_TIMEOUT_MS = 120_000;
const DEFAULT_STALE_MS = 10 * 60_000;
const DEFAULT_OWNERLESS_STALE_MS = 5_000;
const DEFAULT_POLL_INTERVAL_MS = 50;
const RELEASE_ATTEMPTS = 4;
const RELEASE_RETRY_MS = 25;

const sleepBuffer = new Int32Array(new SharedArrayBuffer(4));

function positiveInteger(value, fallback) {
    const parsed = Number.parseInt(value, 10);
    return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
}

function sleepSync(milliseconds) {
    Atomics.wait(sleepBuffer, 0, 0, Math.max(1, milliseconds));
}

function createToken() {
    return `${process.pid}-${Date.now()}-${crypto.randomBytes(8).toString('hex')}`;
}

function ownerPath(lockPath) {
    try {
        return fs.statSync(lockPath).isDirectory() ? path.join(lockPath, OWNER_FILE) : lockPath;
    } catch (_) {
        return lockPath;
    }
}

function readOwner(lockPath) {
    try {
        return JSON.parse(fs.readFileSync(ownerPath(lockPath), 'utf8'));
    } catch (_) {
        return null;
    }
}

function writeOwner(candidateDir, owner) {
    const finalPath = path.join(candidateDir, OWNER_FILE);
    const temporaryPath = path.join(candidateDir, `.owner-${owner.token}.tmp`);
    fs.writeFileSync(temporaryPath, `${JSON.stringify(owner, null, 2)}\n`, 'utf8');
    fs.renameSync(temporaryPath, finalPath);
}

function processIsAlive(pid) {
    if (!Number.isSafeInteger(pid) || pid <= 0) return false;
    if (pid === process.pid) return true;
    try {
        process.kill(pid, 0);
        return true;
    } catch (error) {
        if (error && error.code === 'EPERM') return true;
        if (error && (error.code === 'ESRCH' || error.code === 'EINVAL')) return false;
        return false;
    }
}

function lockAgeMs(lockPath, owner, nowMs = Date.now()) {
    const ownerStart = Number(owner && owner.startedAtMs);
    if (Number.isFinite(ownerStart) && ownerStart > 0) {
        return Math.max(0, nowMs - ownerStart);
    }
    try {
        return Math.max(0, nowMs - fs.statSync(lockPath).mtimeMs);
    } catch (_) {
        return 0;
    }
}

function ownerDescription(owner) {
    if (!owner) return 'owner metadata unavailable';
    return `pid=${owner.pid || 'unknown'}, label=${owner.label || 'unknown'}, started=${owner.startedAt || 'unknown'}`;
}

function createOwnedCandidate(targetPath, owner) {
    const candidateDir = `${targetPath}.candidate-${owner.token}`;
    fs.mkdirSync(candidateDir);
    try {
        writeOwner(candidateDir, owner);
        return candidateDir;
    } catch (error) {
        fs.rmSync(candidateDir, { recursive: true, force: true });
        throw error;
    }
}

function cleanupCandidate(candidateDir) {
    try {
        fs.rmSync(candidateDir, { recursive: true, force: true, maxRetries: 2, retryDelay: 10 });
    } catch (error) {
        console.warn(`[WebPublishLock] Could not remove candidate ${candidateDir}: ${error.message}`);
    }
}

function publishCandidate(candidateDir, targetPath) {
    try {
        // A hard link publishes a fully written owner in one exclusive filesystem operation.
        fs.linkSync(path.join(candidateDir, OWNER_FILE), targetPath);
        return true;
    } catch (error) {
        if (error && error.code === 'EEXIST') return false;
        if (error && ['EACCES', 'EPERM', 'ENOTEMPTY'].includes(error.code) && fs.existsSync(targetPath)) {
            return false;
        }
        throw error;
    }
}

function removePathWithRetries(targetPath, attempts = RELEASE_ATTEMPTS) {
    for (let attempt = 1; attempt <= attempts; attempt += 1) {
        try {
            fs.rmSync(targetPath, { recursive: true, force: true, maxRetries: 1, retryDelay: RELEASE_RETRY_MS });
        } catch (_) {
            // Existence is checked below so a completed delete reported late still succeeds.
        }
        if (!fs.existsSync(targetPath)) return true;
        if (attempt < attempts) sleepSync(RELEASE_RETRY_MS);
    }
    return false;
}

function removeOwnedPath(lockPath, token) {
    if (!fs.existsSync(lockPath)) return { complete: true, removed: false };

    const owner = readOwner(lockPath);
    if (!owner) return { complete: false, removed: false };
    if (owner.token !== token) return { complete: true, removed: false };

    const removed = removePathWithRetries(lockPath);
    return { complete: removed, removed };
}

function recoveryGuardPath(lockPath) {
    return `${lockPath}.recovery`;
}

function abandonmentReason(lockPath, owner, staleMs, ownerlessStaleMs) {
    const ageMs = lockAgeMs(lockPath, owner);
    if (!owner) return ageMs >= ownerlessStaleMs ? 'expired owner metadata' : '';

    const ownerHost = String(owner.host || '').toLowerCase();
    const localHost = os.hostname().toLowerCase();
    const sameHost = !ownerHost || ownerHost === localHost;
    if (sameHost && !processIsAlive(Number(owner.pid))) return 'dead owner';
    if (!sameHost && ageMs >= staleMs) return 'expired remote owner';
    return '';
}

function tryAcquireRecoveryGuard(lockPath, contender, staleMs, ownerlessStaleMs) {
    const guardPath = recoveryGuardPath(lockPath);
    const candidateDir = createOwnedCandidate(guardPath, contender);
    try {
        if (publishCandidate(candidateDir, guardPath)) {
            return { guardPath, token: contender.token };
        }
    } finally {
        cleanupCandidate(candidateDir);
    }

    const guardOwner = readOwner(guardPath);
    if (abandonmentReason(guardPath, guardOwner, staleMs, ownerlessStaleMs)) {
        removePathWithRetries(guardPath);
    }
    return null;
}

function tryRecoverAbandonedLock(lockPath, contender, staleMs, ownerlessStaleMs) {
    const guard = tryAcquireRecoveryGuard(lockPath, contender, staleMs, ownerlessStaleMs);
    if (!guard) return false;

    try {
        if (!fs.existsSync(lockPath)) return true;

        const owner = readOwner(lockPath);
        const reason = abandonmentReason(lockPath, owner, staleMs, ownerlessStaleMs);
        if (!reason) return false;

        console.warn(`[WebPublishLock] Recovering ${reason}: ${ownerDescription(owner)}`);
        return removePathWithRetries(lockPath);
    } finally {
        const released = removeOwnedPath(guard.guardPath, guard.token);
        if (!released.complete) {
            console.warn(`[WebPublishLock] Could not release recovery guard ${guard.guardPath}`);
        }
    }
}

function createLockHandle(lockPath, owner) {
    let released = false;
    return {
        lockDir: lockPath,
        owner,
        release() {
            if (released) return false;
            const outcome = removeOwnedPath(lockPath, owner.token);
            if (outcome.complete) released = true;
            if (!outcome.complete) {
                console.warn(`[WebPublishLock] Release failed for ${lockPath}; retry is allowed`);
            }
            return outcome.removed;
        }
    };
}

function acquireWebPublishLock(options = {}) {
    const lockPath = path.resolve(
        options.lockDir || process.env.FASTBEE_WEB_LOCK_DIR || DEFAULT_LOCK_DIR
    );
    const timeoutMs = positiveInteger(
        options.timeoutMs || process.env.FASTBEE_WEB_LOCK_TIMEOUT_MS,
        DEFAULT_TIMEOUT_MS
    );
    const staleMs = positiveInteger(
        options.staleMs || process.env.FASTBEE_WEB_LOCK_STALE_MS,
        DEFAULT_STALE_MS
    );
    const pollIntervalMs = positiveInteger(
        options.pollIntervalMs || process.env.FASTBEE_WEB_LOCK_POLL_MS,
        DEFAULT_POLL_INTERVAL_MS
    );
    const ownerlessFallbackMs = Math.max(
        pollIntervalMs,
        Math.min(DEFAULT_OWNERLESS_STALE_MS, Math.floor(timeoutMs / 2))
    );
    const ownerlessStaleMs = positiveInteger(
        options.ownerlessStaleMs || process.env.FASTBEE_WEB_LOCK_OWNERLESS_STALE_MS,
        ownerlessFallbackMs
    );
    const startedAtMs = Date.now();
    const owner = {
        token: createToken(),
        pid: process.pid,
        startedAt: new Date(startedAtMs).toISOString(),
        startedAtMs,
        label: String(options.label || path.basename(process.argv[1] || 'node')),
        host: os.hostname()
    };
    const deadline = startedAtMs + timeoutMs;
    let announcedWait = false;

    fs.mkdirSync(path.dirname(lockPath), { recursive: true });
    const candidateDir = createOwnedCandidate(lockPath, owner);
    try {
        while (true) {
            if (publishCandidate(candidateDir, lockPath)) {
                return createLockHandle(lockPath, owner);
            }

            if (tryRecoverAbandonedLock(lockPath, owner, staleMs, ownerlessStaleMs)) continue;

            const currentOwner = readOwner(lockPath);
            if (!announcedWait) {
                console.log(`[WebPublishLock] Waiting for ${lockPath} (${ownerDescription(currentOwner)})`);
                announcedWait = true;
            }
            if (Date.now() >= deadline) {
                throw new Error(
                    `Timed out after ${timeoutMs}ms waiting for Web publish lock ${lockPath} ` +
                    `(${ownerDescription(currentOwner)})`
                );
            }
            sleepSync(Math.min(pollIntervalMs, Math.max(1, deadline - Date.now())));
        }
    } finally {
        cleanupCandidate(candidateDir);
    }
}

function withWebPublishLock(task, options = {}) {
    if (typeof task !== 'function') throw new TypeError('Web publish lock task must be a function');
    const lock = acquireWebPublishLock(options);
    const releaseOrThrow = () => {
        if (lock.release()) return;
        throw new Error(`Failed to release Web publish lock ${lock.lockDir}`);
    };
    let result;
    try {
        result = task(lock.owner);
    } catch (error) {
        try {
            releaseOrThrow();
        } catch (releaseError) {
            error.releaseError = releaseError;
        }
        throw error;
    }

    if (result && typeof result.then === 'function') {
        return Promise.resolve(result).then(
            (value) => {
                releaseOrThrow();
                return value;
            },
            (error) => {
                try {
                    releaseOrThrow();
                } catch (releaseError) {
                    error.releaseError = releaseError;
                }
                throw error;
            }
        );
    }
    releaseOrThrow();
    return result;
}

module.exports = {
    DEFAULT_LOCK_DIR,
    acquireWebPublishLock,
    processIsAlive,
    readOwner,
    withWebPublishLock
};
