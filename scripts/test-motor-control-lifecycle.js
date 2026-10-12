'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const nodes = new Map();
let content = new EventTarget();
nodes.set('dc-content', content);
const app = { onPageHide() {} };
const document = new EventTarget();
document.getElementById = id => nodes.get(id) || null;
let calls = [];
let complete;
const ctx = vm.createContext({ AppState: app, document, AbortController, setTimeout, clearTimeout,
    window: {}, Notification: { success() {}, error() {}, warning() {} },
    apiPostSilentPriority: async (url, body) => {
        calls.push(body);
        if (body.action === 'operation') return new Promise(resolve => { complete = resolve; });
        return { success: true, data: { operationId: calls.length, pending: true } };
    }
});
vm.runInContext(fs.readFileSync(path.join(root, 'web-src/js/utils.js'), 'utf8'), ctx);
for (const file of ['core.js', 'modbus-ctrl.js', 'modbus-ops.js']) {
    vm.runInContext(fs.readFileSync(path.join(root, 'web-src/modules/runtime/device-control', file), 'utf8'), ctx);
}
for (const name of ['_dcCancelInit', '_dcStopAllAutoRefresh', '_setDeviceControlRefreshLoading', '_dcClearRuntimeState']) app[name] = () => {};
let actions = 0;
const motorAction = app._dcMotorAction;
app._dcMotorAction = () => { actions++; };
function click(target) {
    const event = new Event('click');
    Object.defineProperty(event, 'target', { value: { closest: selector => selector === '.dc-motor-action' ? {
        getAttribute: key => key === 'data-dev' ? '0' : 'forward'
    } : null } });
    target.dispatchEvent(event);
}
for (let i = 0; i < 20; i++) {
    app.setupDeviceControlEvents();
    app.setupDeviceControlEvents();
    click(content);
    assert.equal(actions, i + 1, 'one action after repeated setup/destroy');
    app.destroyDeviceControlPage();
    click(content);
    assert.equal(actions, i + 1, 'destroy detaches retained container');
}
app.setupDeviceControlEvents();
const old = content;
content = new EventTarget(); nodes.set('dc-content', content);
app.setupDeviceControlEvents(); click(old); click(content);
assert.equal(actions, 21, 'replacing DOM detaches the old target');
app._dcMotorAction = motorAction;
app._dcGetMotorParams = () => ({ slaveAddress: 3, motorDecimals: 0 });
app._dcIsLifecycleActive = generation => generation === app._dcLifecycleGeneration;
app._dcDelay = async () => true;
(async () => {
    const first = app._dcMotorRequest(0, 'forward');
    await assert.rejects(app._dcMotorRequest(0, 'forward'), /正在发送/);
    await new Promise(setImmediate);
    assert.equal(calls.filter(c => c.action === 'forward').length, 1);
    complete({ success: true, data: { success: true, pending: false, motionKnown: false } });
    await first;
    assert.equal(Object.keys(app._dcMotorPending).length, 0);
    app._dcDelay = async () => false;
    await assert.rejects(app._dcMotorRequest(0, 'reverse'), /页面已关闭/);
    assert.equal(calls.filter(c => c.action === 'reverse').length, 1);
    assert.equal(calls.filter(c => c.action === 'operation').length, 1, 'page exit stops polling');
    app._dcMotorPending = { 3: 'previous' };
    await assert.rejects(app._dcMotorRequest(0, 'stop'), /页面已关闭/);
    assert.equal(calls.filter(c => c.action === 'stop').length, 1, 'stop bypasses pending movement');
    for (const id of ['dc-motor-dir-0', 'dc-motor-run-0']) nodes.set(id, {});
    app._modbusDevices = [{}];
    app._dcUpdateMotorUIFromSSE(0, 'forward');
    assert.equal(nodes.get('dc-motor-run-0').textContent, '运动未确认');
    app._dcUpdateMotorRunUI(0, { lastAcknowledgedAction: 'reverse', motionKnown: false }, 0);
    assert.equal(nodes.get('dc-motor-dir-0').textContent, '反转应答');
    app._dcUpdateMotorUIFromSSE(0, 'spd:10');
    assert.equal(nodes.get('dc-motor-dir-0').textContent, '反转应答', 'parameter reports do not erase acknowledged direction');
    const request = app._dcMotorRequest;
    let parameterValue, resolveParameter, launchCalls = 0, readbacks = 0;
    app._dcMotorRequest = (devIdx, action, value) => {
        if (action === 'setSpeed') {
            parameterValue = value;
            return new Promise(resolve => { resolveParameter = resolve; });
        }
        if (action === 'readStatus') {
            readbacks++;
            return Promise.resolve({ success: true, data: { parametersKnown: true, speed: 29, pulse: 100 } });
        }
        launchCalls++;
        return Promise.resolve({ success: true, data: {} });
    };
    app._dcGetMotorParams = () => ({ slaveAddress: 3, motorDecimals: 2 });
    const speedInput = { value: '0.29', dataset: { dirty: 'true' } };
    nodes.set('dc-motor-speed-in-0', speedInput);
    const setting = app._dcMotorSet(0, 'speed');
    assert.equal(parameterValue, 29, 'decimal scaling accepts floating point roundoff');
    speedInput.value = '0.30';
    resolveParameter({ success: true, data: {} }); await setting;
    assert.equal(readbacks, 1, 'acknowledged parameter is read back through the shared service');
    assert.equal(speedInput.dataset.dirty, 'true', 'editing during a write retains the newer draft');
    await app._dcMotorAction(0, 'forward');
    assert.equal(launchCalls, 0, 'unsent parameter draft blocks launch');
    await app._dcMotorAction(0, 'stop');
    assert.equal(launchCalls, 1, 'unsent draft never blocks stop');
    speedInput.value = '0.291'; parameterValue = null;
    await app._dcMotorSet(0, 'speed');
    assert.equal(parameterValue, null, 'fractional raw register values are rejected');
    speedInput.value = '0.31';
    const nextSetting = app._dcMotorSet(0, 'speed');
    resolveParameter({ success: true, data: {} }); await nextSetting;
    assert.equal(readbacks, 2);
    assert.equal(speedInput.dataset.dirty, 'false', 'unchanged acknowledged draft becomes clean');
    app._dcUpdateMotorRunUI(0, { speed: 55 }, 2);
    assert.equal(speedInput.value, '0.55', 'readback refreshes a clean inactive input');
    speedInput.dataset.dirty = 'true';
    app._dcUpdateMotorRunUI(0, { speed: 66 }, 2);
    assert.equal(speedInput.value, '0.55', 'readback preserves an unsent input');
    app._dcMotorRequest = request;
    nodes.set('dc-motor-speed-0', { textContent: '0.55' });
    nodes.set('dc-motor-pulse-0', { textContent: '1600' });
    app._dcUpdateMotorRunUI(0, { parametersKnown: false }, 2);
    assert.equal(nodes.get('dc-motor-speed-0').textContent, '--', 'failed read cannot present stale speed as live');
    assert.equal(nodes.get('dc-motor-pulse-0').textContent, '--', 'failed read cannot present stale pulse as live');
    assert.equal(speedInput.value, '0.55', 'failed read preserves unsent user draft');
    app._dcMotorPending = {};
    ctx.apiPostSilentPriority = async () => ({ success: true, data: { speed: 100 } });
    assert.equal((await app._dcMotorRequest(0, 'readStatus')).data.speed, 100, 'legacy firmware response remains readable');
    ctx.apiPostSilentPriority = async () => ({ success: true, data: { success: false, operationId: 7, error: 'lost reply' } });
    await assert.rejects(app._dcMotorRequest(0, 'forward'), /lost reply/);
    app._dcDelay = async () => true;
    const retried = [];
    ctx.apiPostSilentPriority = async (_, body) => {
        retried.push(body.action);
        if (retried.length === 1) return { success: true, data: { operationId: 91, pending: true } };
        if (retried.length === 2) throw Object.assign(new Error('busy'), { status: 503 });
        return { success: true, data: { operationId: 91, pending: false, success: true } };
    };
    await app._dcMotorRequest(0, 'forward');
    assert.deepEqual(retried, ['forward', 'operation', 'operation'], 'only result lookup can retry');
    retried.length = 0;
    ctx.apiPostSilentPriority = async (_, body) => {
        retried.push(body.action);
        if (body.action === 'operation') throw Object.assign(new Error('not found'), { status: 404 });
        return { success: true, data: { operationId: 92, pending: true } };
    };
    await assert.rejects(app._dcMotorRequest(0, 'reverse'), /勿重复启动/);
    assert.deepEqual(retried, ['reverse', 'operation']);
    console.log('Motor lifecycle regression passed: 20 remounts, duplicate submit, stop, exit, truthful SSE, scaling, dirty drafts, readback and legacy responses.');
})().catch(error => { console.error(error); process.exitCode = 1; });
