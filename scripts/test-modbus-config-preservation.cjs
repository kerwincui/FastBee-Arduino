'use strict';
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const { minifyJS } = require('./minify-js');
const source = fs.readFileSync('web-src/modules/runtime/protocol/protocol-config.js', 'utf8');

(async () => {
    let cases = 0;
    for (const minified of [false, true]) for (const pin of [255, -1, 0, 14]) {
        const fields = new Map();
        const field = id => {
            if (!fields.has(id)) fields.set(id, { value: '', checked: false });
            return fields.get(id);
        };
        const stored = { modbusRtu: { enabled: true, peripheralId: 'uart_selected_not_first',
            dePin: pin, transferType: 0, master: { tasks: [], devices: [] } } };
        let reads = 0, writes = 0;
        const app = {
            _protocolConfig: { compact: true, modbusRtu: { enabled: true, master: {} } },
            _setValue(id, value) { field(id).value = String(value); },
            _setCheckbox(id, value) { field(id).checked = value; },
            _loadUartPeripherals(id) { field('rtu-peripheral-id').value = id; },
            onModbusModeChange() {}, _loadModbusDevices() {}, _onTransferTypeChange() {},
            refreshMasterStatus() {}, _startMasterStatusRefresh() {}, applyDeveloperModeState() {},
            guardDeveloperModeAction: () => true, _getProtocolName: () => 'Modbus'
        };
        const sandbox = { AppState: app, window: {}, console,
            document: { getElementById: id => id === 'modbus-rtu-form' ? null : field(id) },
            Notification: { success() {}, warning(message) { throw Error(message); }, error(message) { throw Error(message); } },
            apiGet: async (url, params) => {
                assert.equal(url, '/api/protocol/config');
                assert.notEqual(params?.compact, 1, 'An editable form must not request a lossy summary');
                reads++;
                return { success: true, data: stored };
            },
            apiPost: async (url, data) => {
                assert.equal(url, '/api/protocol/modbus-rtu/config');
                assert.equal(data.modbusRtu_peripheralId, stored.modbusRtu.peripheralId);
                assert.equal(data.modbusRtu_dePin, String(pin === 255 ? -1 : pin));
                writes++;
                return { success: true };
            }
        };
        vm.runInNewContext(minified ? minifyJS(source) : source, sandbox);
        app.refreshMasterStatus = () => {};
        app._startMasterStatusRefresh = () => {};
        await app._loadProtocolConfigData('modbus-rtu');
        assert.equal(reads, 1, 'Cached compact data must be replaced with persisted configuration');
        assert.equal(field('rtu-de-pin').value, String(pin === 255 ? -1 : pin));
        assert.equal(await app.saveProtocolConfig('modbus-rtu-form'), true);
        assert.equal(writes, 1);
        cases++;
    }
    console.log(`PASS: ${cases} source/minified load-save cases preserve UART selection and DE configuration`);
})().catch(error => { console.error(error); process.exitCode = 1; });
