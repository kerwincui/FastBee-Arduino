'use strict';
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const { minifyJS } = require('./minify-js');
const source = fs.readFileSync('web-src/modules/runtime/protocol/modbus-control.js', 'utf8');
for (const minified of [false, true]) {
    const nodes = new Map(), classes = new Set();
    const controls = ['motorForward', 'motorReverse', 'motorStop', 'refreshMotorStatus', 'setMotorSpeed'].map(action => ({
        disabled: false, dataset: {}, getAttribute: name => name === 'data-action' ? action : null
    }));
    const body = { parentNode: { insertBefore(node) { nodes.set(node.id, node); } } };
    nodes.set('modbus-device-ctrl-modal', { classList: { toggle(name, enabled) { enabled ? classes.add(name) : classes.delete(name); } },
        querySelector: () => body, querySelectorAll: () => controls });
    const app = { _activeDeviceIdx: 0, _modbusDevices: [{ deviceType: 'motor', enabled: true }] };
    vm.runInNewContext(minified ? minifyJS(source) : source, { AppState: app, document: {
        getElementById: id => nodes.get(id), createElement: () => ({ setAttribute(name, value) { this[name] = value; },
            remove() { nodes.delete(this.id); } }) } });
    app._setCtrlModalOffline(true, 'speed s8 TIMEOUT');
    assert.equal(nodes.get('modbus-ctrl-offline-banner').textContent, 'speed s8 TIMEOUT');
    assert.equal(nodes.get('modbus-ctrl-offline-banner').role, 'status');
    assert.equal(app._isCtrlModalOffline(), true, 'communication failure remains visible');
    assert.equal(app._ctrlModalCommandsBlocked(), false);
    assert.equal(classes.has('modbus-ctrl-offline'), false, 'CSS must not prevent a new explicit request');
    assert(controls.every(control => !control.disabled));
    app._markCtrlModalOnline();
    assert.equal(nodes.has('modbus-ctrl-offline-banner'), false);
    for (const device of [{ deviceType: 'motor', enabled: false }, { deviceType: 'relay', enabled: true }]) {
        app._modbusDevices[0] = device;
        app._setCtrlModalOffline(true, 'offline');
        assert.equal(app._ctrlModalCommandsBlocked(), true);
        assert.equal(controls[0].disabled, true);
        assert.equal(controls[2].disabled, false, 'STOP stays available');
        assert.equal(controls[3].disabled, false, 'refresh stays available');
        app._markCtrlModalOnline();
        assert(controls.every(control => !control.disabled));
    }
}
console.log('PASS: source/minified offline error visibility, explicit motor recovery, disabled devices and STOP');
