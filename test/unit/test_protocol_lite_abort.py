import json
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULES = (
    ROOT / "web-src" / "modules" / "runtime" / "protocol" / "protocol-lite-config.js",
    ROOT / "web-src" / "modules" / "runtime" / "protocol" / "protocol-config.js",
)


NODE_PROBE = r"""
const fs = require('fs');
const vm = require('vm');

const sourcePath = process.argv[1];
const source = fs.readFileSync(sourcePath, 'utf8');
let errorLogs = 0;
const sandbox = {
  AppState: {},
  window: { isPageAbortError: err => !!(err && err._pageAborted) },
  document: { getElementById: () => null },
  console: { ...console, error: () => { errorLogs++; } },
  apiGet: () => Promise.reject(Object.assign(new Error('Request cancelled by page navigation'), {
    name: 'AbortError',
    _pageAborted: true,
  })),
};
vm.runInNewContext(source, sandbox, { filename: sourcePath });

Promise.resolve(sandbox.AppState._loadProtocolConfigData('mqtt'))
  .then(result => process.stdout.write(JSON.stringify({ result, errorLogs })))
  .catch(error => {
    process.stderr.write(String(error && error.stack || error));
    process.exitCode = 1;
  });
"""


NODE_SAVE_RESTORE_PROBE = r"""
const fs = require('fs');
const vm = require('vm');

const sourcePath = process.argv[1];
const source = fs.readFileSync(sourcePath, 'utf8');
const oldBase = { id: 'mqtt-ota-base-url', className: '', type: 'url', value: 'https://edited.example.com' };
const newBase = { id: 'mqtt-ota-base-url', className: '', type: 'url', value: 'https://server.example.com' };
const oldForm = { querySelectorAll: () => [oldBase] };
const newForm = { querySelectorAll: () => [newBase] };
const oldPublish = { innerHTML: '<div>edited-publish</div>' };
const oldSubscribe = { innerHTML: '<div>edited-subscribe</div>' };
const newPublish = { innerHTML: '<div>server-publish</div>' };
const newSubscribe = { innerHTML: '<div>server-subscribe</div>' };
let remounted = false;
let observed = null;

const sandbox = {
  AppState: {},
  document: {
    getElementById(id) {
      const nodes = remounted
        ? { 'mqtt-form': newForm, 'mqtt-publish-topics': newPublish, 'mqtt-subscribe-topics': newSubscribe }
        : { 'mqtt-form': oldForm, 'mqtt-publish-topics': oldPublish, 'mqtt-subscribe-topics': oldSubscribe };
      return nodes[id] || null;
    },
  },
  console,
};
vm.runInNewContext(source, sandbox, { filename: sourcePath });

sandbox.AppState._loadFullProtocolConfig = () => {
  remounted = true;
  sandbox.AppState.saveProtocolConfig = formId => {
    observed = {
      formId,
      otaBaseUrl: newBase.value,
      publishHtml: newPublish.innerHTML,
      subscribeHtml: newSubscribe.innerHTML,
    };
    return Promise.resolve(observed);
  };
  return Promise.resolve();
};

sandbox.AppState.saveProtocolConfig('mqtt-form')
  .then(() => process.stdout.write(JSON.stringify(observed)))
  .catch(error => {
    process.stderr.write(String(error && error.stack || error));
    process.exitCode = 1;
  });
"""


