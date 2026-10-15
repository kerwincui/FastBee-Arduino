import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { safeObservationError, safeObservationFault, safeObservationEvents,
    safeObservationCaptureStats } from '../../scripts/lib/safe-observer-error.mjs';

const outIndex = process.argv.indexOf('--out');
const output = outIndex >= 0 ? path.resolve(process.argv[outIndex + 1]) : fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-safe-observer-'));
if (outIndex >= 0) fs.mkdirSync(output);
const privateDirectory = path.join(output, 'synthetic-private');
const secret = 'synthetic-cookie-secret-used-only-in-offline-test';
const password = 'synthetic-password-different-from-cookie';
const error = new Error(`apiRequestContext.post: Timeout\nCall log:\n - Cookie: token=${secret}\n - password=${password}`);
error.name = 'TimeoutError';
error.cause = { code: 'UND_ERR_CONNECT_TIMEOUT', message: secret };
const options = { privateDirectory };
const checks = [];
function check(name, action) { action(); checks.push(name); }

check('full Cookie call log is private and public fields are safe', () => {
    const fault = safeObservationFault(error, 'cleanup', options);
    assert.deepEqual(fault, { errorType: 'TimeoutError', phase: 'cleanup', safeCode: 'OBSERVER_ERROR', causeCode: 'UND_ERR_CONNECT_TIMEOUT' });
    assert(!JSON.stringify(fault).includes(secret));
    const event = safeObservationEvents.at(-1);
    const raw = fs.readFileSync(event.privateEvidence);
    assert(raw.toString().includes(secret) && raw.toString().includes(password));
    assert.equal(createHash('sha256').update(raw).digest('hex'), event.privateSha256);
});
check('JSON string public error still represents failure', () => {
    const text = safeObservationError(error, 'fatal', options);
    assert(text); assert.equal(JSON.parse(text).safeCode, 'OBSERVER_ERROR');
    assert(!text.includes(secret) && !text.includes(password));
});
check('unknown error names and cause codes cannot carry secrets', () => {
    const fault = safeObservationFault({ name: secret, message: secret, code: secret }, secret, options);
    assert.deepEqual(fault, { errorType: 'Error', phase: 'observer', safeCode: 'OBSERVER_ERROR' });
});
check('initial console 401 keeps its original classification without raw text', () => {
    const publicText = safeObservationError(`Failed resource: status of 401 Cookie:${secret}`, 'console', options);
    assert(/status of 401|401 [(]Unauthorized[)]/i.test(publicText));
    assert(!publicText.includes(secret));
});
check('401 outside original first 300 characters remains a failure', () => {
    const publicText = safeObservationError('x'.repeat(301) + '401 (Unauthorized)', 'console', options);
    assert(!/status of 401|401 [(]Unauthorized[)]/i.test(publicText));
});
check('private write failure never falls back to publishing original', () => {
    const blockedDirectory = path.join(output, 'private-is-file'); fs.writeFileSync(blockedDirectory, 'synthetic');
    const fault = safeObservationFault(error, 'cleanup', { privateDirectory: blockedDirectory });
    assert.equal(fault.safeCode, 'OBSERVER_ERROR');
    assert(safeObservationEvents.at(-1).privateCaptureFailed);
    assert(!JSON.stringify(fault).includes(secret));
});

