"""Execute network form bindings with real events and late-bound save methods."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert = require('assert/strict'), fs = require('fs'), vm = require('vm');
const pairs = [
  ['wifi-form', 'saveNetworkConfig'], ['ap-form', 'saveAPConfig'],
  ['advanced-form', 'saveAdvancedConfig'], ['ethernet-form', 'saveEthernetConfig'],
  ['cellular-form', 'saveCellularConfig']
];
function setup(options = {}) {
  const trace = [], nodes = {}, handlers = new Map();
  const app = {}, root = {dataset: {eventsBound: options.bound ? 'true' : 'false'}};
  let methods;
  const document = {getElementById(id) {
    trace.push(['lookup', id]);
    if (id === 'network-page') return options.noRoot ? null : root;
    return nodes[id] || null;
  }};
  for (const [id] of pairs) {
    if (options.missing === id) continue;
    nodes[id] = {addEventListener(event, callback) {
      assert.equal(this, nodes[id]);
      trace.push(['bind', id, event]);
      handlers.set(id, callback);
    }};
  }
  vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/network.js', 'utf8'), {
    AppState: {registerModule(name, captured) {assert.equal(name, 'network'); methods = captured;}},
    document
  });
  Object.assign(app, methods);
  for (const name of ['setupStatusRefresh', 'setupBleProvision', '_filterNetworkTypesByCapabilities']) {
    app[name] = function() {assert.equal(this, app); trace.push(['setup', name]);};
  }
  app.setupNetworkEvents();
  return {app, trace, nodes, handlers, root};
}
const cases = {
  bindings_follow_existing_dom_order() {
    const state = setup();
    assert.deepEqual(state.trace, [
      ['lookup', 'network-page'], ['lookup', 'network-type'],
      ['lookup', 'network-type-save-btn'], ['lookup', 'wifi-scan-btn'],
      ...pairs.flatMap(([id]) => [['lookup', id], ['bind', id, 'submit']]),
      ['lookup', 'wifi-dhcp'], ['lookup', 'custom-domain'],
      ['setup', 'setupStatusRefresh'], ['setup', 'setupBleProvision'],
      ['setup', '_filterNetworkTypesByCapabilities']
    ]);
    assert.equal(state.root.dataset.eventsBound, 'true');
  },
  each_submit_prevents_default_before_correct_save() {
    const state = setup();
    for (const [id, method] of pairs) {
      const calls = [];
      state.app[method] = function(...args) {
        assert.equal(this, state.app);
        assert.deepEqual(args, []);
        calls.push(method);
      };
      const event = {preventDefault() {assert.equal(this, event); calls.push('prevent');}};
      state.handlers.get(id).call({notTheApp: true}, event);
      assert.deepEqual(calls, ['prevent', method]);
    }
  },
  save_method_is_resolved_at_submit_time() {
    const state = setup();
    for (const [id, method] of pairs) {
      let calls = 0;
      state.app[method] = () => {throw new Error('stale save handler');};
      const listener = state.handlers.get(id);
      state.app[method] = function() {assert.equal(this, state.app); calls++;};
      listener({preventDefault() {}});
      assert.equal(calls, 1);
    }
  },
  missing_forms_do_not_block_remaining_bindings() {
    for (const [missing] of pairs) {
      const state = setup({missing});
      assert.deepEqual([...state.handlers.keys()], pairs.map(([id]) => id).filter(id => id !== missing));
      assert.deepEqual(state.trace.filter(row => row[0] === 'setup').map(row => row[1]),
        ['setupStatusRefresh', 'setupBleProvision', '_filterNetworkTypesByCapabilities']);
    }
  },
  absent_or_already_bound_root_is_a_noop() {
    for (const options of [{noRoot: true}, {bound: true}]) {
      const state = setup(options);
      assert.deepEqual(state.trace, [['lookup', 'network-page']]);
      assert.equal(state.handlers.size, 0);
    }
  },
  repeated_setup_never_duplicates_listeners() {
    const state = setup();
    const count = state.trace.length;
    state.app.setupNetworkEvents();
    assert.deepEqual(state.trace.slice(count), [['lookup', 'network-page']]);
    assert.equal(state.handlers.size, 5);
  },
  save_return_values_do_not_change_listener_return() {
    const state = setup();
    for (const [id, method] of pairs) {
      state.app[method] = () => Promise.resolve('saved');
      assert.equal(state.handlers.get(id)({preventDefault() {}}), undefined);
    }
  },
  save_exceptions_propagate_after_prevent_default() {
    const state = setup();
    for (const [id, method] of pairs) {
      const failure = new Error(method), calls = [];
      state.app[method] = () => {calls.push('save'); throw failure;};
      assert.throws(() => state.handlers.get(id)({preventDefault() {calls.push('prevent');}}), error => error === failure);
      assert.deepEqual(calls, ['prevent', 'save']);
    }
  },
  prevent_default_exceptions_never_call_save() {
    const state = setup();
    for (const [id, method] of pairs) {
      const failure = new Error('prevent');
      let saves = 0;
      state.app[method] = () => saves++;
      assert.throws(() => state.handlers.get(id)({preventDefault() {throw failure;}}), error => error === failure);
      assert.equal(saves, 0);
    }
  }
};
cases[process.argv[1]]();
'''


class NetworkFormBindingTests(unittest.TestCase):
    def run_case(self, name):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run([node, '-e', SCRIPT, name], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8',
                                errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_bindings_follow_existing_dom_order(self):
        self.run_case('bindings_follow_existing_dom_order')

    def test_each_submit_prevents_default_before_correct_save(self):
        self.run_case('each_submit_prevents_default_before_correct_save')

    def test_save_method_is_resolved_at_submit_time(self):
        self.run_case('save_method_is_resolved_at_submit_time')

    def test_missing_forms_do_not_block_remaining_bindings(self):
        self.run_case('missing_forms_do_not_block_remaining_bindings')

    def test_absent_or_already_bound_root_is_a_noop(self):
        self.run_case('absent_or_already_bound_root_is_a_noop')

    def test_repeated_setup_never_duplicates_listeners(self):
        self.run_case('repeated_setup_never_duplicates_listeners')

    def test_save_return_values_do_not_change_listener_return(self):
        self.run_case('save_return_values_do_not_change_listener_return')

    def test_save_exceptions_propagate_after_prevent_default(self):
        self.run_case('save_exceptions_propagate_after_prevent_default')

    def test_prevent_default_exceptions_never_call_save(self):
        self.run_case('prevent_default_exceptions_never_call_save')


if __name__ == '__main__':
    unittest.main()