NODE_LAZY_RENDER_PROBE = r"""
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const root = process.argv[1];
const minified = process.argv[2] === 'minified';
const { minifyJS } = require(path.join(root, 'scripts/minify-js.js'));
const protocolDir = path.join(root, 'web-src/modules/runtime/protocol');
const tables = {
  'all-devices-body': { innerHTML: '' },
  'modbus-devices-body': { innerHTML: '' },
};
const requested = [];
let developerMode = false;
const sandbox = {
  AppState: {
    isDeveloperModeEnabled: () => developerMode,
    getDeveloperModeDisabledText: () => 'Enable <developer> & retry',
    applyDeveloperModeState() {},
  },
  window: {},
  console,
  setTimeout,
  clearTimeout,
  t: (key, fallback) => fallback,
  escapeHtml: value => String(value).replace(/[&<>"']/g, ch => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  })[ch]),
  document: {
    getElementById: id => tables[id] || null,
    querySelector: () => null,
    createElement: () => ({ dataset: {}, remove() {} }),
    head: {
      appendChild(script) {
        const modules = {
          'protocol-full-config': 'protocol-config.js',
          'protocol-modbus-rtu': 'modbus-config.js',
        };
        requested.push(script.dataset.module);
        assert.ok(modules[script.dataset.module], 'Unexpected lazy dependency');
        evaluate(path.join(protocolDir, modules[script.dataset.module]));
        script.onload();
      },
    },
  },
};
vm.createContext(sandbox);
function evaluate(file) {
  const source = fs.readFileSync(file, 'utf8');
  vm.runInContext(minified ? minifyJS(source) : source, sandbox, { filename: file });
}
function checkActions(html, expectedCount, lockedCount) {
  const buttons = Array.from(html.matchAll(/<button\b[^>]*>/g), match => match[0]);
  assert.strictEqual(buttons.length, expectedCount);
  assert.strictEqual(buttons.filter(button => / disabled\b/.test(button)).length, lockedCount);
  for (const button of buttons) {
    const control = button.includes('data-protocol-action="open-control-modal"');
    assert.strictEqual(/ disabled\b/.test(button), !developerMode && !control);
    if (!developerMode && !control) {
      assert.ok(button.includes('title="Enable &lt;developer&gt; &amp; retry"'));
    }
  }
}

(async () => {
  evaluate(path.join(root, 'web-src/js/module-loader.js'));
  evaluate(path.join(protocolDir, 'common.js'));
  evaluate(path.join(protocolDir, 'mqtt-config.js'));
  evaluate(path.join(protocolDir, 'protocol-lite-config.js'));
  const app = sandbox.AppState;
  const mqtt = { topic: '/device/report', qos: 2, enabled: true };
  const mqttHtml = app._buildMqttTopicItemHtml(0, mqtt, app._getMqttPublishTopicOptions(mqtt));
  assert.ok(mqttHtml.includes('value="/device/report"'));
  assert.ok(mqttHtml.includes('<option value="2" selected>2</option>'));
  assert.strictEqual(app._getProtocolIndexedItem({ closest: () => ({ dataset: { index: '3' } }) }, '.mqtt-topic-item').index, 3);
  assert.strictEqual(typeof app._renderAllDevices, 'undefined');
  assert.deepStrictEqual(requested, []);

  await new Promise(resolve => sandbox.ModuleLoader.loadModule('protocol-modbus-rtu', resolve));
  assert.deepStrictEqual(requested, ['protocol-full-config', 'protocol-modbus-rtu']);
  assert.ok(sandbox.ModuleLoader.isLoaded('protocol-modbus-rtu'));
  app._renderAllDevices();
  app._renderDeviceTable();
  assert.ok(tables['all-devices-body'].innerHTML.includes('colspan="6"'));
  assert.ok(tables['modbus-devices-body'].innerHTML.includes('colspan="8"'));

  app._masterTasks = [{ name: '<sensor & "one">', slaveAddress: 4, functionCode: 4,
    startAddress: 7, quantity: 2, mappings: [{}], enabled: false }];
  app._modbusDevices = [
    { name: '<relay & "two">', deviceType: 'relay', slaveAddress: 5,
      channelCount: 4, controlProtocol: 1, coilBase: 9, sensorId: '<id>', ncMode: true },
    { name: 'PWM', deviceType: 'pwm', channelCount: 2, pwmRegBase: 6, pwmResolution: 12, enabled: false },
    { name: 'PID', deviceType: 'pid', channelCount: 3, pidAddrs: [12] },
  ];
  app._renderAllDevices();
  app._renderDeviceTable();
  const allHtml = tables['all-devices-body'].innerHTML;
  assert.ok(allHtml.includes('&lt;sensor &amp; &quot;one&quot;&gt;'));
  assert.ok(allHtml.includes('&lt;relay &amp; &quot;two&quot;&gt;'));
  for (const info of ['FC04 @7 x2 [1 ', '4ch Reg@9 id:&lt;id&gt;', '2ch Reg@6 12bit', '3ch PV@12']) {
    assert.ok(allHtml.includes(info), info);
  }
  for (const type of ['sensor', 'relay', 'pwm', 'pid']) {
    assert.ok(allHtml.includes('protocol-badge--' + type));
  }
  assert.strictEqual((allHtml.match(/protocol-status-off/g) || []).length, 2);
  assert.strictEqual((allHtml.match(/protocol-status-on/g) || []).length, 2);
  assert.ok(allHtml.includes('data-protocol-action="edit-device" data-index="0" data-source="sensor"'));
  assert.ok(allHtml.includes('data-protocol-action="edit-device" data-index="2" data-source="control"'));
  assert.ok(allHtml.includes('data-protocol-action="open-mapping" data-index="0"'));
  checkActions(allHtml, 12, 9);
  const deviceHtml = tables['modbus-devices-body'].innerHTML;
  assert.ok(deviceHtml.includes('<td>4</td><td>9</td><td>Register</td><td>ON</td>'));
  assert.ok(deviceHtml.includes('data-protocol-action="remove-device" data-index="2"'));
  checkActions(deviceHtml, 9, 6);

  developerMode = true;
  app._renderAllDevices();
  app._renderDeviceTable();
  checkActions(tables['all-devices-body'].innerHTML, 12, 0);
  checkActions(tables['modbus-devices-body'].innerHTML, 9, 0);
  await new Promise(resolve => sandbox.ModuleLoader.loadModule('protocol-modbus-rtu', resolve));
  assert.strictEqual(requested.length, 2, 'Reopening Modbus must reuse the loaded sequence');
  process.stdout.write(JSON.stringify({ mqtt: true, lazyRendering: true, minified }));
})().catch(error => {
  process.stderr.write(String(error && error.stack || error));
  process.exitCode = 1;
});
"""