const AsyncFunction = Object.getPrototypeOf(async function() {}).constructor;
async function executeChecker(filename, kind) {
    const source = fs.readFileSync(filename, 'utf8').replace(/^import .*;\r?\n/gm, '')
        .replaceAll('import.meta.url', 'checkerModuleUrl');
    const checkerOutput = path.join(output, kind);
    const logs = [];
    const page = {
        setDefaultTimeout() {}, route: async () => {},
        on(name, callback) {
            if (name === 'pageerror') callback(error);
            if (kind === 'menus' && name === 'response') callback({ url: () => 'http://192.0.2.15/api/features/status', status: () => 200, json: async () => { throw error; } });
            if (kind === 'menus' && name === 'requestfailed') callback({
                failure: () => ({ errorText: `Cookie: ${secret}` }), url: () => 'http://192.0.2.15/api/health', method: () => 'GET'
            });
        },
        goto: async () => { throw error; }, screenshot: async () => {}, locator: () => ({})
    };
    const context = { newPage: async () => page, route: async () => {} };
    const browser = { newContext: async () => context, close: async () => {} };
    const fakeRequire = () => ({ chromium: { launch: async () => browser }, expect: { configure: () => () => ({}) } });
    const checkerProcess = {
        argv: kind === 'menus' ? ['node', filename, 'http://192.0.2.15', checkerOutput] : ['node', filename, checkerOutput],
        env: { FASTBEE_BASE_URL: 'http://192.0.2.15', FASTBEE_PASSWORD: password, FASTBEE_EXPECTED_MAC: '14:C1:9F:C1:52:D8' }
    };
    const bindings = { assert, fs, path, checkerModuleUrl: filename, createRequire: () => fakeRequire,
        isReadOnlyBatch: () => false, isReadOnlyMotorStatus: () => false, isIntentionalHealthCancellation: () => false,
        isNavigatedAbortedRead: () => false,
        waitForMainShellStyles: async () => {},
        safeObservationError: (fault, phase) => safeObservationError(fault, phase, options),
        safeObservationEvents, safeObservationCaptureStats, process: checkerProcess,
        console: { log: (...args) => logs.push(args.join(' ')) } };
    await new AsyncFunction(...Object.keys(bindings), source)(...Object.values(bindings));
    const publicBytes = fs.readFileSync(path.join(checkerOutput, 'report.json'), 'utf8');
    const report = JSON.parse(publicBytes);
    assert.equal(report.passed, false); assert.equal(checkerProcess.exitCode, 1);
    assert(!publicBytes.includes(secret) && !publicBytes.includes(password));
    assert(!logs.join('\n').includes(secret) && !logs.join('\n').includes(password));
    assert.equal(JSON.parse(report.error).errorType, 'TimeoutError');
    assert.equal(JSON.parse(report.pageErrors[0]).safeCode, 'OBSERVER_ERROR');
    return report;
}

const menus = await executeChecker('scripts/test-web-device-menus.mjs', 'menus');
assert.equal(JSON.parse(menus.apiIntegrity[0].error).safeCode, 'OBSERVER_ERROR');
assert.equal(menus.requestFailures.length, 1);
assert(!menus.requestFailures[0].reason.includes(secret));
checks.push('actual menu checker fatal, pageerror, API evidence and request failure stay FAIL without public Cookie');
await executeChecker('scripts/test-com15-monitor-device.mjs', 'dashboard');
checks.push('actual dashboard checker and terminal stay FAIL without public Cookie');

const { collectWorkerErrors, finalizeMonitorRun } = await import('../browser/fleet-stability-monitor.mjs');
const workerErrors = collectWorkerErrors([{ status: 'rejected', reason: error }, { status: 'fulfilled' }],
    [{ label: 'synthetic-device' }, { label: 'healthy-device' }]);
assert.equal(workerErrors.length, 1);
assert(!workerErrors[0].includes(secret) && !workerErrors[0].includes(password));
assert.equal(JSON.parse(workerErrors[0].slice('synthetic-device: worker '.length)).errorType, 'TimeoutError');
assert.deepEqual(finalizeMonitorRun({ stopRequested: false, workerErrors }), { finished: false, exitCode: 2 });
assert.deepEqual(finalizeMonitorRun({ stopRequested: false, workerErrors: [] }), { finished: true, exitCode: 0 });
assert(fs.readFileSync(safeObservationEvents.at(-1).privateEvidence, 'utf8').includes(secret));
checks.push('actual fleet rejected worker Cookie error remains private and exit two');

check('public audit metadata is bounded while all originals persist', () => {
    const before = fs.readdirSync(privateDirectory).length;
    for (let index = 0; index < 130; index++) safeObservationFault(error, 'observer', options);
    assert.equal(safeObservationEvents.length, 128);
    assert.equal(fs.readdirSync(privateDirectory).length - before, 130);
    assert(safeObservationCaptureStats.publicEventsDropped > 0);
    assert(!JSON.stringify(safeObservationEvents).includes(secret));
});
fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify({ status: 'PASS', devicesAccessed: false, checks }, null, 2), { flag: 'wx' });
console.log(JSON.stringify({ status: 'PASS', devicesAccessed: false, cases: checks.length }));
