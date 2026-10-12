'use strict';
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const { minifyJS } = require('./minify-js');
const source = fs.readFileSync('web-src/modules/runtime/protocol/modbus-config.js', 'utf8');
(async () => {
    for (const compact of [false, true]) {
        const fields = new Map(), errors = [];
        const field = id => {
            if (!fields.has(id)) fields.set(id, { value: '', checked: false, inert: false,
                classList: { remove() {} }, scrollIntoView() {} });
            return fields.get(id);
        };
        const app = { guardDeveloperModeAction: () => true, ensureModal: () => ({}),
            showModal() {}, hideModal() {}, toggleVisible() {},
            _protocolConfig: { modbusRtu: { motorTravelDraftSupported: false } } };
        vm.runInNewContext(compact ? minifyJS(source) : source, { AppState: app, window: {}, console,
            document: { getElementById: field }, Notification: {
                warning: text => errors.push(text), error: text => errors.push(text) } });
        app._renderAllDevices = () => {};
        app._modbusDevices = [{ deviceType: 'motor', slaveAddress: 17, motorRegs: [0, 1, 2, 5, 7],
            motorMinPosition: 0, motorMaxPosition: 0, motorCurrentPosition: 0 }];
        app._openEditModal(0);
        field('mdev-edit-motor-limits').checked = true;
        field('mdev-edit-motor-min-pos').value = '-400';
        field('mdev-edit-motor-max-pos').value = '800';
        let complete, submissions = 0;
        app.saveProtocolConfig = async form => {
            assert.equal(form, 'modbus-rtu-form');
            assert.equal(app._modbusDevices[0].motorMinPosition, -400);
            assert.equal(app._modbusDevices[0].motorMaxPosition, 800);
            submissions++;
            return new Promise(resolve => { complete = resolve; });
        };
        const failed = app._saveEditModalToDevice();
        assert.equal(await app._saveEditModalToDevice(), false, 'duplicate save is not sent');
        assert.equal(submissions, 1);
        assert.equal(app._editingDeviceIdx, 0, 'keep form until device result');
        complete(false);
        assert.equal(await failed, false);
        assert.equal(app._editingDeviceIdx, 0);
        assert.equal(app._modbusDevices[0].motorMaxPosition, 0);
        assert.equal(field('mdev-edit-motor-max-pos').value, '800', 'failed save retains input');
        assert.equal(field('modbus-device-edit-modal').inert, false);
        const success = app._saveEditModalToDevice();
        complete(true);
        assert.equal(await success, true);
        assert.equal(app._editingDeviceIdx, -1);
        assert.equal(app._modbusDevices[0].motorMinPosition, -400);
        assert.equal(app._modbusDevices[0].motorMaxPosition, 800);
        assert.equal(errors.length, 0);
        app._openEditModal(0);
        app.saveProtocolConfig = async () => true;
        assert.equal(await app._saveEditModalToDevice(), true);
        assert.equal(app._modbusDevices[0].motorProtocol.startupPosition, 0);
        app._openEditModal(0);
        assert.equal(field('mdev-edit-motor-limits').checked, true);
        field('mdev-edit-motor-min-pos').value = '801';
        assert.equal(await app._saveEditModalToDevice(), false);
        assert.equal(app._modbusDevices[0].motorProtocol.startupPosition, 0);
        field('mdev-edit-motor-limits').checked = false;
        assert.equal(await app._saveEditModalToDevice(), true);
        assert.equal(app._modbusDevices[0].motorProtocol.startupPosition, undefined);
        assert.equal(app._modbusDevices[0].motorMinPosition, 0);
        assert.equal(app._modbusDevices[0].motorMaxPosition, 0);
        assert.equal(app._modbusDevices[0].motorMoveStep, undefined);
    }
    console.log('PASS: modal submits once, waits for device, preserves failed edits and closes after confirmed save');
})().catch(error => { console.error(error); process.exitCode = 1; });
