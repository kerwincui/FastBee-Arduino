'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

function setup() {
    const elements = new Map();
    class Element {
        constructor(tag) { this.tagName = tag; this.children = []; this.style = {}; }
        set textContent(text) { this.text = String(text); this.children = []; }
        get textContent() { return (this.text || '') + this.children.map(x => x.textContent).join(' '); }
        set innerHTML(_) { throw new Error('Untrusted result must use textContent'); }
        appendChild(child) { this.children.push(child); return child; }
        insertAdjacentElement(_, child) { elements.set(child.id, child); }
        remove() { elements.delete(this.id); }
        querySelectorAll() { return []; }
    }
    const fields = {name: {value: 'Rule'}, 'original-id': {value: ''}, enabled: {checked: false},
        report: {checked: false}, error: {}, triggers: {}, actions: {}};
    for (const [key, props] of Object.entries(fields)) elements.set('periph-exec-' + key, Object.assign(new Element('div'), props));
    // Exercise the actual trigger/action collectors against DOM seams, not
    // replacement collector functions overwritten when the form registers.
    const block = (id, controls) => {
        const nodes = new Map(Object.entries(controls).map(([selector, props]) => {
            const field = Object.assign(new Element('input'), {id: id + selector}, props);
            elements.set(field.id, field);
            return [selector, field];
        }));
        return {querySelector: selector => nodes.get(selector) || null};
    };
    const trigger = block('trigger', {'.pe-trigger-type': {value: '1'},
        '.pe-timer-mode': {value: '0'}, '.pe-interval': {value: '30'}, '.pe-timepoint': {value: '08:00'}});
    const action = block('action', {'.pe-action-type': {value: '5', selectedOptions: [{dataset: {}}]},
        '.pe-target-periph': {value: 'dac'}, '.pe-action-value': {value: '3'},
        '.pe-use-received-value': {checked: false}, '.pe-sync-delay': {value: '0'}, '.pe-exec-mode': {value: '0'}});
    elements.get('periph-exec-triggers').querySelectorAll = selector => selector === '.periph-exec-config-item' ? [trigger] : [];
    elements.get('periph-exec-actions').querySelectorAll = selector => selector === '.periph-exec-config-item' ? [action] : [];
    elements.set('periph-exec-form', {querySelectorAll: () => [...elements.values()].filter(x => x.value !== undefined || x.checked !== undefined)});
    elements.set('save-periph-exec-btn', new Element('button'));
    const calls = [], errors = [];
    const app = {registerModule(_, members) {Object.assign(this, members);},
        guardDeveloperModeAction: () => true,
        showInlineError: (_, value) => errors.push(value), clearInlineError() {},
        hideModal() {}};
    const context = vm.createContext({AppState: app, window: {}, console: {error() {}},
        Notification: {success() {}}, document: {getElementById: id => elements.get(id),
            querySelectorAll: () => [], createElement: tag => new Element(tag)},
        apiPostJson: async (url, body) => {calls.push({url, body}); return responder(url, body);}});
    let responder = url => url.endsWith('/preflight') ? response() : {success: true};
    for (const name of ['periph-exec', 'periph-exec-templates', 'periph-exec-form']) {
        vm.runInContext(fs.readFileSync(path.join(__dirname, '../../web-src/modules/runtime/' + name + '.js'), 'utf8'), context);
    }
    Object.assign(app, {_peEditLoadSeq: 1, _peEditorDataReady: true, currentPage: 'periph-exec'});
    return {app, elements, calls, errors, setResponder: fn => {responder = fn;},
        text: () => elements.get('periph-exec-preflight')?.textContent || ''};
}
function response(patch = {}, item = {}) {
    return {success: true, data: {allowed: true, requiresConfirmation: true, issues: [],
        actionContractsVersion: 1, actionContractsTruncated: false,
        actionContracts: [{index: 0, actionType: 5, coverage: 'described', result: 'sdk_ack_only',
            idempotence: 'absolute_command_same_config', replay: 'no_automatic_replay',
            timeoutScope: 'final_write_admission_zero_wait_whole_deadline_unknown', timeoutBudgetMs: null, ...item}], ...patch}};
}

