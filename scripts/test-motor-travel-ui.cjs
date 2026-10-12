'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../web-src/modules/runtime/protocol/modbus-relay-motor.js'), 'utf8');
const nodes = new Map(), posts = [], messages = [];
const element = id => {
    if (!nodes.has(id)) nodes.set(id, { value: '', checked: false, dataset: {}, style: {}, isConnected: true, textContent: '' });
    return nodes.get(id);
};
const app = { _activeDeviceIdx: 0, _modbusCtrlGeneration: 1, _modbusDevices: [{ slaveAddress: 19 }] };
const context = vm.createContext({ AppState: app, document: { getElementById: element }, console,
    Notification: { error: value => messages.push(value), success: value => messages.push(value) },
    apiPostPriority: async (url, params) => { posts.push({ url, params }); return { success: true }; }
});
context.window = context;
vm.runInContext(fs.readFileSync(path.join(__dirname, '../web-src/js/utils.js'), 'utf8'), context);
vm.runInContext(source, context);
const ready = { motorRef: 'motor.19.test', travelToken: 'boot-1', softLimitEnabled: true,
    referenceKnown: false, stationaryConfirmed: false, parametersKnown: true, pulse: 20, speed: 10 };
function fresh(changes = {}) { app._applyProtocolMotorState(Object.assign({}, ready, changes)); }
function onsite(value) { element('motor-travel-stationary').checked = true; element('motor-travel-reference').value = value; }
(async () => {
    fresh({ softLimitEnabled: false }); onsite('10'); await app.confirmMotorReference(); assert.equal(posts.length, 0);
    fresh(); assert.match(element('motor-travel-status').textContent, /参考位置未确认/);
    element('motor-travel-stationary').checked = false;
    await app.confirmMotorReference(); assert.equal(posts.length, 0);
    for (const value of ['', '1.5', '80mm', 'NaN', 'Infinity', '2147483648', '-2147483649']) {
        onsite(value); await app.confirmMotorReference(); assert.equal(posts.length, 0);
    }
    onsite('-79'); await app.confirmMotorReference(); assert.equal(posts.length, 1);
    assert.equal(posts[0].url, '/api/modbus/motor/control');
    assert.equal(posts[0].params.action, 'confirmReference');
    assert.equal(posts[0].params.motorRef, 'motor.19.test');
    assert.equal(posts[0].params.position, '-79');
    assert.equal(posts[0].params.stationaryConfirmed, 'true');
    assert.equal(element('motor-travel-stationary').checked, false);
    onsite('-79'); await app.confirmMotorReference(); assert.equal(posts.length, 1, 'double-click cannot replay confirmation');
    fresh({ travelToken: 'boot-2', referenceKnown: true });
    assert.equal(element('motor-travel-stationary').checked, false);
    onsite(''); await app.confirmMotorStationary(); assert.equal(posts.length, 2);
    assert.equal(posts[1].params.action, 'confirmStationary');
    assert.equal(posts[1].params.position, undefined, 'stationary confirmation cannot assert a coordinate');
    fresh({ travelToken: 'boot-3' }); onsite('40'); app._modbusCtrlGeneration++;
    await app.confirmMotorReference(); assert.equal(posts.length, 2, 'closed/reopened modal cannot reuse state');
    fresh({ travelToken: 'boot-4' }); onsite('40'); app._activeDeviceIdx=1;
    await app.confirmMotorReference(); assert.equal(posts.length, 2, 'another device cannot inherit reference');
    app._activeDeviceIdx=0; fresh({ travelToken: 'boot-5', pending: true }); onsite('40');
    await app.confirmMotorReference(); assert.equal(posts.length, 2, 'pending command blocks confirmation');
    assert(posts.every(post => ['confirmReference', 'confirmStationary'].includes(post.params.action)));
    console.log('Travel UI passed: unconfigured, unconfirmed, invalid input, double click, stale modal, device switch, pending; no motor commands.');
})().catch(error => { console.error(error); process.exitCode = 1; });
