// Feed descriptors emitted by the real C++ serializer to the production UI.
// DOM and transport are seams; no firmware, HTTP or physical output is touched.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../..');
const schema = JSON.parse(fs.readFileSync(0, 'utf8'));
const nodes = new Map();
function input(value = '') {
    let text = String(value);
    return {get value() { return text; }, set value(v) { text = String(v); }, checked: false,
        disabled: false, required: false, dataset: {}, textContent: '', listeners: {},
        classList: {toggle() {}}, selectedOptions: [{disabled: false}],
        removeAttribute(name) { delete this[name]; },
        addEventListener(name, callback) { this.listeners[name] = callback; }};
}
function group() {
    const element = input();
    const children = new Map(['command', 'channel', 'value', 'unit', 'schema-error', 'channel-fields']
        .map(name => ['.pe-pca-' + name, input()]));
    element.querySelector = selector => children.get(selector) || null;
    element.querySelectorAll = () => ['command', 'channel', 'value'].map(name => children.get('.pe-pca-' + name));
    return element;
}
const app = {registerModule(name, methods) { Object.assign(this, methods); }};
const context = {AppState: app, window: {}, TextEncoder, console,
    document: {getElementById: id => nodes.get(id) || null, createElement: () => group()},
    escapeHtml: value => String(value).replace(/[&<>"']/g, char => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[char]))};
vm.createContext(context);
for (const name of ['peripherals', 'peripheral-save', 'periph-exec-form', 'periph-exec-templates']) {
    vm.runInContext(fs.readFileSync(path.join(root, 'web-src/modules/runtime/' + name + '.js'), 'utf8'), context);
}
let pca = structuredClone(schema.pca), mcp = structuredClone(schema.mcp);
Object.assign(app, {
    _getPeripheralTypeDescriptor: type => type === 57 ? pca : {implemented: true},
    _getPeripheralDriverMeta: () => mcp,
    _setPeripheralReferenceOptions(id, type, value) { if (value !== undefined) nodes.get(id).value = value; },
    _setFeatureAvailabilityHint() {}, _updatePeripheralPinRoleHint() {}, _fillDriverSelect() {},
    _setSectionVisible(node, visible) { if (node) node.visible = visible; },
    _periphFormConfig: {enabled: false}, _periphFormSession: {}, _pePeripherals: [{id: 'pwm', type: 57}]
});
const aliases = {busPeripheralId: 'bus-id', oePeripheralId: 'oe-id'};
for (const field of pca.configFields) nodes.set('pca9685-' + (aliases[field.key] || field.key), input());
nodes.set('pca9685-feature-status', input());
nodes.get('pca9685-bus-id').value = 'bus'; nodes.get('pca9685-oe-id').value = 'oe';
app._updatePca9685Fields();
assert.equal(nodes.get('pca9685-clockHz').value, '25000000');
assert.equal(nodes.get('pca9685-channelMask').step, '1');
assert.equal(nodes.get('pca9685-pwmHz').step, 'any');
assert.equal(nodes.get('pca9685-openDrain').checked, false);
let data = {};
assert.equal(app._collectPca9685Config(data), '');
assert.equal(Object.keys(JSON.parse(data.pca9685)).length, 11);
assert.equal(data.pins, '');
nodes.get('pca9685-address').value = '112';
assert(app._collectPca9685Config({})); nodes.get('pca9685-address').value = '64';
nodes.get('pca9685-oe-id').value = 'oe'.repeat(21);
assert(app._collectPca9685Config({})); nodes.get('pca9685-oe-id').value = 'oe';
nodes.get('pca9685-clockHz').value = '500000'; nodes.get('pca9685-pwmHz').value = '0.5';
nodes.get('pca9685-minPulseUs').value = '1000000'; nodes.get('pca9685-maxPulseUs').value = '1500000';
assert.equal(app._collectPca9685Config({}), ''); // no independent UI limits.
nodes.get('pca9685-pwmHz').value = '0'; assert(app._collectPca9685Config({}));
// A changed firmware numeric domain/default must actually change the existing
// form and its save validator, rather than merely display a schema label.
pca.configFields.find(field => field.key === 'address').max = 65;
pca.configFields.find(field => field.key === 'clockHz').default = 24000000;
app._periphFormSession = {};
app._updatePca9685Fields();
assert.equal(nodes.get('pca9685-clockHz').value, '24000000');
assert.equal(nodes.get('pca9685-address').max, '65');
nodes.get('pca9685-clockHz').value = '26000000';
nodes.get('pca9685-openDrain').checked = true;
app._updatePca9685Fields(); // metadata refresh must not replace an in-progress edit.
assert.equal(nodes.get('pca9685-clockHz').value, '26000000');
assert.equal(nodes.get('pca9685-openDrain').checked, true);
nodes.get('pca9685-address').value = '66'; assert(app._collectPca9685Config({}));
pca = {}; assert(app._collectPca9685Config({}));
pca = structuredClone(schema.pca);
for (const id of ['driver', 'bus-id', 'reset-id', 'address', 'interval', 'reset-fields', 'group-hint'])
    nodes.set('io-expander-' + id, input());
mcp.name = 'MCP23017'; mcp.requiresResetReference = true; mcp.groupActivation = true;
nodes.get('io-expander-driver').value = 'MCP23017';
nodes.get('io-expander-bus-id').value = 'bus'; nodes.get('io-expander-reset-id').value = 'reset';
app._updateIoExpanderFields(true);
assert.equal(nodes.get('io-expander-interval').value, '20');
assert.equal(nodes.get('io-expander-interval').max, '1000');
assert.equal(app._collectI2cDriverConfig({enabled: 0}, 55), '');
assert(app._collectI2cDriverConfig({enabled: 1}, 55)); // parent/group guard retained.
mcp.configFields.find(field => field.key === 'sampleInterval').max = 30;
nodes.get('io-expander-interval').value = '31';
assert(app._collectI2cDriverConfig({enabled: 0}, 55));
app._updateIoExpanderFields(false, {sampleInterval: 0, address: 32, busPeripheralId: 'bus', resetPeripheralId: 'reset'});
assert.equal(nodes.get('io-expander-interval').value, '20'); // persisted legacy zero -> explicit new default.
nodes.get('io-expander-reset-id').value = '中'.repeat(14);
assert(app._collectI2cDriverConfig({enabled: 0}, 55)); // 42 UTF-8 bytes, not 14 bytes.

app._peTypeCatalog = [{value: 57, ...structuredClone(schema.pca)}];
const action = payload => ({actionType: 10, targetPeriphId: 'pwm', actionValue: JSON.stringify(payload), useReceivedValue: false});
assert.equal(app._pca9685CommandError(action({action: 'duty', channel: 15, value: 100})), '');
assert.equal(app._pca9685CommandError(action({action: 'pulse', channel: 0, value: 0})), '');
assert.equal(app._pca9685CommandError(action({action: 'angle', channel: 0, value: 10000})), '');
for (const payload of [{action: 'prepare'}, {action: 'start'}, {action: 'stop', channel: 0},
    {action: 'duty', channel: 16, value: 0}, {action: 'duty', channel: 0.5, value: 0},
    {action: 'duty', channel: 0, value: 101}, {action: 'enable', powerCycleConfirmed: true}])
    assert(app._pca9685CommandError(action(payload)));
assert(app._pca9685CommandError({...action({action: 'stop'}), useReceivedValue: true}));
const blockNodes = new Map(['target-periph', 'action-type', 'action-value', 'action-value-group',
    'use-received-value-group', 'use-received-value'].map(key => ['.pe-' + key, input()]));
blockNodes.get('.pe-target-periph').value = 'pwm'; blockNodes.get('.pe-action-type').value = '10';
blockNodes.get('.pe-action-value').value = JSON.stringify({action: 'duty', channel: 1, value: 25});
blockNodes.get('.pe-action-value-group').insertAdjacentElement = (where, element) => blockNodes.set('.pe-pca9685-group', element);
const block = {querySelector: selector => blockNodes.get(selector) || null};
app._syncPca9685ActionFields(block);
const rendered = blockNodes.get('.pe-pca9685-group');
assert(!rendered.querySelector('.pe-pca-command').innerHTML.includes('prepare'));
assert(!rendered.querySelector('.pe-pca-command').innerHTML.includes('start'));
assert.equal(rendered.querySelector('.pe-pca-value').max, '100');
assert.equal(rendered.querySelector('.pe-pca-unit').textContent, '%');
rendered.querySelector('.pe-pca-value').value = '35';
rendered.querySelector('.pe-pca-value').listeners.input();
assert.equal(JSON.parse(blockNodes.get('.pe-action-value').value).value, 35);
app._peTypeCatalog[0].commandDescriptors.find(item => item.id === 'duty').fields.find(field => field.key === 'value').max = 30;
app._syncPca9685ActionFields(block);
assert.equal(rendered.querySelector('.pe-pca-value').max, '30');
assert(app._pca9685CommandError(action({action: 'duty', channel: 1, value: 35})));
rendered.querySelector('.pe-pca-command').value = '';
rendered.querySelector('.pe-pca-command').listeners.input();
assert(app._pca9685CommandError({...action({}), actionValue: blockNodes.get('.pe-action-value').value}));
const before = blockNodes.get('.pe-action-value').value;
app._peTypeCatalog = [];
app._syncPca9685ActionFields(block);
assert.equal(blockNodes.get('.pe-action-value').value, before);
assert.equal(rendered.querySelector('.pe-pca-command').disabled, true);
assert(app._pca9685CommandError(action({action: 'stop'})));
console.log('H1 actual descriptor Web checks passed');
