import json
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "web-src" / "modules" / "runtime" / "protocol" / "mqtt-config.js"


NODE_PROBE = r"""
const fs = require('fs');
const vm = require('vm');

const sourcePath = process.argv[1];
const source = fs.readFileSync(sourcePath, 'utf8');
const sandbox = {
  AppState: {},
  window: { isPageAbortError: () => false },
  document: {
    getElementById: () => null,
    querySelector: () => null,
  },
  console,
  setTimeout: () => 0,
  clearTimeout: () => {},
};
vm.runInNewContext(source, sandbox, { filename: sourcePath });

const app = sandbox.AppState;
if (typeof app._getMqttStatusFailureReason !== 'function' ||
    typeof app._mqttErrorCodeToText !== 'function') {
  throw new Error('MQTT status helpers were not registered');
}

// Exercise the badge branch that is easy to miss in string-only checks:
// autoReconnect=false/stopped + a backend error must remain visibly failed.
const elements = {
  'mqtt-status-badge': {
    className: 'mqtt-status-badge mqtt-status-detecting',
    textContent: '',
    title: '',
    classList: { contains: function (name) { return this._owner.className.split(/\\s+/).indexOf(name) >= 0; } },
  },
  'mqtt-status-server': { textContent: '' },
  'mqtt-status-clientid': { textContent: '' },
  'mqtt-status-reconnects': { textContent: '' },
  'mqtt-status-reason': { textContent: '', title: '' },
};
elements['mqtt-status-badge'].classList._owner = elements['mqtt-status-badge'];
sandbox.document.getElementById = function (id) { return elements[id] || null; };
app._updateMqttStatusUI({
  enabled: true,
  initialized: true,
  connected: false,
  connecting: false,
  stopped: true,
  autoReconnect: false,
  internetAvailable: true,
  networkConnected: true,
  lastErrorCode: 4,
  lastErrorClass: 'auth',
  lastErrorMessage: 'Broker 拒绝：用户名或密码错误',
});

// `internetAvailable` can lag behind a healthy STA connection (for example
// when the configured probe host is unavailable). The badge must not call that
// a local network outage when the backend explicitly reports networkConnected.
const networkLagElements = {
  'mqtt-status-badge': {
    className: 'mqtt-status-badge mqtt-status-detecting',
    textContent: '',
    title: '',
    classList: { contains: function (name) { return this._owner.className.split(/\\s+/).indexOf(name) >= 0; } },
  },
  'mqtt-status-server': { textContent: '' },
  'mqtt-status-clientid': { textContent: '' },
  'mqtt-status-reconnects': { textContent: '' },
  'mqtt-status-reason': { textContent: '', title: '' },
};
networkLagElements['mqtt-status-badge'].classList._owner = networkLagElements['mqtt-status-badge'];
sandbox.document.getElementById = function (id) { return networkLagElements[id] || null; };
app._updateMqttStatusUI({
  enabled: true,
  initialized: true,
  connected: false,
  connecting: true,
  autoReconnect: true,
  internetAvailable: false,
  networkConnected: true,
  statusReason: '正在连接 MQTT Broker',
  lastErrorCode: 0,
  lastErrorClass: 'none',
});

// A temporary memory guard stop must remain visible even though the user
// configuration is still enabled and the client is not persistently disabled.
const memoryStopElements = {
  'mqtt-status-badge': {
    className: 'mqtt-status-badge mqtt-status-detecting',
    textContent: '',
    title: '',
    classList: { contains: function (name) { return this._owner.className.split(/\s+/).indexOf(name) >= 0; } },
  },
  'mqtt-status-server': { textContent: '' },
  'mqtt-status-clientid': { textContent: '' },
  'mqtt-status-reconnects': { textContent: '' },
  'mqtt-status-reason': { textContent: '', title: '' },
};
memoryStopElements['mqtt-status-badge'].classList._owner = memoryStopElements['mqtt-status-badge'];
sandbox.document.getElementById = function (id) { return memoryStopElements[id] || null; };
app._updateMqttStatusUI({
  enabled: true,
  initialized: true,
  connected: false,
  connecting: false,
  stopped: true,
  stoppedByMemory: true,
  networkConnected: true,
  lastErrorCode: -14,
  lastErrorName: 'MQTT_STOPPED_BY_MEMORY',
  lastErrorClass: 'resource',
  lastErrorMessage: '设备内存紧张，MQTT 已暂时停止，内存恢复后将自动重连',
});

process.stdout.write(JSON.stringify({
  backendReason: app._getMqttStatusFailureReason({
    statusReason: '无法连接 MQTT Broker（DNS/TCP）',
    lastError: -2,
  }),
  readableError: app._getMqttStatusFailureReason({
    lastErrorMessage: 'Broker 拒绝：用户名或密码错误',
    lastError: 4,
  }),
  http503Message: app._getMqttStatusFailureReason({
    status: 503,
    data: { message: 'Server low memory, request rejected' },
  }),
  http401Reason: app._getMqttStatusFailureReason({
    status: 401,
    data: { success: false, error: 'Unauthorized', code: 401 },
  }),
  wrappedBackendReason: app._getMqttStatusFailureReason({
    message: 'Request failed',
    data: { statusReason: '设备当前内存紧张' },
  }),
  http503MemoryReason: app._getMqttStatusFailureReason({
    status: 503,
    data: { success: false, error: 'Server low memory, request rejected' },
  }),
  networkReason: app._getMqttStatusFailureReason({
    name: 'TypeError',
    message: 'Failed to fetch',
  }),
  timeoutReason: app._getMqttStatusFailureReason({
    name: 'AbortError',
    message: 'Request timed out after 15000ms',
    _requestTimeout: true,
  }),
  stoppedFailureBadge: elements['mqtt-status-badge'].textContent,
  stoppedFailureReason: elements['mqtt-status-reason'].textContent,
  networkLagBadge: networkLagElements['mqtt-status-badge'].textContent,
  networkLagReason: networkLagElements['mqtt-status-reason'].textContent,
  memoryStopBadge: memoryStopElements['mqtt-status-badge'].textContent,
  memoryStopReason: memoryStopElements['mqtt-status-reason'].textContent,
  envelopeMessage: app._getMqttStatusFailureReason({
    success: false,
    message: '状态接口返回失败',
  }),
  legacyCode: app._mqttErrorCodeToText(-5),
  clientInitCode: app._mqttErrorCodeToText(-11),
  temporaryStopCode: app._mqttErrorCodeToText(-14),
}));
"""