class ProtocolLiteAbortTests(unittest.TestCase):
    def test_page_navigation_abort_is_silent_and_resolved(self):
        for module in MODULES:
            with self.subTest(module=module.name):
                result = subprocess.run(
                    ["node", "-e", NODE_PROBE, str(module)],
                    cwd=ROOT,
                    check=True,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                )
                payload = json.loads(result.stdout)
                self.assertIsNone(payload["result"])
                self.assertEqual(payload["errorLogs"], 0)

    def test_first_save_restores_edits_into_remounted_form(self):
        result = subprocess.run(
            ["node", "-e", NODE_SAVE_RESTORE_PROBE, str(MODULES[0])],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        payload = json.loads(result.stdout)
        self.assertEqual(payload["formId"], "mqtt-form")
        self.assertEqual(payload["otaBaseUrl"], "https://edited.example.com")
        self.assertEqual(payload["publishHtml"], "<div>edited-publish</div>")
        self.assertEqual(payload["subscribeHtml"], "<div>edited-subscribe</div>")

    def test_mqtt_and_modbus_lazy_rendering_in_source_and_minified_code(self):
        for mode in ("source", "minified"):
            with self.subTest(mode=mode):
                result = subprocess.run(
                    ["node", "-e", NODE_LAZY_RENDER_PROBE, str(ROOT), mode],
                    cwd=ROOT,
                    check=True,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                )
                payload = json.loads(result.stdout)
                self.assertTrue(payload["mqtt"])
                self.assertTrue(payload["lazyRendering"])
                self.assertEqual(payload["minified"], mode == "minified")


if __name__ == "__main__":
    unittest.main()
