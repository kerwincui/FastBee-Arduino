'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { minifyJS } = require('./minify-js');
const source = fs.readFileSync('web-src/modules/runtime/protocol/modbus-config.js', 'utf8');
const html = fs.readFileSync('web-src/pages/fragments/protocol-modbus-rtu.html', 'utf8');
const removed = ['profile', 'finite', 'position-tracking', 'startup-reference', 'startup-position', 'cur-pos', 'move-step', 'last-pulse'];
for (const field of removed) assert(!html.includes('id="mdev-edit-motor-' + field + '"'));
assert(!html.includes('mdev-edit-travel-forwardClearanceMm'));
for (const compact of [false, true]) {
    const fields = new Map(), warnings = [];
    const field = id => {
        if (removed.some(name => id === 'mdev-edit-motor-' + name)) return null;
        if (!fields.has(id)) fields.set(id, { value: '', checked: false, open: false,
            classList: { remove() {} }, scrollIntoView() {} });
        return fields.get(id);
    };
    const app = { guardDeveloperModeAction: () => true, ensureModal: () => ({}),
        showModal() {}, hideModal() {}, toggleVisible(node, visible) { node.visible = visible; } };
    vm.runInNewContext(compact ? minifyJS(source) : source, { AppState: app, window: {}, console,
        document: { getElementById: field }, Notification: { warning: message => warnings.push(message) } });
    app._renderAllDevices = () => {};
    app._modbusDevices = [];
    app._openEditModal(-1); field('mdev-edit-type').value = 'motor'; field('mdev-edit-addr').value = '17';
    assert.equal(field('mdev-edit-motor-profile'), null);
    assert.equal(app._onMotorProtocolPreset, undefined);
    assert.equal(field('mdev-edit-motor-limits').checked, false);
    assert.equal(field('mdev-edit-motor-advanced').open, false);
    assert.equal(field('mdev-edit-travel-section').open, false);
    assert.equal(app._saveEditModal(), true);
    let saved = app._modbusDevices[0];
    assert.equal(saved.motorProtocol.finitePulseMove, true);
    assert.equal(saved.motorMinPosition, 0); assert.equal(saved.motorMaxPosition, 0);
    assert.equal(saved.motorProtocol.startupPosition, undefined);
    assert.equal(saved.slaveAddress, 17); assert.equal(saved.sensorId, 'motor_17');
    assert.deepEqual(Array.from(saved.motorRegs), [0, 1, 2, 5, 7]);
    // Optional limits are independent of basic motion and persist on reopening.
    app._openEditModal(0); field('mdev-edit-motor-limits').checked = true; app._onMotorLimitToggle();
    field('mdev-edit-motor-min-pos').value = '-90'; field('mdev-edit-motor-max-pos').value = '180';
    assert.equal(app._saveEditModal(), true);
    saved = app._modbusDevices[0];
    assert.equal(saved.motorProtocol.positionTracking, 'commanded'); assert.equal(saved.motorProtocol.startupPosition, 0);
    app._openEditModal(0); assert.equal(field('mdev-edit-motor-limits').checked, true);
    field('mdev-edit-motor-limits').checked = false;
    field('mdev-edit-motor-min-pos').value = 'unused-invalid';
    field('mdev-edit-motor-max-pos').value = 'unused-invalid';
    assert.equal(app._saveEditModal(), true, 'Disabled advanced fields cannot block a basic save');
    assert.equal(saved.motorMinPosition, 0); assert.equal(saved.motorMaxPosition, 0);
    assert.equal(saved.motorProtocol.startupPosition, undefined);
    // Existing custom devices retain their protocol and identity.
    saved.motorRegs = [300, 300, 300, 301, 302];
    Object.assign(saved.motorProtocol, { readFunction: 4, writeFunction: 16, speedReadRegister: 401,
        pulseReadRegister: 402, forwardValue: 17, reverseValue: 34, stopValue: 0 });
    app._openEditModal(0); assert.equal(app._saveEditModal(), true);
    assert.equal(saved.motorProtocol.stopValue, 0); assert.equal(saved.motorProtocol.writeFunction, 16);
    assert.equal(saved.motorProtocol.pulseReadRegister, 402); assert.equal(saved.slaveAddress, 17);
    // Removing editor fields must preserve an existing bounded custom profile
    // on ordinary name edits, without touching another device in the list.
    const relay = { name: 'Existing relay', deviceType: 'relay', slaveAddress: 21,
        coilBase: 42, channelCount: 4, ncMode: true, enabled: false };
    app._modbusDevices.push(relay);
    const relayBefore = JSON.stringify(relay);
    const redundant = { motorCurrentPosition: 99, motorMoveStep: 25, motorLastPulse: 800,
        motorTravelDraft: { stale: true }, coilBase: 42, ncMode: true, batchRegister: 12,
        batchRegType: 4, pwmRegBase: 31, pwmResolution: 12, pidAddrs: [7, 8], pidDecimals: 3 };
    saved.motorMinPosition = -400; saved.motorMaxPosition = 800;
    for (const tracking of ['conservative', 'commanded']) {
        Object.assign(saved, redundant);
        Object.assign(saved.motorProtocol, { positionTracking: tracking, startupPosition: -50 });
        const protocolBefore = JSON.stringify(saved.motorProtocol);
        const identityBefore = saved.sensorId;
        app._openEditModal(0);
        field('mdev-edit-name').value = 'Renamed motor';
        assert.equal(app._saveEditModal(), true);
        assert.equal(saved.name, 'Renamed motor');
        assert.equal(saved.sensorId, identityBefore);
        assert.equal(saved.motorMinPosition, -400); assert.equal(saved.motorMaxPosition, 800);
        assert.equal(JSON.stringify(saved.motorProtocol), protocolBefore);
        for (const key of Object.keys(redundant)) assert(!Object.hasOwn(saved, key), key);
        assert.equal(JSON.stringify(app._modbusDevices[1]), relayBefore);
    }
    // Removing an acknowledgement checkbox must not claim a non-finite profile
    // supports bounded motion merely because the user edited its display name.
    saved.motorProtocol.finitePulseMove = false;
    app._openEditModal(0); assert.equal(app._saveEditModal(), true);
    assert.equal(saved.motorProtocol.finitePulseMove, false);
    // A new motor must not inherit the previously edited device's custom
    // registers, optional travel limits or explicit non-finite setting.
    app._openEditModal(-1);
    field('mdev-edit-type').value = 'motor'; field('mdev-edit-addr').value = '18';
    assert.equal(field('mdev-edit-motor-limits').checked, false);
    assert.equal(app._saveEditModal(), true);
    const fresh = app._modbusDevices[2];
    assert.equal(fresh.sensorId, 'motor_18');
    assert.deepEqual(Array.from(fresh.motorRegs), [0, 1, 2, 5, 7]);
    assert.equal(fresh.motorProtocol.finitePulseMove, true);
    assert.equal(fresh.motorMinPosition, 0); assert.equal(fresh.motorMaxPosition, 0);
    assert.equal(saved.motorProtocol.finitePulseMove, false);
    app._openEditModal(0);
    assert.equal(app._saveEditModal(), true); assert.equal(saved.motorProtocol.finitePulseMove, false);
    assert.equal(warnings.length, 0);
}
console.log('PASS: source/minified motor defaults, optional limits, profile preservation, stale-field cleanup, device isolation');