class MqttStatusVisibilityTests(unittest.TestCase):
    def run_probe(self):
        result = subprocess.run(
            ["node", "-e", NODE_PROBE, str(MODULE)],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return json.loads(result.stdout)

    def test_backend_status_reason_has_priority(self):
        result = self.run_probe()
        self.assertEqual(result["backendReason"], "无法连接 MQTT Broker（DNS/TCP）")

    def test_readable_error_message_is_exposed_to_ui(self):
        result = self.run_probe()
        self.assertEqual(result["readableError"], "Broker 拒绝：用户名或密码错误")

    def test_legacy_error_code_mapping_is_preserved(self):
        result = self.run_probe()
        self.assertIn("MQTTS", result["legacyCode"])

    def test_client_init_error_code_mapping_is_readable(self):
        result = self.run_probe()
        self.assertIn("MQTT_CLIENT_INIT_FAILED", result["clientInitCode"])

    def test_temporary_memory_stop_code_mapping_is_readable(self):
        result = self.run_probe()
        self.assertIn("暂时停止", result["temporaryStopCode"])

    def test_http_503_nested_message_is_preserved(self):
        result = self.run_probe()
        self.assertIn("内存紧张", result["http503Message"])

    def test_http_401_nested_reason_is_preserved(self):
        result = self.run_probe()
        self.assertEqual(result["http401Reason"], "登录已过期，请重新登录")

    def test_http_503_memory_reason_is_actionable(self):
        result = self.run_probe()
        self.assertIn("内存紧张", result["http503MemoryReason"])

    def test_browser_network_error_is_translated(self):
        result = self.run_probe()
        self.assertEqual(result["networkReason"], "无法连接设备，请检查网络连接")

    def test_request_timeout_is_translated(self):
        result = self.run_probe()
        self.assertEqual(result["timeoutReason"], "状态接口请求超时，请检查设备响应或稍后重试")

    def test_stopped_error_keeps_failure_badge_and_reason(self):
        result = self.run_probe()
        self.assertEqual(result["stoppedFailureBadge"], "连接失败")
        self.assertEqual(result["stoppedFailureReason"], "Broker 拒绝：用户名或密码错误")

    def test_stale_internet_probe_does_not_hide_broker_connecting_state(self):
        result = self.run_probe()
        self.assertEqual(result["networkLagBadge"], "连接中")
        self.assertEqual(result["networkLagReason"], "正在连接 MQTT Broker")

    def test_temporary_memory_stop_keeps_memory_protection_reason(self):
        result = self.run_probe()
        self.assertEqual(result["memoryStopBadge"], "内存保护")
        self.assertIn("暂时停止", result["memoryStopReason"])

    def test_nested_backend_reason_beats_generic_error_wrapper(self):
        result = self.run_probe()
        self.assertEqual(result["wrappedBackendReason"], "设备当前内存紧张")

    def test_unsuccessful_envelope_message_is_preserved(self):
        result = self.run_probe()
        self.assertEqual(result["envelopeMessage"], "状态接口返回失败")


if __name__ == "__main__":
    unittest.main()
