'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
const pending = id => ({ success: true, data: { operationId: id, pending: true, success: false } });
const done = (id, extra = {}) => ({ success: true, data: { operationId: id, pending: false, success: true, ...extra } });
function setup(responder = (_, request) => request.action === 'operation' ? done(request.operationId, { speed: 0, pulse: 0 }) : pending(7)) {
    const nodes = new Map(), calls = [], notices = [];
    let now = 1000;
    const element = id => {
        if (!nodes.has(id)) nodes.set(id, { value: '0', textContent: '', dataset: {}, disabled: false });
        return nodes.get(id);
    };
    const app = { _activeDeviceIdx: 0, _modbusCtrlGeneration: 1,
        _modbusDevices: [{ slaveAddress: 8, motorDecimals: 0 }] };
    const context = vm.createContext({ AppState: app, document: { getElementById: element },
        Date: { now: () => now }, Math, Number, Promise,
        console: { error() {}, warn() {} },
        setTimeout(fn, ms) { now += ms; queueMicrotask(fn); },
        Notification: Object.fromEntries(['success', 'error', 'warning'].map(level => [level, text => notices.push({ level, text })])),
        apiPostSilentPriority: async (url, request) => { calls.push({ url, ...request }); return responder(url, request, app); }
    });
    context.window = context;
    vm.runInContext(fs.readFileSync(path.join(root, 'web-src/js/utils.js'), 'utf8'), context);
    for (const file of ['modbus-control.js', 'modbus-relay-motor.js'])
        vm.runInContext(fs.readFileSync(path.join(root, 'web-src/modules/runtime/protocol', file), 'utf8'), context);
    app._appendDebugLog = app._appendDebugError = () => {};
    app._setCtrlModalOffline = (value, message) => { app._modbusCtrlOffline = value; app.offlineMessage = message; };
    return { app, calls, notices, element, context };
}
async function main() {
    for (const [handler, action] of [['motorForward', 'forward'], ['motorReverse', 'reverse'], ['motorStop', 'stop'],
        ['setMotorSpeed', 'setSpeed'], ['setMotorPulse', 'setPulse'], ['refreshMotorStatus', 'readStatus']]) {
        const h = setup();
        await h.app[handler]();
        const parameterWrite = ['setSpeed', 'setPulse'].includes(action);
        assert.equal(h.calls.length, parameterWrite ? 4 : 2, handler);
        if (parameterWrite) {
            assert.equal(h.calls[2].action, 'readStatus', 'write is followed by shared readback');
            assert.equal(h.calls[3].action, 'operation');
        }
        assert.equal(h.calls[0].action, action);
        assert.equal(h.calls[0].slaveAddress, 8);
        assert.match(h.calls[0].requestId, /^[a-z0-9-]+$/);
        assert.ok(h.calls[0].requestId.length < 64);
        assert.equal(h.calls[0].value, 0);
        assert.equal(h.calls[1].action, 'operation');
        assert.equal(h.calls[1].operationId, 7);
        assert.equal(h.notices.filter(n => n.level === 'error').length, 0);
        if (action === 'readStatus') assert.equal(h.element('motor-cur-speed').textContent, '0');
        if (['forward', 'reverse', 'stop', 'readStatus'].includes(action)) {
            assert.equal(h.element('motor-cur-direction').textContent,
                ({ forward: '正转应答', reverse: '反转应答', stop: '停止应答' })[action] || '--');
            assert.equal(h.element('modbus-ctrl-motor-run-status').textContent, action === 'readStatus' ? '就绪' : '指令已应答');
        }
    }
    const unique = setup();
    await unique.app.motorForward(); await unique.app.motorForward();
    assert.notEqual(unique.calls[0].requestId, unique.calls[2].requestId);
    const recovery = setup();
    recovery.app._modbusDevices[0].deviceType = 'motor';
    recovery.app._modbusCtrlOffline = true;
    await recovery.app.motorForward();
    assert.equal(recovery.calls.filter(call => call.action === 'forward').length, 1,
        'a previous read timeout must not silently discard a new explicit motor request');
    recovery.app._modbusDevices[0].enabled = false;
    await recovery.app.motorReverse();
    assert.equal(recovery.calls.filter(call => call.action === 'reverse').length, 0,
        'a disabled motor remains disabled');
    for (const bad of ['', '-1', '65536', 'NaN', '1.25']) {
        for (const [method, id] of [['setMotorSpeed', 'speed'], ['setMotorPulse', 'pulse']]) {
            const h = setup(); h.element('modbus-ctrl-motor-' + id).value = bad;
            await h.app[method](); assert.equal(h.calls.length, 0, bad); assert.equal(h.notices[0].level, 'error');
        }
    }
    const decimals = setup(); decimals.app._modbusDevices[0].motorDecimals = 2;
    decimals.element('modbus-ctrl-motor-speed').value = '1.23';
    await decimals.app.setMotorSpeed(); assert.equal(decimals.calls[0].value, 123);
    for (const response of [{ success: false, error: 'invalid' }, { success: true },
        { success: true, data: { pending: true } }, done(7, { success: false, error: 'TIMEOUT' }),
        done(7, { success: undefined })]) {
        const h = setup(() => response); await h.app.motorForward();
        assert.equal(h.calls.length, 1); assert.equal(h.notices[0].level, 'error');
    }
    const mismatch = setup((_, r) => r.action === 'operation' ? done(8) : pending(7));
    await mismatch.app.motorForward(); assert.equal(mismatch.notices[0].level, 'error');
    const timeout = setup(() => pending(7)); await timeout.app.motorForward();
    assert.equal(timeout.calls.filter(r => r.action === 'forward').length, 1);
    assert.equal(timeout.notices[0].level, 'error');
    let release;
    const duplicate = setup((_, r) => r.action === 'forward' ? new Promise(resolve => { release = resolve; }) : done(8));
    const first = duplicate.app.motorForward(); await duplicate.app.motorForward();
    assert.equal(duplicate.calls.length, 1); assert.equal(duplicate.notices[0].level, 'error');
    duplicate.app._modbusCtrlOffline = true; await duplicate.app.motorStop();
    assert.equal(duplicate.calls[1].action, 'stop');
    release(done(7)); await first;
    assert.equal(Object.keys(duplicate.app._protocolMotorPending).length, 0);
    for (const reject of [false, true]) {
        let finish;
        const h = setup(() => new Promise((resolve, fail) => { finish = reject ? fail : resolve; }));
        const running = h.app.refreshMotorStatus();
        h.element('motor-cur-speed').textContent = 'new-device';
        h.app._modbusCtrlGeneration++;
        finish(reject ? new Error('old transport error') : pending(7)); await running;
        assert.equal(h.calls.length, 1); assert.equal(h.element('motor-cur-speed').textContent, 'new-device');
        assert.equal(h.app._modbusCtrlOffline, undefined); assert.equal(h.notices.length, 0);
    }
    const stale = setup(() => done(7, { success: false, error: 'Motor status unavailable' }));
    stale.element('motor-cur-speed').textContent = '50'; await stale.app.refreshMotorStatus();
    assert.equal(stale.app._modbusCtrlOffline, true); assert.equal(stale.element('motor-cur-speed').textContent, '--');
    const synced = setup();
    synced.app._applyProtocolMotorState({ parametersKnown: true, speed: 29, pulse: 400, lastAcknowledgedAction: 'reverse' });
    assert.equal(synced.element('modbus-ctrl-motor-speed').value, '29');
    assert.equal(synced.element('modbus-ctrl-motor-pulse').value, '400');
    assert.equal(synced.element('motor-cur-direction').textContent, '反转应答');
    synced.element('modbus-ctrl-motor-speed').dataset.dirty = 'true';
    synced.element('modbus-ctrl-motor-speed').value = 'draft';
    synced.app._applyProtocolMotorState({ parametersKnown: true, speed: 39, pulse: 500 });
    assert.equal(synced.element('modbus-ctrl-motor-speed').value, 'draft');
    assert.equal(synced.element('motor-cur-speed').textContent, '39');
    synced.app._applyProtocolMotorState({ parametersKnown: false });
    assert.equal(synced.element('motor-cur-speed').textContent, '--');
    assert.equal(synced.element('modbus-ctrl-motor-pulse').value, '');
    assert.equal(synced.element('modbus-ctrl-motor-speed').value, 'draft');
    let poll = 0;
    const busy = setup((_, r) => {
        if (r.action !== 'operation') return pending(7);
        if (!poll++) throw Object.assign(new Error('busy'), { status: 503 });
        return done(7);
    });
    await busy.app.motorForward();
    assert.deepEqual(busy.calls.map(c => c.action), ['forward', 'operation', 'operation']);
    const missing = setup((_, r) => {
        if (r.action === 'operation') throw Object.assign(new Error('not found'), { status: 404 });
        return pending(7);
    });
    await missing.app.motorReverse();
    assert.equal(missing.calls.filter(c => c.action === 'reverse').length, 1);
    assert.match(missing.notices[0].text, /勿重复启动/);
    console.log('Protocol motor contract passed: IDs, async results, single submit, stop, precision, failure, timeout and stale views.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
