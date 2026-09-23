import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class DeviceHealthTelemetryTests(unittest.TestCase):
    def run_extract(self, serial_output):
        script = r"""
const { DeviceHealthChecker } = require('./scripts/test-device-health.js');
const checker = new DeviceHealthChecker();
process.stdout.write(JSON.stringify(checker.extractTelemetry(process.argv[1])));
"""
        result = subprocess.run(
            ["node", "-e", script, serial_output],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        return json.loads(result.stdout)

    def test_uses_latest_sta_address_after_ap_recovery(self):
        telemetry = self.run_extract(
            "[STATUS] WiFi=AP_MODE ap_ip=192.168.4.1 clients=0\n"
            "[WiFi] Connected! IP=192.168.5.20 GW=192.168.5.1\n"
            "[STATUS] WiFi=CONNECTED ssid=fastbee ip=192.168.5.20 rssi=-70 ch=11\n"
        )
        self.assertEqual("STA", telemetry["networkMode"])
        self.assertEqual("192.168.5.20", telemetry["ip"])

    def test_reports_ap_mode_without_mislabeling_it_connected(self):
        telemetry = self.run_extract(
            "[STATUS] uptime=10m free8=47000 blk8=30000\n"
            "[STATUS] WiFi=AP_MODE ap_ip=192.168.4.1 clients=0\n"
        )
        self.assertEqual("AP", telemetry["networkMode"])
        self.assertEqual("192.168.4.1", telemetry["ip"])

    def test_psram_total_heap_is_not_reported_as_dram(self):
        telemetry = self.run_extract(
            "[PROTO-ALIVE] handle called heap=8404948 mqtt=disabled\n"
            "[STATUS] uptime=2m free8=38368 blk8=25588 psram=8366580\n"
            "[STATUS] WiFi=CONNECTED ssid=fastbee ip=192.168.5.10 rssi=-45 ch=11\n"
        )
        self.assertEqual(38368, telemetry["dramFree"]["last"])
        self.assertEqual(8366580, telemetry["psramFree"]["last"])

    def test_psram_only_protocol_sample_is_not_reported_as_dram(self):
        telemetry = self.run_extract(
            "[PROTO-ALIVE] handle called heap=8404948 mqtt=disabled\n"
        )
        self.assertEqual(0, telemetry["dramFree"]["samples"])
        self.assertEqual(8404948, telemetry["psramFree"]["last"])

    def test_platform_ready_heap_is_a_valid_short_capture_dram_sample(self):
        telemetry = self.run_extract(
            "[BOOT] Platform ready! Heap: 55260 bytes\n"
            "FastBee IoT Platform Ready!\n"
        )
        self.assertEqual(1, telemetry["dramFree"]["samples"])
        self.assertEqual(55260, telemetry["dramFree"]["last"])

    def test_psram_only_capture_is_a_warning_not_a_hard_failure(self):
        script = r"""
const { DeviceHealthChecker } = require('./scripts/test-device-health.js');
const checker = new DeviceHealthChecker();
checker.checkSerialOutput = function() {
  return Promise.resolve({
    port: 'COM15',
    status: 'success',
    serialOutput: '[PROTO-ALIVE] handle called heap=8404948 mqtt=disabled\n',
    telemetry: this.extractTelemetry('[PROTO-ALIVE] handle called heap=8404948 mqtt=disabled\n')
  });
};
checker.verifyPeripherals = () => Promise.resolve({ peripherals: [], warnings: [] });
checker.runFullCheck(['COM15']).then(result => {
  process.stdout.write(JSON.stringify({
    success: result.success,
    errors: checker.errors,
    warnings: result.warnings
  }));
});
"""
        result = subprocess.run(
            ["node", "-e", script],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        payload = json.loads(result.stdout.splitlines()[-1])
        self.assertTrue(payload["success"])
        self.assertEqual([], payload["errors"])
        self.assertEqual(1, len(payload["warnings"]))

    def test_reports_disconnected_state_without_an_ip(self):
        telemetry = self.run_extract(
            "[STATUS] WiFi=CONNECTED ssid=fastbee ip=192.168.5.17 rssi=-65 ch=11\n"
            "[STATUS] WiFi=DISCONNECTED mode=1\n"
        )
        self.assertEqual("DISCONNECTED", telemetry["networkMode"])
        self.assertIsNone(telemetry["ip"])

    def test_extracts_uptime_from_status_and_metrics_logs(self):
        telemetry = self.run_extract(
            "[STATUS] uptime=1h2m3s free8=47000 blk8=30000\n"
            "[METRICS] heap=47000 dram8=47000 blk8=30000 up=3725s\n"
        )
        self.assertEqual(3725, telemetry["uptime"]["last"])
        self.assertEqual(2, telemetry["uptime"]["samples"])

    def test_default_fleet_includes_all_five_serial_ports(self):
        source = (ROOT / "scripts" / "test-device-health.js").read_text(encoding="utf-8")
        expected = "['COM4', 'COM11', 'COM15', 'COM7', 'COM16']"
        self.assertGreaterEqual(source.count(expected), 2)

    def test_json_report_creates_missing_parent_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            report = Path(temp_dir) / "nested" / "health.json"
            script = r"""
const { writeJsonReport } = require('./scripts/test-device-health.js');
writeJsonReport(process.argv[1], { success: true, results: [] });
"""
            subprocess.run(
                ["node", "-e", script, str(report)],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                {"success": True, "results": []},
                json.loads(report.read_text(encoding="utf-8")),
            )

    def test_eight_hour_capture_buffer_scales_without_becoming_unbounded(self):
        script = r"""
const { DeviceHealthChecker } = require('./scripts/test-device-health.js');
const checker = new DeviceHealthChecker();
checker.serialTimeoutMs = 8 * 60 * 60 * 1000;
process.stdout.write(String(checker.getSerialCaptureMaxBuffer()));
"""
        result = subprocess.run(
            ["node", "-e", script],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        max_buffer = int(result.stdout)
        self.assertGreaterEqual(max_buffer, 8 * 1024 * 1024)
        self.assertLessEqual(max_buffer, 16 * 1024 * 1024)

    def test_mqtt_reconnect_abort_is_not_a_boot_failure(self):
        script = r"""
const { findBootFailurePatterns } = require('./scripts/test-device-health.js');
const matches = findBootFailurePatterns('[MQTT] Reconnect aborted: insufficient memory');
process.stdout.write(JSON.stringify(matches.map(String)));
"""
        result = subprocess.run(
            ["node", "-e", script],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertEqual([], json.loads(result.stdout))

    def test_explicit_abort_call_is_a_boot_failure(self):
        script = r"""
const { findBootFailurePatterns } = require('./scripts/test-device-health.js');
const matches = findBootFailurePatterns('abort() was called at PC 0x40381234 on core 0');
process.stdout.write(JSON.stringify(matches.map(String)));
"""
        result = subprocess.run(
            ["node", "-e", script],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertEqual(1, len(json.loads(result.stdout)))

    def test_system_status_exposes_internal_dram_and_largest_block(self):
        source = (ROOT / "src/network/handlers/SystemRouteHandler.cpp").read_text(
            encoding="utf-8"
        )
        start = source.index("void SystemRouteHandler::handleSystemStatus")
        end = source.index("void SystemRouteHandler::handleSystemRestart", start)
        handler = source[start:end]
        self.assertIn(
            'doc["memory"]["dram8Free"] = FastBee::MemoryBudget::dram8Free()',
            handler,
        )
        self.assertIn(
            'doc["memory"]["dram8Largest"] = FastBee::MemoryBudget::dram8Largest()',
            handler,
        )

    def test_fleet_snapshot_uses_current_metrics_schema(self):
        source = (ROOT / "scripts" / "run-device-fleet-tests.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn('Path "/api/system/metrics"', source)
        self.assertIn("$metricsData.heap.dram_free", source)
        self.assertIn("$metricsData.heap.dram_largest", source)
        self.assertIn("$metricsData.heap.psram_free", source)

    def test_fleet_login_retries_transient_busy_responses(self):
        source = (ROOT / "scripts" / "run-device-fleet-tests.ps1").read_text(
            encoding="utf-8"
        )
        start = source.index("function Invoke-FleetLogin")
        end = source.index("function Invoke-FleetApi", start)
        block = source[start:end]
        self.assertIn("$attempt -lt 4", block)
        self.assertIn("Start-Sleep -Seconds 2", block)


if __name__ == "__main__":
    unittest.main()
