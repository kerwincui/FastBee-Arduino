"""Bound HTTP observation of one device; never grants complete S11 acceptance.

Use a fresh output directory and a reviewed private binding. The only POST is
one initial login. Authenticated GET activity renews the existing sliding
session; a later 401 is a failed window, never an excuse to silently log in.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import threading
import time
from datetime import datetime, timezone
from urllib.parse import urlsplit


MAX_SECONDS = 604800
CONFIG_NAMES = frozenset(("device.json", "network.json", "features.json",
    "peripherals.json", "periph_exec.json", "protocol.json", "rule_scripts.json",
    "screen_panel.json", "point_history.json"))
REQUIRED_CONFIGS = CONFIG_NAMES - {"rule_scripts.json", "screen_panel.json", "point_history.json"}
CONFIG_API_VIEWS = {"network.json": "/api/network/config?compact=1",
                    "protocol.json": "/api/protocol/config?compact=1"}
SHA = re.compile(r"^[0-9a-f]{64}$")
REPORT_REPLACE_DELAYS = (.025, .05, .075)
# Separate from the sampling clock: virtual soak tests patch time.sleep.
_report_publish_wait = threading.Event().wait


def save_report_atomic(output, report):
    """Retry only Windows sharing/access contention, keeping atomic replacement."""
    pending, target = output / "report.pending", output / "report.json"
    report.setdefault("reportPublishRetries", 0)
    report.setdefault("reportPublishFailures", 0)
    counts = report.setdefault("reportPublishErrorCounts", {})
    def write_pending():
        pending.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    write_pending()
    for attempt in range(len(REPORT_REPLACE_DELAYS) + 1):
        try:
            pending.replace(target)
            return
        except OSError as error:
            winerror = getattr(error, "winerror", None)
            kind = ("WINERROR_" + str(winerror) if sys.platform == "win32" and winerror is not None
                    else type(error).__name__)
            counts[kind] = counts.get(kind, 0) + 1
            retry = (sys.platform == "win32" and winerror in (5, 32, 33) and
                     attempt < len(REPORT_REPLACE_DELAYS))
            if retry:
                report["reportPublishRetries"] += 1
            else:
                report["reportPublishFailures"] += 1
                report["status"] = "FAIL"
            # Preserve the final failure in pending even when target stays locked.
            write_pending()
            if not retry:
                raise
            _report_publish_wait(REPORT_REPLACE_DELAYS[attempt])


class ObservationError(Exception):
    def __init__(self, code, fatal=False):
        self.code, self.fatal = code, fatal
        super().__init__(code)


def require(condition, code, fatal=True):
    if not condition:
        raise ObservationError(code, fatal)


def digest(value):
    return hashlib.sha256(value).hexdigest()


def mac(value):
    return re.sub(r"[^0-9a-f]", "", str(value).lower())


def number(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def json_data(body):
    try:
        value = json.loads(body)
    except (ValueError, UnicodeError):
        raise ObservationError("invalid_json") from None
    require(isinstance(value, dict), "invalid_json_object", False)
    require(value.get("success") is not False, "api_rejected", False)
    data = value.get("data", value)
    require(isinstance(data, dict), "invalid_api_data", False)
    return data


def diagnostic_field(data, keys, expected):
    """Record an optional diagnostic literally; missing/null is never zero."""
    value = data
    for key in keys:
        if not isinstance(value, dict) or key not in value:
            return {"observed": False, "reason": "missing_field",
                    "path": ".".join(keys)}
        value = value[key]
    valid = type(value) is expected
    if expected is int:
        valid = valid and value >= 0
    return {"observed": valid, "reason": "observed" if valid else
            "null_value" if value is None else "invalid_type_or_value",
            "path": ".".join(keys), "value": value}


def percentile(values, fraction=.95):
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


def config_fingerprint(name, body):
    data = json_data(body)
    if name not in CONFIG_API_VIEWS:
        return digest(body)
    if name == "network.json":
        require(isinstance(data.get("network"), dict) and isinstance(data.get("sta"), dict),
                "incomplete_network_view", False)
        # The dedicated configuration response also contains live RSSI/IP.
        # Only this documented runtime block is excluded from config drift.
        data = {key: value for key, value in data.items() if key != "status"}
    else:
        require(isinstance(data.get("mqtt"), dict) or isinstance(data.get("modbusRtu"), dict),
                "incomplete_protocol_view", False)
    return digest(json.dumps(data, sort_keys=True, separators=(",", ":"),
                             ensure_ascii=False, allow_nan=False).encode("utf-8"))


class Binding:
    """Freeze local evidence and each immutable live configuration document."""
    def __init__(self, filename):
        self.path = Path(filename).resolve()
        raw = self.path.read_bytes()
        self.sha256 = digest(raw)
        self.data = json.loads(raw)
        b = self.data
        require(b.get("schemaVersion") == 1, "binding_schema")
        require(isinstance(b.get("deviceId"), str) and b["deviceId"], "device_id")
        require(len(mac(b.get("mac"))) == 12, "expected_mac")
        url = urlsplit(b.get("url", ""))
        require(url.scheme in ("http", "https") and url.hostname and not url.username
                and not url.password and url.path in ("", "/") and not url.query and not url.fragment,
                "origin_only")
        for key in ("sourceSha256", "firmwareSha256", "webSha256"):
            require(bool(SHA.fullmatch(str(b.get(key, "")))), "binding_" + key)
        require(isinstance(b.get("firmwareVersion"), str) and b["firmwareVersion"], "firmware_version")
        require(type(b.get("sketchSize")) is int and b["sketchSize"] > 0, "sketch_size")
        require(type(b.get("hasPsram")) is bool, "psram_contract")
        intervals = b.get("intervals", {})
        for key, minimum in (("health", 30), ("resources", 60), ("page", 600), ("config", 600)):
            require(type(intervals.get(key)) is int and minimum <= intervals[key] <= 3600,
                    "sampling_interval_" + key)
        configs = b.get("configs", [])
        names = [c.get("name") for c in configs]
        require(len(names) == len(set(names)) and REQUIRED_CONFIGS.issubset(names), "config_set")
        for item in configs:
            require(item.get("name") in CONFIG_NAMES and SHA.fullmatch(str(item.get("sha256", ""))),
                    "config_contract")
        # point_history.json defines history bindings; runtime point values,
        # users.lastLogin, logs and historical sample files are never hashed.
        self.evidence = []
        phases = set()
        for item in b.get("deploymentEvidence", []):
            require(item.get("phase") in ("firmware", "filesystem", "all"), "deployment_phase")
            report_path = (self.path.parent / item["path"]).resolve()
            report_raw = report_path.read_bytes()
            require(digest(report_raw) == item.get("sha256"), "deployment_report_hash")
            report = json.loads(report_raw)
            require(report.get("written") is True and report.get("verified") is True,
                    "deployment_not_verified")
            require(report.get("firmwareSha256") == b["firmwareSha256"], "deployment_firmware_mismatch")
            require(report.get("port") in b.get("ports", []), "deployment_port_mismatch")
            require(report.get("phase") == item["phase"], "deployment_phase_mismatch")
            if item["phase"] in ("all", "firmware"):
                require(report.get("applicationWritten") is True, "application_not_written")
                phases.add("firmware")
            if item["phase"] in ("all", "filesystem"):
                require(report.get("webSha256") == b["webSha256"], "deployment_web_mismatch")
                require(SHA.fullmatch(str(report.get("filesystemSha256", ""))), "filesystem_hash_missing")
                phases.add("filesystem")
            # One identity proof per phase prevents accidentally combining
            # two S3 reports that happen to carry an identical app hash.
            identity_path = (self.path.parent / item["identityPath"]).resolve()
            identity_raw = identity_path.read_bytes()
            require(digest(identity_raw) == item.get("identitySha256"), "identity_evidence_hash")
            identity = json.loads(identity_raw)
            require(mac(identity.get("mac")) == mac(b["mac"]) and identity.get("port") == report.get("port"),
                    "deployment_identity_mismatch")
            require(identity.get("deploymentReportSha256") == item["sha256"]
                    and identity.get("sourceSha256") == b["sourceSha256"], "identity_candidate_binding")
            self.evidence.extend(((report_path, item["sha256"]), (identity_path, item["identitySha256"])))
        require(phases == {"firmware", "filesystem"}, "both_deployment_phases_required")

    def check(self):
        try:
            require(digest(self.path.read_bytes()) == self.sha256, "binding_changed")
            for filename, expected in self.evidence:
                require(digest(filename.read_bytes()) == expected, "deployment_evidence_changed")
        except OSError:
            raise ObservationError("binding_unavailable", True) from None


class Transport:
    def __init__(self, origin, password, username="admin"):
        import requests
        self.session = requests.Session()
        self.session.trust_env = False
        self.origin, self.password, self.username = origin.rstrip("/"), password, username
        self.login_count = 0

    def request(self, route, login=False):
        import requests
        started = time.monotonic()
        response = None
        try:
            if login:
                require(self.login_count == 0, "repeat_login_forbidden")
                self.login_count += 1
                response = self.session.post(self.origin + "/api/auth/login",
                    data={"username": self.username, "password": self.password},
                    timeout=(3, 12), allow_redirects=False, stream=True)
            else:
                allowed = route in ("/api/health", "/api/system/info", "/api/system/info?full=1", "/api/system/status", "/api/system/metrics", "/") or route in CONFIG_API_VIEWS.values() or (
                    route.startswith("/api/config/transfer/export?name=") and
                    route.split("=", 1)[1] in CONFIG_NAMES - CONFIG_API_VIEWS.keys())
                require(allowed, "read_route_not_allowed")
                response = self.session.get(self.origin + route, timeout=(3, 12),
                    allow_redirects=False, stream=True, headers={"Accept-Encoding": "identity"})
            status = response.status_code
            require(status == 200, "http_" + str(status), status in (401, 403) or 300 <= status < 400)
            chunks, size = [], 0
            for chunk in response.iter_content(16384):
                size += len(chunk)
                require(size <= 2 * 1024 * 1024, "response_too_large")
                chunks.append(chunk)
            body = b"".join(chunks)
            length = response.headers.get("Content-Length")
            if length is not None and not response.headers.get("Content-Encoding"):
                require(length.isdigit() and int(length) == len(body), "truncated_body", False)
            require(bool(body), "empty_body", False)
            return body, time.monotonic() - started, bool(response.headers.get("Set-Cookie"))
        except requests.exceptions.RequestException:
            # Never persist raw exceptions: server URLs or text can contain
            # private materials. Failed response bodies remain failures.
            raise ObservationError("transport_or_body_failure") from None
        finally:
            if response is not None:
                response.close()

    def close(self):
        self.session.close()


class Monitor:
    def __init__(self, binding, output, seconds, transport, clock=time.monotonic,
                 wall=time.time, sleep=time.sleep):
        require(type(seconds) is int and 30 <= seconds <= MAX_SECONDS, "duration_bounds")
        self.binding, self.b = binding, binding.data
        self.output = Path(output)
        self.output.mkdir(parents=True, exist_ok=False)
        self.seconds, self.transport = seconds, transport
        self.clock, self.wall, self.sleep = clock, wall, sleep
        self.report = dict(schemaVersion=1, deviceId=self.b["deviceId"], status="RUNNING",
            bindingSha256=binding.sha256, requestedSeconds=seconds, startedAt=self.stamp(),
            intervals=self.b["intervals"], failures=0, failureCounts={}, samples={},
            authentication={"initialLogins": 0, "cookieRenewals": 0, "policy": "sliding-session-no-relogin"},
            coverage={"scope": "HTTP_CONFIG_AND_MEMORY", "s11Complete": False,
                "missing": ["remoteFirmwareSha256", "physicalOutputs", "browserInteraction", "SSE",
                            "taskStackHwm", "fullPeripheralFreshness", "panicSerialLog",
                            "secretOnlyConfigurationChanges"]})
        self.times = {key: [] for key in self.b["intervals"]}
        self.previous = {}
        self.latencies = []
        self.started = None
        self.last_clock = None
        self.last_wall = None
        self.save()

    def stamp(self):
        return datetime.fromtimestamp(self.wall(), timezone.utc).isoformat()

    def append(self, filename, value):
        with (self.output / filename).open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(value, ensure_ascii=False, allow_nan=False) + "\n")

    def save(self):
        self.report["updatedAt"] = self.stamp()
        save_report_atomic(self.output, self.report)

    def fail(self, code):
        self.report["failures"] += 1
        counts = self.report["failureCounts"]
        counts[code] = counts.get(code, 0) + 1
        self.append("events.jsonl", {"at": self.stamp(), "kind": "failure", "code": code})

    def request(self, route, login=False):
        body, latency, renewed = self.transport.request(route, login=login)
        require(number(latency), "invalid_latency", False)
        if renewed:
            self.report["authentication"]["cookieRenewals"] += 1
            self.append("events.jsonl", {"at": self.stamp(), "kind": "session_cookie_renewed"})
        return body, latency

    def advancing(self, key, value):
        require(number(value), "missing_" + key, False)
        old = self.previous.get(key)
        if old is not None:
            require(value >= old, "reset_" + key)
            require(value > old, "stale_" + key, False)
        self.previous[key] = value

    def sample(self, group):
        row = {"at": self.stamp(), "group": group}
        if group == "health":
            body, latency = self.request("/api/health")
            data = json_data(body)
            self.advancing("healthUptime", data.get("timestamp"))
            require(data.get("degraded") is not True and data.get("memory", {}).get("guardLevel")
                    not in ("SEVERE", "CRITICAL") and data.get("memoryDegradation", {}).get("active") is not True,
                    "memory_degradation", False)
            self.latencies.append(latency)
            row.update(latencySeconds=latency, uptimeMs=data["timestamp"])
        elif group == "resources":
            # Default info is brief and omits capability pools. A pressure-forced
            # brief response still fails the required pool checks below.
            data = json_data(self.request("/api/system/info?full=1")[0])
            require(mac(data.get("network", {}).get("macAddress")) == mac(self.b["mac"]), "identity_changed")
            require(data.get("device", {}).get("firmwareVersion") == self.b["firmwareVersion"]
                    and data.get("flash", {}).get("sketchSize") == self.b["sketchSize"], "firmware_changed")
            self.advancing("deviceUptime", data.get("uptime", {}).get("seconds"))
            memory = data.get("memory", {})
            pools = memory.get("capabilityPools", {})
            self.advancing("memorySampleUptime", pools.get("sampledAtUptimeMs"))
            selected = {}
            for name in ("internal8", "internalDma") + (("psram8",) if self.b["hasPsram"] else ()):
                pool = pools.get(name, {})
                require(pool.get("observed") is True, "missing_pool_" + name, False)
                require(all(type(pool.get(k)) is int and pool[k] >= 0 for k in ("free", "minFree", "largestBlock")),
                        "invalid_pool_" + name, False)
                require(pool["largestBlock"] <= pool["free"] and pool["minFree"] <= pool["free"],
                        "inconsistent_pool_" + name, False)
                selected[name] = {k: pool[k] for k in ("free", "minFree", "largestBlock")}
            status = json_data(self.request("/api/system/status")[0])
            require(mac(status.get("macAddress")) == mac(self.b["mac"]), "identity_changed")
            row.update(uptimeSeconds=data["uptime"]["seconds"], memory=selected)
            self.report["lastMemory"] = selected
            # Keep status evidence even if the additional metrics GET fails.
            # Current status may omit restart; a missing block is not a clean boot.
            diagnostics = {"at": self.stamp(), "group": group,
                "restart": diagnostic_field(status, ("restart",), dict),
                "sources": {"restart": "/api/system/status",
                            "metrics": "/api/system/metrics"}}
            try:
                metrics = json_data(self.request("/api/system/metrics")[0])
                diagnostics["metrics"] = {
                    "mqttQueueDepth": diagnostic_field(metrics, ("mqtt", "queue_depth"), int),
                    "isrQueueOverruns": diagnostic_field(metrics, ("isr", "queue_overruns"), int),
                    "isrQueueWaiting": diagnostic_field(metrics, ("isr", "queue_waiting"), int),
                    "memguard": diagnostic_field(metrics, ("memguard",), dict),
                    "stability": diagnostic_field(metrics, ("stability",), dict)}
            except ObservationError as error:
                diagnostics["metrics"] = {"observed": False, "reason": error.code}
                raise  # Existing run() records the failed sample/window; no retry/login.
            finally:
                self.append("resource-diagnostics.jsonl", diagnostics)
        elif group == "page":
            body, latency = self.request("/")
            require(b"<html" in body.lower() or b"<!doctype html" in body.lower(), "invalid_page", False)
            row.update(bytes=len(body), latencySeconds=latency)
        elif group == "config":
            hashes = {}
            for config in self.b["configs"]:
                name = config["name"]
                route = CONFIG_API_VIEWS.get(name, "/api/config/transfer/export?name=" + name)
                body, _ = self.request(route)
                require(config_fingerprint(name, body) == config["sha256"], "config_changed")
                hashes[config["name"]] = config["sha256"]
            row["hashes"] = hashes
        self.append("samples.jsonl", row)

    def finish(self, completed):
        now = self.clock()
        elapsed = max(0, now - self.started) if self.started is not None else 0
        self.report["elapsedSeconds"] = elapsed
        coverage = True
        for group, interval in self.b["intervals"].items():
            ticks = self.times[group]
            stats = {"successful": len(ticks), "spanSeconds": ticks[-1] - ticks[0] if ticks else 0}
            # Both boundary samples are required. The final cycle runs each
            # group even when duration is not divisible by its interval.
            stats["covered"] = bool(len(ticks) >= 2 and ticks[0] - self.started <= interval / 2
                                    and ticks[-1] >= self.started + self.seconds)
            coverage = coverage and stats["covered"]
            self.report["samples"][group] = stats
        self.report["healthP95Seconds"] = percentile(self.latencies)
        if self.latencies and self.report["healthP95Seconds"] > 1:
            self.fail("health_p95_over_1s")
        self.report["status"] = ("FAIL" if self.report["failures"] or self.report.get("reportPublishFailures", 0) else
            "HTTP_SCOPE_PASS" if completed and coverage and elapsed >= self.seconds else "INCOMPLETE")
        self.report["finishedAt"] = self.stamp()
        self.save()
        return self.report

    def run(self):
        completed = False
        try:
            self.binding.check()
            login = json_data(self.request("/api/auth/login", login=True)[0])
            self.report["authentication"]["initialLogins"] = 1
            require(login.get("success", True) is not False, "login_failed")
            self.started = self.clock()
            due = {group: self.started for group in self.b["intervals"]}
            while True:
                now, wall = self.clock(), self.wall()
                if self.last_clock is not None:
                    require(now >= self.last_clock, "monotonic_clock_reversed")
                    if abs((wall - self.last_wall) - (now - self.last_clock)) > 5:
                        self.append("events.jsonl", {"at": self.stamp(), "kind": "wall_clock_jump"})
                self.last_clock, self.last_wall = now, wall
                self.binding.check()
                final = now >= self.started + self.seconds
                for group, interval in self.b["intervals"].items():
                    tick = self.clock()
                    if tick < due[group] and not final:
                        continue
                    if tick > due[group] + max(10, interval / 2):
                        self.fail("sampling_gap_" + group)
                    try:
                        self.sample(group)
                        self.times[group].append(self.clock())
                    except ObservationError as error:
                        self.fail(error.code)
                        if error.fatal:
                            return self.finish(False)
                    due[group] = tick + interval
                self.save()
                if final:
                    completed = True
                    break
                self.sleep(max(0, min(min(due.values()), self.started + self.seconds) - self.clock()))
        except ObservationError as error:
            self.fail(error.code)
        except KeyboardInterrupt:
            self.append("events.jsonl", {"at": self.stamp(), "kind": "interrupted"})
        except Exception as error:
            self.fail("observer_" + type(error).__name__)
        finally:
            self.transport.close()
        return self.finish(completed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binding", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--seconds", type=int, required=True)
    args = parser.parse_args()
    binding = Binding(args.binding)
    password = os.environ.get("FASTBEE_PASSWORD")
    require(bool(password), "password_environment_required")
    observer = Monitor(binding, args.out, args.seconds,
        Transport(binding.data["url"], password, os.environ.get("FASTBEE_USERNAME", "admin")))
    report = observer.run()
    print(json.dumps({"status": report["status"], "failures": report["failures"], "s11Complete": False}))
    return 0 if report["status"] == "HTTP_SCOPE_PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
