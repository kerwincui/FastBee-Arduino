// Production modules and full save method; DOM and HTTP are explicit fakes.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../..');
const nodes = new Map();
function node(value = '') {
    let text = String(value);
    return {get value() {return text;}, set value(v) {text = String(v);},
        checked: false, disabled: false, textContent: '', reset() {},
        classList: {add() {}, remove() {}, toggle() {}}, dataset: {}};
}
for (const id of ['peripheral-original-id','peripheral-id-input','peripheral-name-input',
    'peripheral-type-input','peripheral-enabled-input','peripheral-pins-input',
    'peripheral-error','save-peripheral-btn','ds1302-hour-mode','ds1302-trickle-charge',
    'ds1302-boot-seed','ds1302-utc-offset','peripheral-modal-title','peripheral-form'])
    nodes.set(id, node());
let error = '', preflights = [], posts = [];
const app = {registerModule(name, methods) {Object.assign(this, methods);}};
const context = {AppState: app, window: {}, console, TextEncoder,
    Notification: {success() {}, warning() {}},
    document: {getElementById: id => nodes.get(id) || null, querySelectorAll: () => []},
    apiPost: async (url, data) => {posts.push({url, data: {...data}}); return {success: true};}};
vm.createContext(context);
for (const name of ['peripherals','peripheral-save'])
    vm.runInContext(fs.readFileSync(path.join(root, 'web-src/modules/runtime', name + '.js'), 'utf8'), context);
Object.assign(app, {
    guardDeveloperModeAction: () => true, currentPage: 'peripheral',
    _periphTypeCatalog: [{id: 50}], _isPeripheralTypeAvailable: () => true,
    clearInlineError() {error = '';}, showInlineError(el, text) {error = text;},
    _updatePeripheralTypeAvailability() {}, closePeripheralModal() {}, loadPeripherals() {},
    async _runPeripheralPreflight(data) {preflights.push({...data}); return true;},
    _loadPeripheralSaveModule() {}, async _ensureModalForAction() {return {};},
    _loadPeripheralProviders() {}, _resetPeripheralPreflight() {}, _syncPeripheralTypeSelect() {},
    onPeripheralTypeChange() {}, showModal() {}
});
nodes.get('peripheral-id-input').value = 'rtc';
nodes.get('peripheral-name-input').value = 'RTC';
nodes.get('peripheral-type-input').value = '50';
nodes.get('peripheral-pins-input').value = '5,6,7';
nodes.get('ds1302-hour-mode').value = '1';
nodes.get('ds1302-trickle-charge').value = '0';
async function save(enabled, offset) {
    nodes.get('ds1302-boot-seed').checked = enabled;
    nodes.get('ds1302-utc-offset').value = offset;
    error = ''; posts = []; preflights = []; app._periphFormSession = {};
    await app.savePeripheralConfig();
}
(async () => {
    app._restoreRtcBootSeed({bootSeed: true, utcOffsetMinutes: 480});
    assert.equal(nodes.get('ds1302-boot-seed').checked, true);
    assert.equal(nodes.get('ds1302-utc-offset').value, '480');
    await app.openPeripheralModal();
    assert.equal(nodes.get('ds1302-boot-seed').checked, false);
    assert.equal(nodes.get('ds1302-utc-offset').value, '');
    for (const params of [{}, {bootSeed: 'true', utcOffsetMinutes: '480'},
        {bootSeed: 1, utcOffsetMinutes: 841}]) {
        app._restoreRtcBootSeed(params);
        assert.equal(nodes.get('ds1302-boot-seed').checked, false);
        assert.equal(nodes.get('ds1302-utc-offset').value, '');
    }
    for (const offset of ['-840','-1','0','480','840']) {
        await save(true, offset);
        assert.equal(error, '');
        assert.equal(posts.length, 1); assert.equal(preflights.length, 1);
        assert.equal(posts[0].data.bootSeed, true);
        assert.equal(posts[0].data.utcOffsetMinutes, offset);
        assert.equal(preflights[0].bootSeed, true);
        assert.equal(preflights[0].utcOffsetMinutes, offset);
    }
    for (const offset of ['', '-841', '841', 'NaN', '1e2', '0.5', ' 0', '0 ', '0x0', '+8']) {
        await save(true, offset);
        assert(error); assert.equal(posts.length, 0); assert.equal(preflights.length, 0);
    }
    await save(false, '');
    assert.equal(error, ''); assert.equal(posts[0].data.bootSeed, false);
    assert.equal(posts[0].data.utcOffsetMinutes, '');
    await save(false, '841'); assert(error); assert.equal(posts.length, 0);
    console.log('RTC boot full form save and restoration passed; no network/device');
})().catch(error => {console.error(error); process.exitCode = 1;});
