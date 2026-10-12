"""Offline evidence tests: no network, COM port, build, or device mutations."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch


spec = importlib.util.spec_from_file_location("candidate_monitor",
    Path(__file__).resolve().parents[2] / "scripts/monitor-device-candidate.py")
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)
API_CONFIG_FIXTURES = {
    "network.json": {"network": {"mode": 0}, "sta": {"hasPassword": True}},
    "protocol.json": {"mqtt": {"enabled": False}}}


class Clock:
    def __init__(self):
        self.now = 0
        self.wall_offset = 0

    def mono(self):
        return self.now

    def wall(self):
        return 1800000000 + self.now + self.wall_offset

    def sleep(self, seconds):
        self.now += seconds


class FakeTransport:
    def __init__(self, clock, binding, hook=None):
        self.clock, self.binding, self.hook = clock, binding, hook
        self.calls, self.closed = [], False

    def request(self, route, login=False):
        self.calls.append((route, login))
        if self.hook:
            value = self.hook(route, self.clock.now)
            if value is not None:
                return value
        t = self.clock.now
        pool = {"observed": True, "free": 50000, "minFree": 40000, "largestBlock": 30000}
        values = {
            "/api/auth/login": {"success": True},
            "/api/health": {"timestamp": t * 1000, "degraded": False},
            "/api/system/info?full=1": {"network": {"macAddress": self.binding.data["mac"]},
                "device": {"firmwareVersion": "test-v1"}, "flash": {"sketchSize": 3000000},
                "uptime": {"seconds": t}, "memory": {"capabilityPools": {
                    "sampledAtUptimeMs": t * 1000, "internal8": pool, "internalDma": pool}}},
            "/api/system/status": {"macAddress": self.binding.data["mac"]},
            "/api/system/metrics": {"mqtt": {"queue_depth": 0},
                "isr": {"queue_overruns": 0, "queue_waiting": 0},
                "memguard": {"level": 0, "level_name": "NORMAL", "critical_duration_s": 0},
                "stability": {"dram_history_count": 0, "leak_suspect": False, "uptime_hours": 0}}}
        # Match handleSystemInfo: the default response keeps identity/uptime,
        # but only an explicit full request can supply capability pools.
        brief = copy.deepcopy(values["/api/system/info?full=1"])
        brief["brief"] = True
        brief["memory"] = {"heapFree": 50000, "heapMaxAlloc": 30000}
        values["/api/system/info"] = brief
        if route == "/":
            return b"<!DOCTYPE html><html></html>", .1, False
        if route.startswith("/api/config/transfer/export?"):
            return b"{}", .1, False
        for name, endpoint in monitor.CONFIG_API_VIEWS.items():
            if route == endpoint:
                return json.dumps(API_CONFIG_FIXTURES[name]).encode(), .1, False
        return json.dumps(values[route]).encode(), .1, not login

    def close(self):
        self.closed = True


class CandidateMonitorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.clock = Clock()
        report = {"written": True, "verified": True, "applicationWritten": True,
            "phase": "all", "port": "COM4", "firmwareSha256": "a" * 64,
            "webSha256": "b" * 64, "filesystemSha256": "c" * 64}
        report_hash = self.write("deployment.json", report)
        identity = {"mac": "14:63:93:90:2F:64", "port": "COM4",
            "deploymentReportSha256": report_hash, "sourceSha256": "d" * 64}
        identity_hash = self.write("identity.json", identity)
        self.contract = {"schemaVersion": 1, "deviceId": "test-c3", "url": "http://192.0.2.1",
            "mac": identity["mac"], "ports": ["COM4"], "sourceSha256": "d" * 64,
            "firmwareSha256": "a" * 64, "webSha256": "b" * 64,
            "firmwareVersion": "test-v1", "sketchSize": 3000000, "hasPsram": False,
            "intervals": {"health": 30, "resources": 60, "page": 600, "config": 600},
            "configs": [{"name": n, "sha256": monitor.config_fingerprint(n,
                json.dumps(API_CONFIG_FIXTURES[n]).encode() if n in API_CONFIG_FIXTURES else b"{}")}
                for n in sorted(monitor.REQUIRED_CONFIGS)],
            "deploymentEvidence": [{"path": "deployment.json", "sha256": report_hash,
                "phase": "all", "identityPath": "identity.json", "identitySha256": identity_hash}]}
        self.binding = self.bind()

    def write(self, name, value):
        raw = json.dumps(value).encode()
        (self.root / name).write_bytes(raw)
        return monitor.digest(raw)

    def bind(self):
        self.write("binding.json", self.contract)
        return monitor.Binding(self.root / "binding.json")

    def make(self, hook=None, seconds=1200):
        transport = FakeTransport(self.clock, self.binding, hook)
        observer = monitor.Monitor(self.binding, self.root / "out", seconds, transport,
            self.clock.mono, self.clock.wall, self.clock.sleep)
        return observer, transport

    def test_report_replace_retries_windows_contention_without_sampling_clock_or_failure_reset(self):
        observer, _ = self.make(seconds=120)
        observer.fail('existing_device_failure')
        original = (observer.output / 'report.json').read_bytes()
        original_replace = Path.replace
        failures = []
        for code in (5, 32, 33):
            error = PermissionError(13, 'report temporarily held')
            error.winerror = code
            failures.append(error)
        def replace(pending, destination):
            if failures:
                self.assertEqual((observer.output / 'report.json').read_bytes(), original)
                raise failures.pop(0)
            return original_replace(pending, destination)
        with patch.object(monitor.sys, 'platform', 'win32'), \
                patch.object(Path, 'replace', autospec=True, side_effect=replace) as rename, \
                patch.object(monitor.time, 'sleep') as sampling_sleep:
            observer.save()
        sampling_sleep.assert_not_called()
        self.assertEqual(self.clock.now, 0)
        self.assertEqual(rename.call_count, 4)
        saved = json.loads((observer.output / 'report.json').read_text(encoding='utf8'))
        self.assertEqual(saved['reportPublishRetries'], 3)
        self.assertEqual(saved['reportPublishErrorCounts'],
                         {'WINERROR_5': 1, 'WINERROR_32': 1, 'WINERROR_33': 1})
        self.assertEqual(saved['reportPublishFailures'], 0)
        self.assertEqual(saved['failureCounts'], {'existing_device_failure': 1})
        self.assertEqual(observer.run()['status'], 'FAIL')
        self.assertEqual(observer.report['reportPublishRetries'], 3)

    def test_report_replace_exhaustion_raises_and_preserves_previous_atomic_target(self):
        observer, _ = self.make(seconds=120)
        target = observer.output / 'report.json'
        original = target.read_bytes()
        error = PermissionError(13, 'still held')
        error.winerror = 32
        with patch.object(monitor.sys, 'platform', 'win32'), \
                patch.object(Path, 'replace', side_effect=error) as rename, \
                patch.object(monitor, '_report_publish_wait') as wait:
            with self.assertRaises(PermissionError) as raised:
                observer.save()
        self.assertIs(raised.exception, error)
        self.assertEqual(rename.call_count, 4)
        self.assertEqual(wait.call_count, 3)
        self.assertLess(sum(call.args[0] for call in wait.call_args_list), 1)
        self.assertEqual(observer.report['reportPublishRetries'], 3)
        self.assertEqual(observer.report['reportPublishErrorCounts'], {'WINERROR_32': 4})
        self.assertEqual(observer.report['reportPublishFailures'], 1)
        self.assertEqual(target.read_bytes(), original)
        pending = json.loads((observer.output / 'report.pending').read_text(encoding='utf8'))
        self.assertEqual(pending['reportPublishErrorCounts'], {'WINERROR_32': 4})
        self.assertEqual(pending['reportPublishRetries'], 3)
        self.assertEqual(pending['reportPublishFailures'], 1)
        self.assertEqual(pending['status'], 'FAIL')
        result = observer.run()
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(json.loads(target.read_text(encoding='utf8'))['reportPublishFailures'], 1)

    def test_report_replace_non_windows_or_other_error_is_not_retried(self):
        observer, _ = self.make(seconds=120)
        for platform, code in (('linux', 5), ('win32', 112), ('win32', None)):
            with self.subTest(platform=platform, code=code):
                report = dict(status='RUNNING')
                error = PermissionError(13, 'not retryable')
                if code is not None:
                    error.winerror = code
                with patch.object(monitor.sys, 'platform', platform), \
                        patch.object(Path, 'replace', side_effect=error) as rename, \
                        patch.object(monitor, '_report_publish_wait') as wait:
                    with self.assertRaises(PermissionError) as raised:
                        monitor.save_report_atomic(observer.output, report)
                self.assertIs(raised.exception, error)
                rename.assert_called_once()
                wait.assert_not_called()
                self.assertEqual(report['reportPublishRetries'], 0)
                self.assertEqual(report['reportPublishFailures'], 1)
                self.assertEqual(sum(report['reportPublishErrorCounts'].values()), 1)
                self.assertEqual(report['status'], 'FAIL')

    def test_report_pending_write_failure_is_not_retried_as_replace_contention(self):
        observer, _ = self.make(seconds=120)
        error = PermissionError(13, 'pending write denied')
        error.winerror = 5
        with patch.object(monitor.sys, 'platform', 'win32'), \
                patch.object(Path, 'write_text', side_effect=error) as write, \
                patch.object(Path, 'replace') as rename, \
                patch.object(monitor, '_report_publish_wait') as wait:
            with self.assertRaises(PermissionError):
                observer.save()
        write.assert_called_once()
        rename.assert_not_called()
        wait.assert_not_called()

    def test_run_report_contention_does_not_advance_virtual_sample_span(self):
        observer, _ = self.make(seconds=120)
        original_replace = Path.replace
        errors = [PermissionError(13, 'report held')]
        errors[0].winerror = 5
        def replace(pending, destination):
            if pending.name == 'report.pending' and errors:
                raise errors.pop()
            return original_replace(pending, destination)
        with patch.object(monitor.sys, 'platform', 'win32'), \
                patch.object(Path, 'replace', autospec=True, side_effect=replace):
            result = observer.run()
        self.assertEqual(result['status'], 'HTTP_SCOPE_PASS')
        self.assertEqual(result['elapsedSeconds'], 120)
        self.assertEqual(result['reportPublishRetries'], 1)
        self.assertEqual(result['reportPublishErrorCounts'], {'WINERROR_5': 1})
        self.assertEqual(result['failures'], 0)
        self.assertEqual(result['samples']['health']['successful'], 5)

    def test_independent_intervals_and_http_scope_only(self):
        observer, transport = self.make()
        report = observer.run()
        self.assertEqual(report["status"], "HTTP_SCOPE_PASS")
        self.assertEqual({k: v["successful"] for k, v in report["samples"].items()},
                         {"health": 41, "resources": 21, "page": 3, "config": 3})
        self.assertFalse(report["coverage"]["s11Complete"])
        self.assertIn("remoteFirmwareSha256", report["coverage"]["missing"])
        self.assertEqual(sum(login for _, login in transport.calls), 1)
        self.assertGreater(report["authentication"]["cookieRenewals"], 1)
        routes = [route for route, _ in transport.calls]
        self.assertEqual(routes.count("/api/system/info?full=1"), 21)
        self.assertNotIn("/api/system/info", routes)
        self.assertEqual(routes.count("/api/system/metrics"), 21)
        self.assertNotIn("/api/system/diag-bundle", routes)
        self.assertIn("/api/network/config?compact=1", routes)
        self.assertIn("/api/protocol/config?compact=1", routes)
        self.assertNotIn("/api/config/transfer/export?name=network.json", routes)
        self.assertNotIn("/api/config/transfer/export?name=protocol.json", routes)

    def test_metrics_failure_preserves_restart_and_recovery_cannot_clear_failure(self):
        restart = {"resetReason": "POWERON", "resetReasonCode": 1,
                   "abnormalRestart": False, "snapshotValid": False}
        def hook(route, now):
            if route == "/api/system/status":
                return json.dumps({"macAddress": self.binding.data["mac"],
                                   "restart": restart}).encode(), .1, False
            if route == "/api/system/metrics" and now == 60:
                raise monitor.ObservationError("transport_or_body_failure")
        observer, transport = self.make(hook, seconds=120)
        result = observer.run()
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["failureCounts"]["transport_or_body_failure"], 1)
        self.assertEqual(result["samples"]["resources"]["successful"], 2)
        self.assertEqual(sum(login for _, login in transport.calls), 1)
        self.assertEqual(sum(route == "/api/system/metrics" for route, _ in transport.calls), 3)
        rows = [json.loads(line) for line in (self.root / "out/resource-diagnostics.jsonl").read_text().splitlines()]
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(row["restart"]["value"] == restart for row in rows))
        self.assertEqual(rows[1]["metrics"], {"observed": False, "reason": "transport_or_body_failure"})
        self.assertTrue(rows[2]["metrics"]["mqttQueueDepth"]["observed"])
        self.assertFalse(result["coverage"]["s11Complete"])

    def test_default_info_is_brief_and_full_info_has_required_pools(self):
        transport = FakeTransport(self.clock, self.binding)
        brief = json.loads(transport.request("/api/system/info")[0])
        full = json.loads(transport.request("/api/system/info?full=1")[0])
        self.assertTrue(brief["brief"])
        self.assertNotIn("capabilityPools", brief["memory"])
        for key in ("network", "device", "flash", "uptime"):
            self.assertEqual(brief[key], full[key])
        self.assertTrue(full["memory"]["capabilityPools"]["internal8"]["observed"])
        self.assertTrue(full["memory"]["capabilityPools"]["internalDma"]["observed"])

    def test_full_info_pressure_fallback_fails_even_after_pool_recovery(self):
        fixture = FakeTransport(self.clock, self.binding)
        def hook(route, now):
            if route == "/api/system/info?full=1" and now == 60:
                brief = json.loads(fixture.request("/api/system/info")[0])
                # Aggregate heap/PSRAM cannot replace an unobserved capability pool.
                brief["memory"]["psramFree"] = 8 * 1024 * 1024
                return json.dumps(brief).encode(), .1, False
        observer, transport = self.make(hook, seconds=120)
        result = observer.run()
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["failureCounts"], {"missing_memorySampleUptime": 1})
        self.assertEqual(result["samples"]["resources"]["successful"], 2)
        self.assertEqual(sum(login for _, login in transport.calls), 1)
        self.assertEqual(sum(route == "/api/system/info?full=1" for route, _ in transport.calls), 3)
        samples = [json.loads(line) for line in
                   (self.root / "out/samples.jsonl").read_text().splitlines()]
        resources = [row for row in samples if row["group"] == "resources"]
        self.assertEqual([row["uptimeSeconds"] for row in resources], [0, 120])
        self.assertTrue(all(row["memory"]["internal8"]["free"] == 50000 for row in resources))
        self.assertFalse(result["coverage"]["s11Complete"])

    def test_optional_diagnostics_preserve_missing_null_and_false_distinct_from_zero(self):
        metrics = {"mqtt": {"queue_depth": 0}, "isr": {"queue_overruns": False},
                   "memguard": None, "stability": {"leak_suspect": False}}
        observer, _ = self.make(lambda route, _: (json.dumps(metrics).encode(), .1, False)
                                if route == "/api/system/metrics" else None, seconds=120)
        result = observer.run()
        self.assertEqual(result["status"], "HTTP_SCOPE_PASS")
        self.assertFalse(result["coverage"]["s11Complete"])
        rows = [json.loads(line) for line in (self.root / "out/resource-diagnostics.jsonl").read_text().splitlines()]
        for row in rows:
            self.assertEqual(row["restart"]["reason"], "missing_field")
            self.assertFalse(row["restart"]["observed"])
            self.assertNotIn("value", row["restart"])
            self.assertEqual(row["metrics"]["mqttQueueDepth"]["value"], 0)
            self.assertTrue(row["metrics"]["mqttQueueDepth"]["observed"])
            self.assertIs(row["metrics"]["isrQueueOverruns"]["value"], False)
            self.assertFalse(row["metrics"]["isrQueueOverruns"]["observed"])
            self.assertEqual(row["metrics"]["isrQueueOverruns"]["reason"], "invalid_type_or_value")
            self.assertEqual(row["metrics"]["isrQueueWaiting"]["reason"], "missing_field")
            self.assertNotIn("value", row["metrics"]["isrQueueWaiting"])
            self.assertEqual(row["metrics"]["memguard"]["reason"], "null_value")
            self.assertIsNone(row["metrics"]["memguard"]["value"])
            self.assertEqual(row["metrics"]["stability"]["value"], {"leak_suspect": False})

    def test_metrics_401_preserves_status_then_stops_without_relogin(self):
        def hook(route, now):
            if route == "/api/system/metrics" and now == 60:
                raise monitor.ObservationError("http_401", True)
        observer, transport = self.make(hook, seconds=120)
        result = observer.run()
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["failureCounts"]["http_401"], 1)
        self.assertEqual(self.clock.now, 60)
        self.assertEqual(sum(login for _, login in transport.calls), 1)
        self.assertEqual(transport.calls[-1], ("/api/system/metrics", False))
        self.assertTrue(transport.closed)
        rows = [json.loads(line) for line in (self.root / "out/resource-diagnostics.jsonl").read_text().splitlines()]
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[-1]["restart"]["reason"], "missing_field")
        self.assertEqual(rows[-1]["metrics"], {"observed": False, "reason": "http_401"})

    def test_metrics_transport_uses_get_without_task_stack_or_extra_post(self):
        transport = monitor.Transport("http://192.0.2.1", "never-output-this")
        self.addCleanup(transport.close)
        response = Mock(status_code=200, headers={})
        response.iter_content.return_value = [b'{"mqtt":{"queue_depth":0}}']
        with patch.object(transport.session, "get", return_value=response) as get, \
                patch.object(transport.session, "post") as post:
            body, _, _ = transport.request("/api/system/metrics")
            self.assertEqual(json.loads(body)["mqtt"]["queue_depth"], 0)
            get.assert_called_once()
            self.assertEqual(get.call_args.args[0], "http://192.0.2.1/api/system/metrics")
            with self.assertRaises(monitor.ObservationError):
                transport.request("/api/system/task-stack-snapshot")
            self.assertEqual(get.call_count, 1)
            post.assert_not_called()

    def test_168_hours_bound_and_no_fake_elapsed_completion(self):
        observer, _ = self.make(seconds=604800)
        observer.started = 0
        report = observer.finish(True)
        self.assertEqual(report["status"], "INCOMPLETE")
        self.assertTrue(all(v["successful"] == 0 for v in report["samples"].values()))
        with self.assertRaises(monitor.ObservationError):
            monitor.Monitor(self.binding, self.root / "other", 604801, None)

    def test_full_info_transport_allows_only_exact_query_without_extra_post(self):
        transport = monitor.Transport("http://192.0.2.1", "never-output-this")
        self.addCleanup(transport.close)
        response = Mock(status_code=200, headers={})
        response.iter_content.return_value = [b'{"memory":{"capabilityPools":{}}}']
        with patch.object(transport.session, "get", return_value=response) as get, \
                patch.object(transport.session, "post") as post:
            body, _, _ = transport.request("/api/system/info?full=1")
            self.assertIn("capabilityPools", json.loads(body)["memory"])
            get.assert_called_once()
            self.assertEqual(get.call_args.args[0], "http://192.0.2.1/api/system/info?full=1")
            for route in ("/api/system/info?full=0", "/api/system/info?full=true",
                          "/api/system/info?full=1&brief=1", "/api/system/info?probe=1"):
                with self.subTest(route=route), self.assertRaises(monitor.ObservationError) as raised:
                    transport.request(route)
                self.assertEqual(raised.exception.code, "read_route_not_allowed")
            self.assertEqual(get.call_count, 1)
            post.assert_not_called()

    def test_wall_clock_jump_does_not_supply_elapsed_time(self):
        def hook(route, now):
            if now == 30:
                self.clock.wall_offset = 604800
        observer, _ = self.make(hook, seconds=120)
        report = observer.run()
        self.assertEqual(report["elapsedSeconds"], 120)
        self.assertIn("wall_clock_jump", (self.root / "out/events.jsonl").read_text())

    def test_monotonic_reversal_fails(self):
        observer, _ = self.make(seconds=120)
        observer.sleep = lambda _: setattr(self.clock, "now", -1)
        report = observer.run()
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("monotonic_clock_reversed", report["failureCounts"])

    def test_failure_then_recovery_is_still_fail(self):
        def hook(route, now):
            if route == "/api/health" and now == 30:
                raise monitor.ObservationError("transport_or_body_failure")
        observer, _ = self.make(hook, seconds=120)
        report = observer.run()
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["failureCounts"]["transport_or_body_failure"], 1)
        self.assertEqual(report["samples"]["health"]["successful"], 4)

    def test_401_stops_without_reauthentication(self):
        def hook(route, now):
            if route == "/api/health" and now == 30:
                raise monitor.ObservationError("http_401", True)
        observer, transport = self.make(hook)
        report = observer.run()
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(sum(login for _, login in transport.calls), 1)
        self.assertEqual(self.clock.now, 30)
        self.assertTrue(transport.closed)

    def test_reset_stale_empty_and_missing_metrics_never_pass(self):
        cases = [("/api/health", b"{}", "missing_healthUptime"),
                 ("/api/health", b'{"timestamp":30000}', "stale_healthUptime"),
                 ("/api/health", b'{"timestamp":1}', "reset_healthUptime"),
                 ("/api/system/info?full=1", b"{}", "identity_changed")]
        for index, (route, raw, code) in enumerate(cases):
            with self.subTest(code=code):
                self.clock = Clock()
                def hook(actual, now):
                    if actual == route and now >= 60:
                        return raw, .1, False
                transport = FakeTransport(self.clock, self.binding, hook)
                observer = monitor.Monitor(self.binding, self.root / ("case" + str(index)), 120,
                    transport, self.clock.mono, self.clock.wall, self.clock.sleep)
                result = observer.run()
                self.assertEqual(result["status"], "FAIL")
                self.assertIn(code, result["failureCounts"])

    def test_same_mac_firmware_and_config_changes_fail(self):
        transport = FakeTransport(self.clock, self.binding)
        body = json.loads(transport.request("/api/system/info?full=1")[0])
        body["device"]["firmwareVersion"] = "other-version"
        observer, _ = self.make(lambda route, _: (json.dumps(body).encode(), .1, False)
                                if route == "/api/system/info?full=1" else None)
        self.assertIn("firmware_changed", observer.run()["failureCounts"])
        self.contract["configs"][0]["sha256"] = "e" * 64
        self.binding = self.bind()
        other = monitor.Monitor(self.binding, self.root / "config-out", 30,
            FakeTransport(self.clock, self.binding), self.clock.mono, self.clock.wall, self.clock.sleep)
        self.assertIn("config_changed", other.run()["failureCounts"])

    def test_missing_memory_pool_is_not_zero_or_psram_fallback(self):
        fixture = FakeTransport(self.clock, self.binding)
        body = json.loads(fixture.request("/api/system/info?full=1")[0])
        body["memory"]["capabilityPools"]["internalDma"]["observed"] = False
        observer, _ = self.make(lambda route, _: (json.dumps(body).encode(), .1, False)
                                if route == "/api/system/info?full=1" else None, seconds=30)
        result = observer.run()
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("missing_pool_internalDma", result["failureCounts"])

    def test_redacted_view_canonical_hash_excludes_only_live_network_status(self):
        base = {"network": {"mode": 0}, "sta": {"hasPassword": True, "password": "********"}}
        first = dict(base, status={"rssi": -55, "ipAddress": "192.0.2.1"})
        later = dict(base, status={"rssi": -70, "ipAddress": "192.0.2.2"})
        self.assertEqual(monitor.config_fingerprint("network.json", json.dumps(first).encode()),
                         monitor.config_fingerprint("network.json", json.dumps(later, indent=2).encode()))
        later["network"] = {"mode": 1}
        self.assertNotEqual(monitor.config_fingerprint("network.json", json.dumps(first).encode()),
                            monitor.config_fingerprint("network.json", json.dumps(later).encode()))
        for name in monitor.CONFIG_API_VIEWS:
            with self.subTest(name=name), self.assertRaises(monitor.ObservationError):
                monitor.config_fingerprint(name, b"{}")

    def test_report_and_binding_changes_fail(self):
        (self.root / "deployment.json").write_text("{}")
        with self.assertRaises(monitor.ObservationError) as raised:
            self.binding.check()
        self.assertEqual(raised.exception.code, "deployment_evidence_changed")

    def test_output_reuse_rejected_without_overwrite(self):
        observer, _ = self.make()
        original = (self.root / "out/report.json").read_bytes()
        with self.assertRaises(FileExistsError):
            self.make()
        self.assertEqual(original, (self.root / "out/report.json").read_bytes())

    def test_growing_history_logs_and_auth_files_cannot_be_config_contracts(self):
        for name in ("users.json", "auth.json", "history.jsonl", "logs.json"):
            with self.subTest(name=name):
                self.contract["configs"].append({"name": name, "sha256": "e" * 64})
                with self.assertRaises(monitor.ObservationError):
                    self.bind()
                self.contract["configs"].pop()

    def test_phase_evidence_cannot_cross_device_or_candidate(self):
        identity = json.loads((self.root / "identity.json").read_bytes())
        identity["mac"] = "E8:3D:C1:F7:65:44"
        self.contract["deploymentEvidence"][0]["identitySha256"] = self.write("identity.json", identity)
        with self.assertRaises(monitor.ObservationError):
            self.bind()

    def test_transport_truncation_and_nonfinite_fields(self):
        import requests
        transport = monitor.Transport("http://192.0.2.1", "never-output-this")
        self.addCleanup(transport.close)
        response = Mock(status_code=200, headers={"Content-Length": "10"})
        response.iter_content.return_value = [b"{}"]
        with patch.object(transport.session, "get", return_value=response):
            with self.assertRaises(monitor.ObservationError) as raised:
                transport.request("/api/health")
            self.assertEqual(raised.exception.code, "truncated_body")
        response.headers = {}
        response.iter_content.side_effect = requests.exceptions.ChunkedEncodingError("private-body")
        with patch.object(transport.session, "get", return_value=response):
            with self.assertRaises(monitor.ObservationError) as raised:
                transport.request("/api/health")
            self.assertEqual(str(raised.exception), "transport_or_body_failure")
        self.assertFalse(monitor.number(float("nan")))
        self.assertFalse(monitor.number(True))


if __name__ == "__main__":
    unittest.main()
