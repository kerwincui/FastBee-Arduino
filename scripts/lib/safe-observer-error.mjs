import fs from 'node:fs';
import path from 'node:path';
import { createHash, randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';

const privateRoot = fileURLToPath(new URL('../../.device-config-backups/observer-errors/', import.meta.url));
const errorTypes = new Set(['Error', 'TypeError', 'RangeError', 'SyntaxError', 'AssertionError', 'TimeoutError', 'AbortError']);
const phases = new Set(['observer', 'browser', 'page', 'pageerror', 'console', 'menu', 'api', 'http-body', 'http-evidence',
    'dashboard', 'fatal', 'evidence', 'report', 'worker', 'route', 'watchdog', 'resource', 'input', 'request',
    'probe', 'connect', 'headers', 'body', 'login', 'logout', 'cleanup', 'context-close']);
const causeCodes = new Set(['ECONNRESET', 'ECONNREFUSED', 'ETIMEDOUT', 'ENETUNREACH', 'EHOSTUNREACH',
    'EAI_AGAIN', 'ENOTFOUND', 'UND_ERR_CONNECT_TIMEOUT', 'UND_ERR_HEADERS_TIMEOUT', 'UND_ERR_BODY_TIMEOUT',
    'UND_ERR_SOCKET', 'ABORT_ERR']);

// Only this bounded metadata list is public. Every captured original remains
// in a fresh private file, even when its public metadata ages out of the list.
export const safeObservationEvents = [];
export const safeObservationCaptureStats = { captured: 0, failed: 0, publicEventsDropped: 0 };

export function safeObservationFault(error, phase = 'observer', options = {}) {
    const normalizedPhase = String(phase).toLowerCase().replaceAll('_', '-');
    const fault = {
        errorType: errorTypes.has(error?.name) ? error.name : 'Error',
        phase: phases.has(normalizedPhase) ? normalizedPhase : 'observer',
        safeCode: 'OBSERVER_ERROR'
    };
    const causeCode = error?.cause?.code ?? error?.code;
    if (causeCodes.has(causeCode)) fault.causeCode = causeCode;
    const event = { ...fault, privateRawStored: false };
    try {
        const message = String(error?.message ?? error);
        // Keep the existing first-300-character console 401 predicate. Only a
        // fixed known status is public; the surrounding original stays private.
        if (fault.phase === 'console' && /status of 401|401 [(]Unauthorized[)]/i.test(message.slice(0, 300))) {
            fault.safeCode = 'OBSERVER_ERROR 401 (Unauthorized)';
            fault.causeCode = 'HTTP_401';
            Object.assign(event, fault);
        }
        const directory = options.privateDirectory || privateRoot;
        fs.mkdirSync(directory, { recursive: true });
        const filename = path.join(directory, `${process.pid}-${randomUUID()}.json`);
        const raw = JSON.stringify({ at: new Date().toISOString(), ...fault,
            message, stack: String(error?.stack ?? '') });
        fs.writeFileSync(filename, raw, { flag: 'wx', mode: 0o600 });
        event.privateRawStored = true;
        event.privateEvidence = filename;
        event.privateSha256 = createHash('sha256').update(raw).digest('hex');
        safeObservationCaptureStats.captured++;
    } catch {
        event.privateCaptureFailed = true;
        safeObservationCaptureStats.failed++;
    }
    if (safeObservationEvents.length === 128) {
        safeObservationEvents.shift();
        safeObservationCaptureStats.publicEventsDropped++;
    }
    safeObservationEvents.push(event);
    return fault;
}

export function safeObservationError(error, phase = 'observer', options = {}) {
    return JSON.stringify(safeObservationFault(error, phase, options));
}
