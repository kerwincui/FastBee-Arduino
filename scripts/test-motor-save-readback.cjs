'use strict';
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const { minifyJS } = require('./minify-js');
const source = fs.readFileSync('web-src/modules/runtime/protocol/protocol-config.js', 'utf8');
(async () => {
    for (const compact of [false, true]) {
        let drop = false, failures = 0, reads = 0, writes = 0, stored = [];
        const app = { guardDeveloperModeAction: () => true,
            _masterTasks: [], _modbusDevices: [{ slaveAddress: 17, deviceType: 'motor',
                motorMinPosition: -400, motorMaxPosition: 800 }], _getProtocolName: () => 'Modbus' };
        const sandbox = { AppState: app, window: {}, console,
            document: { getElementById: id => id === 'modbus-rtu-form' ? null : { value: 'fixture', checked: true } },
            Notification: { success() {}, warning() {}, error() { failures++; } },
            apiPost: async (url, data) => {
                assert.equal(url, '/api/protocol/modbus-rtu/config'); writes++;
                if (!drop) stored = JSON.parse(data.modbusRtu_master_devices);
                return { success: true };
            },
            apiGetFresh: async (url, params) => {
                assert.equal(url, '/api/protocol/config'); assert.equal(params.section, 'runtime'); reads++;
                return { success: true, data: { modbusRtu: { master: { devices: stored } } } };
            } };
        vm.runInNewContext(compact ? minifyJS(source) : source, sandbox);
        assert.equal(await app.saveProtocolConfig('modbus-rtu-form'), true);
        assert.equal(reads, 1); assert.equal(writes, 1); assert.equal(failures, 0);
        drop = true;
        app._modbusDevices[0].motorMaxPosition = 900;
        assert.equal(await app.saveProtocolConfig('modbus-rtu-form'), false);
        assert.equal(reads, 2); assert.equal(writes, 2); assert.equal(failures, 1);
        drop = false;
        assert.equal(await app.saveProtocolConfig('modbus-rtu-form'), true);
        assert.equal(stored[0].motorMaxPosition, 900);
    }
    console.log('PASS: saved limits require fresh matching device readback; mismatches remain failed');
})().catch(error => { console.error(error); process.exitCode = 1; });