test('actual save checks once, reviews SDK boundary and saves only after matching fresh review', async () => {
    const s = setup();
    await s.app.savePeriphExecRule();
    assert.deepEqual(s.calls.map(x => x.url), ['/api/periph-exec/preflight']);
    assert.match(s.text(), /未验证物理输出/); assert.match(s.text(), /时限未知/);
    assert.ok(s.elements.get('periph-exec-preflight').children.some(x => x.tagName === 'details'));
    await s.app.savePeriphExecRule();
    assert.deepEqual(s.calls.map(x => x.url), ['/api/periph-exec/preflight', '/api/periph-exec/preflight', '/api/periph-exec']);
    assert.equal(s.calls[2].body.actions[0].actionValue, '3');
    assert.equal(s.app._peSavePending, false);
});
test('changed form and new warning invalidate prior confirmation', async () => {
    const s = setup(); await s.app.savePeriphExecRule();
    s.elements.get('periph-exec-name').value = 'Changed';
    await s.app.savePeriphExecRule();
    s.setResponder(() => response({requiresConfirmation: true, issues: [{severity: 'warning', message: 'Changed target'}]}));
    await s.app.savePeriphExecRule();
    assert.equal(s.calls.length, 3); assert.ok(s.calls.every(x => x.url.endsWith('/preflight')));
    assert.match(s.text(), /Changed target/);
});
test('denial, malformed response and HTTP error never fall back to saving', async () => {
    for (const result of [response({allowed: false}), response({issues: [{severity: 'error', message: '<img onerror=x>'}]}),
        {success: true}, {success: false}, response({allowed: 'true'})]) {
        const s = setup(); s.setResponder(() => result);
        await s.app.savePeriphExecRule(); await s.app.savePeriphExecRule();
        assert.ok(s.calls.every(x => x.url.endsWith('/preflight')));
    }
    const s = setup(); s.setResponder(() => {throw Object.assign(new Error('not found'), {status: 404});});
    await s.app.savePeriphExecRule(); assert.equal(s.calls.length, 1); assert.ok(s.errors.length);
});
test('stale response cannot save or render and pending click cannot issue another request', async () => {
    const s = setup(); let release;
    s.setResponder(() => new Promise(resolve => {release = resolve;}));
    const pending = s.app.savePeriphExecRule(); s.app.savePeriphExecRule();
    assert.equal(s.calls.length, 1);
    s.app._peEditLoadSeq++;
    release(response()); await pending;
    assert.equal(s.calls.length, 1); assert.equal(s.text(), ''); assert.equal(s.app._peSavePending, false);
});
test('missing, future, truncated, mismatched or unknown contracts remain unknown without inventing a save gate', () => {
    const s = setup(), rule = {actions: [{actionType: 5}]};
    for (const patch of [{actionContractsVersion: 2}, {actionContractsTruncated: true},
        {actionContracts: null}, {actionContracts: []}, {actionContracts: [{index: 2, actionType: 5}]}]) {
        const value = s.app._periphExecContractNotes(response(patch).data, rule);
        assert.equal(value.review, false); assert.match(value.notes.join(''), /未知/);
    }
    const unknown = s.app._periphExecContractNotes(response({}, {coverage: 'unknown', result: '<script>x</script>'}).data, rule);
    assert.match(unknown.notes.join(''), /未知/); assert.doesNotMatch(unknown.notes.join(''), /script/);
});
test('toggle, sample conversion budget and status observation are distinct', () => {
    const s = setup(), rule = {actions: [{actionType: 5}]};
    const toggle = s.app._periphExecContractNotes(response({}, {result: 'local_acceptance_only', idempotence: 'non_idempotent'}).data, rule);
    assert.match(toggle.notes.join(''), /不要直接重试/); assert.equal(toggle.review, true);
    const sample = s.app._periphExecContractNotes(response({}, {result: 'sample_with_quality',
        timeoutScope: 'conversion_budget_not_whole_action_deadline', timeoutBudgetMs: 66}).data, rule);
    assert.match(sample.notes.join(''), /66 ms/); assert.match(sample.notes.join(''), /不是整个动作/);
    assert.equal(sample.review, false);
    const status = s.app._periphExecContractNotes(response({}, {result: 'controller_status_only'}).data, rule);
    assert.match(status.notes.join(''), /只读取温控状态/); assert.equal(status.review, false);
});
test('safe text rendering and API update operation preserve existing rule id', async () => {
    const s = setup(); s.elements.get('periph-exec-original-id').value = 'existing';
    s.setResponder(url => url.endsWith('/preflight') ? response({requiresConfirmation: true,
        issues: [{severity: 'warning', message: '<script>untrusted</script>'}]}) : {success: true});
    await s.app.savePeriphExecRule(); assert.match(s.text(), /<script>untrusted<\/script>/);
    await s.app.savePeriphExecRule();
    assert.equal(s.calls[0].body.operation, 'update'); assert.equal(s.calls[0].body.rule.id, 'existing');
    assert.equal(s.calls[2].url, '/api/periph-exec/update');
});
test('ordinary descriptive metadata does not add a confirmation step without a decision', async () => {
    const s = setup(); s.setResponder(url => url.endsWith('/preflight') ? response({requiresConfirmation: false}) : {success: true});
    await s.app.savePeriphExecRule();
    assert.deepEqual(s.calls.map(x => x.url), ['/api/periph-exec/preflight', '/api/periph-exec']);
});
test('form edits during an allowed no-confirm preflight cannot save the old snapshot', async () => {
    const s = setup(); let release;
    s.setResponder(() => new Promise(resolve => {release = resolve;}));
    const pending = s.app.savePeriphExecRule();
    s.elements.get('periph-exec-name').value = 'Changed while checking';
    release(response({requiresConfirmation: false})); await pending;
    assert.equal(s.calls.length, 1); assert.equal(s.text(), '');
    assert.match(s.errors.join(''), /配置已变化/); assert.equal(s.app._pePreflightReview, null);
});
test('a newly inserted action control also invalidates an in-flight form snapshot', async () => {
    const s = setup(); let release;
    s.setResponder(() => new Promise(resolve => {release = resolve;}));
    const pending = s.app.savePeriphExecRule();
    s.elements.set('added-action', {value: '1'});
    release(response({requiresConfirmation: false})); await pending;
    assert.equal(s.calls.length, 1); assert.match(s.errors.join(''), /配置已变化/);
});
