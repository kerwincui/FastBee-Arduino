'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(process.env.FASTBEE_TEST_MOTOR_SOURCE || path.join(__dirname, '..'));
const drain = () => new Promise(setImmediate);
function fixture(protocol) {
    const nodes = new Map(), calls = [], timers = [];
    const element = id => {
        if (!nodes.has(id)) nodes.set(id, { value: '', dataset: {}, style: {}, isConnected: true, textContent: '' });
        return nodes.get(id);
    };
    let next = { success: true, data: { parametersKnown: true, speed: 71, pulse: 230, lastAcknowledgedAction: 'reverse' } };
    const app = { _dcLifecycleGeneration: 1, _dcAutoRefreshTimers: {}, _modbusCtrlGeneration: 1,
        _activeDeviceIdx: 0, _modbusDevices: [{ deviceType: 'motor', slaveAddress: 9 }, { deviceType: 'relay', slaveAddress: 6 }] };
    const document = { getElementById: element, hidden: false };
    const context = vm.createContext({ AppState: app, document, console, Math, Date, Number, Promise,
        setTimeout(fn, ms) { timers.push({ fn, ms }); return timers.length; }, clearTimeout() {},
        apiGetSilentFresh: async (url, params) => { calls.push({ url, params }); return typeof next === 'function' ? next() : next; },
        apiPostSilentPriority() { throw new Error('Background sync must not submit a command or bus read'); }
    });
    const file = protocol ? 'protocol/modbus-relay-motor.js' : 'device-control/modbus-ctrl.js';
    vm.runInContext(fs.readFileSync(path.join(root, 'web-src/modules/runtime', file), 'utf8'), context);
    app._getMotorDevParams = () => ({ slaveAddress: 9, motorDecimals: 1 });
    app._dcGetMotorParams = () => ({ slaveAddress: 9, motorDecimals: 1 });
    app._dcIsLifecycleActive = generation => generation === app._dcLifecycleGeneration;
    app._markCtrlModalOnline = () => {};
    return { app, calls, timers, document, element, setNext(value) { next = value; },
        start() { protocol ? app._startProtocolMotorSync() : app._dcStartMotorSync(); },
        close() { if (protocol) app._modbusCtrlGeneration++; else app._dcLifecycleGeneration++; },
        speed: protocol ? 'modbus-ctrl-motor-speed' : 'dc-motor-speed-in-0',
        direction: protocol ? 'motor-cur-direction' : 'dc-motor-dir-0' };
}
(async () => {
    for (const protocol of [false, true]) {
        const h = fixture(protocol);
        h.start(); await drain();
        assert.equal(h.calls.length, 1);
        assert.equal(h.calls[0].url, '/api/modbus/motor/state');
        assert.equal(h.calls[0].params.slaveAddress, 9, 'no hardcoded slave8');
        assert.equal(h.element(h.speed).value, '7.1');
        assert.equal(h.element(h.direction).textContent, '反转应答');
        assert.equal(h.timers.length, 1, 'one serial timer for the whole view');
        h.element(h.speed).dataset.dirty = 'true'; h.element(h.speed).value = '9.0';
        h.setNext({ success: true, data: { parametersKnown: true, speed: 88, pulse: 330, lastAcknowledgedAction: 'stop' } });
        await h.timers.shift().fn(); await drain();
        assert.equal(h.element(h.speed).value, '9.0');
        assert.equal(h.element(h.direction).textContent, '停止应答');
        h.document.hidden = true;
        const beforeHidden = h.calls.length;
        await h.timers.shift().fn(); await drain();
        assert.equal(h.calls.length, beforeHidden, 'hidden tab performs no requests');
        h.document.hidden = false;
        h.setNext(() => { throw new Error('offline'); });
        await h.timers.shift().fn(); await drain();
        assert.equal(h.timers[0].ms, 10000, 'back off without polling storms');
        assert.equal(h.element(h.speed).value, '9.0', 'transport failure preserves draft');
        h.close(); await h.timers.shift().fn(); await drain();
        assert.equal(h.timers.length, 0, 'closed view stops rescheduling');

        const delayed = fixture(protocol);
        let release;
        delayed.setNext(() => new Promise(resolve => { release = resolve; }));
        delayed.start(); await drain(); delayed.close();
        delayed.element(delayed.speed).value = 'new-view';
        release({ success: true, data: { speed: 99, pulse: 100, parametersKnown: true } });
        await drain();
        assert.equal(delayed.element(delayed.speed).value, 'new-view');
        assert.equal(delayed.timers.length, 0);

        const pending = fixture(protocol);
        let finish;
        pending.setNext(() => new Promise(resolve => { finish = resolve; }));
        pending.start(); await drain();
        if (protocol) { pending.app._protocolMotorSequence = 1; pending.app._protocolMotorAcknowledged = 'stop'; }
        else { pending.app._dcMotorSequence = 1; pending.app._dcMotorAcknowledgements = { 0: 'stop' }; }
        pending.element(pending.direction).textContent = '停止应答';
        finish({ success: true, data: { lastAcknowledgedAction: 'forward' } }); await drain();
        assert.equal(pending.element(pending.direction).textContent, '停止应答', 'old state cannot overwrite a newer command');
        pending.close(); await pending.timers.shift().fn();
    }
    console.log('Motor cached-state sync passed: both views, generic address, scaling, drafts, lifecycle, hidden tabs, backoff and late responses; zero control POSTs.');
})().catch(error => { console.error(error); process.exitCode = 1; });
