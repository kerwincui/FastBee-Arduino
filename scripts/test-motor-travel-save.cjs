'use strict';
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const { minifyJS } = require('./minify-js');
const source = fs.readFileSync('web-src/modules/runtime/protocol/protocol-config.js', 'utf8');
const clone = value => JSON.parse(JSON.stringify(value));
(async () => {
    for (const compact of [false, true]) {
        const fields = new Map();
        const field = id => {
            if (!fields.has(id)) fields.set(id, { value: '', checked: false, querySelector: () => null });
            return fields.get(id);
        };
        const fixture = { modbusRtu: { enabled: true, peripheralId: 'uart-fixture',
            motorTravelDraftSupported: true, master: { tasks: [], devices: [{
                slaveAddress: 17, deviceType: 'motor', motorMinPosition: 0, motorMaxPosition: 0,
                motorTravelDraft: { version: 1, forwardClearanceMm: 222, reverseClearanceMm: 88,
                    forwardMarginMm: 5, reverseMarginMm: 5, countsPerMm: 0 }
            }] } } };
        let stored = clone(fixture);
        const calls = [], errors = [];
        let ignoreWrite = false;
        const app = { guardDeveloperModeAction: () => true,
            _setCheckbox: (id, value) => { field(id).checked = value; },
            _setValue: (id, value) => { field(id).value = value; },
            _loadUartPeripherals: id => { field('rtu-peripheral-id').value = id; },
            onModbusModeChange() {}, _loadModbusDevices() {}, _onTransferTypeChange() {},
            applyDeveloperModeState() {}, _getProtocolName: () => 'Modbus' };
        const sandbox = { AppState: app, console, window: {},
            document: { getElementById: field },
            Notification: { warning() { throw new Error('unexpected warning'); }, success() {},
                error(message) { errors.push(message); } },
            apiGet: async (url, params) => {
                assert.equal(url, '/api/protocol/config');
                assert.equal(params.section, 'runtime');
                return { success: true, data: clone(stored) };
            },
            apiPost: async (url, data) => {
                calls.push({ url, data });
                assert.equal(url, '/api/protocol/modbus-rtu/config');
                if (!ignoreWrite) stored.modbusRtu.master.devices = JSON.parse(data.modbusRtu_master_devices);
                return { success: true };
            } };
        vm.runInNewContext(compact ? minifyJS(source) : source, sandbox);
        app.refreshMasterStatus = () => {};
        app._startMasterStatusRefresh = () => {};
        await app._loadProtocolConfigData('modbus-rtu');
        assert.equal(app._protocolConfig.modbusRtu.motorTravelDraftSupported, true);
        app._modbusDevices[0].motorTravelDraft.forwardMarginMm = 6;
        assert.equal(await app.saveProtocolConfig('modbus-rtu-form'), true);
        assert.equal(calls.length, 1);
        assert.equal(app._protocolConfig, null);
        assert.equal(app._motorTravelDraftSupported, true);
        await app._loadProtocolConfigData('modbus-rtu', { noCache: true });
        assert.equal(app._modbusDevices[0].motorTravelDraft.forwardMarginMm, 6);
        assert.equal(app._modbusDevices[0].motorTravelDraft.countsPerMm, 0);
        assert.equal(app._modbusDevices[0].motorMinPosition, 0);
        assert.equal(app._modbusDevices[0].motorMaxPosition, 0);
        assert.equal(app._protocolConfig.modbusRtu.motorTravelDraftSupported, true);
        stored.modbusRtu.motorTravelDraftSupported = false;
        await app._loadProtocolConfigData('modbus-rtu', { noCache: true });
        assert.equal(app._motorTravelDraftSupported, false);
        app._modbusDevices[0].motorMinPosition = -400;
        app._modbusDevices[0].motorMaxPosition = 800;
        ignoreWrite = true;
        assert.equal(await app.saveProtocolConfig('modbus-rtu-form'), false, 'ACK alone cannot confirm limits');
        assert.match(errors.pop(), /读回不一致/);
        ignoreWrite = false;
        assert.equal(await app.saveProtocolConfig('modbus-rtu-form'), true);
        assert.equal(stored.modbusRtu.master.devices[0].motorMinPosition, -400);
        assert.equal(stored.modbusRtu.master.devices[0].motorMaxPosition, 800);
    }
    console.log('PASS: production load/save/reload, source/minified, mocked transport only');
})().catch(error => { console.error(error); process.exitCode = 1; });
