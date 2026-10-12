"""Prepared recovery/CAS behavior tests; no execution or device access during development."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class MqttTestRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise RuntimeError("C++17 compiler required for final recovery gate")
        headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not headers:
            raise RuntimeError("Installed ArduinoJson required")
        cls.directory = tempfile.TemporaryDirectory(prefix="fastbee-mqtt-test-recovery-")
        cls.addClassCleanup(cls.directory.cleanup)
        source = (ROOT / "src/network/handlers/MqttRouteHandler.cpp").read_text(encoding="utf-8")
        storage = (ROOT / "src/systems/ConfigStorage.cpp").read_text(encoding="utf-8")
        cls.inputs = [ROOT / name for name in (
            "src/network/handlers/MqttRouteHandler.cpp", "src/systems/ConfigStorage.cpp",
            "include/network/FixedJsonField.h", "src/network/FixedJsonField.cpp",
            "test/unit/cpp/mqtt_test_recovery_harness.cpp.in", "test/unit/test_mqtt_test_recovery.py",
            "include/systems/ProtocolConfigGuard.h", "include/core/ReportInstanceBinding.h",
            "include/core/InternalAtomicWord.h", "include/protocols/MqttClientLease.h")]
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        methods = '#include "network/FixedJsonField.h"\n' + "\n".join(method(storage, signature) for signature in (
            "bool ConfigStorage::saveJSONConfig(",
            "bool ConfigStorage::loadJSONConfig(",
            "bool ConfigStorage::compareExchangeMqttConfig(",
        ))
        methods += "\n" + "\n".join(method(source, signature) for signature in (
            "static bool backupMqttConfig(",
            "static bool restoreMqttConfigFromBackup(",
            "static bool saveMqttTestConfig(",
            "void MqttRouteHandler::checkPendingTestRestore(",
        ))
        fixture = (ROOT / "test/unit/cpp/mqtt_test_recovery_harness.cpp.in").read_text(encoding="utf-8")
        fixture = fixture.replace("@METHODS@", methods)
        cpp = Path(cls.directory.name) / "recovery.cpp"
        cpp.write_text(fixture, encoding="utf-8")
        cls.binary = Path(cls.directory.name) / ("recovery.exe" if os.name == "nt" else "recovery")
        built = subprocess.run([compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            "-I"+str(ROOT / "include"), "-I"+str(headers[0].parent),
            str(cpp), str(ROOT / "src/network/FixedJsonField.cpp"), "-o", str(cls.binary)], capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError("MQTT recovery inputs changed during verification")

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class MqttTestIsolationIntegrationTests(unittest.TestCase):
    def test_all_business_transport_entries_reject_probe_before_work(self):
        source = (ROOT / "src/protocols/MQTTClient.cpp").read_text(encoding="utf-8")
        for name in ("publish", "publishToTopicImpl", "hasUsableFastBeeOtaReplyTopic", "publishDeviceInfo",
                     "publishMonitorData", "publishReportData", "publishModbusRtuReport", "publishOtaReply",
                     "publishDeviceEvent", "publishNtpSync", "subscribe", "subscribeExact",
                     "publishExact", "unsubscribeExact", "subscribeAll"):
            body = method(source, "bool MQTTClient::"+name+"(").split("{", 1)[1].lstrip()
            self.assertTrue(body.startswith("if (isConnectionTestOnly()) return false;"), name)
        for name, prepared in (("publishToTopic", "false"), ("publishPreparedTopic", "true")):
            body = method(source, "bool MQTTClient::"+name+"(").split("{", 1)[1].strip()
            self.assertEqual(body, "return publishToTopicImpl(topicIndex, message, " + prepared + ");\n}")
        for name in ("processQueuedCommands", "processQueuedReports", "mqttCallback"):
            body = method(source, "void MQTTClient::"+name+"(").split("{", 1)[1].lstrip()
            guard = "if (isConnectionTestOnly() || stopped) return;" if name == "mqttCallback" else "if (isConnectionTestOnly()) return;"
            self.assertTrue(body.startswith(guard), name)

    def test_begin_is_single_use_and_reload_is_private(self):
        header = (ROOT / "include/protocols/MQTTClient.h").read_text(encoding="utf-8")
        source = (ROOT / "src/protocols/MQTTClient.cpp").read_text(encoding="utf-8")
        self.assertGreater(header.index("bool loadMqttConfig("), header.index("private:", header.index("class MQTTClient")))
        body = method(source, "bool MQTTClient::begin(")
        self.assertLess(body.index("_beginAttempted.exchange("), body.index("loadMqttConfig()"))
        self.assertLess(body.index("if (!loadMqttConfig()) return false;"), body.index("mqttClient.setServer"))
        self.assertIn("_connectionTestOnly ? 0 : _reportInstanceId", header)

    def test_probe_has_no_will_or_business_connection_event(self):
        source = (ROOT / "src/protocols/MQTTClient.cpp").read_text(encoding="utf-8")
        reconnect = method(source, "bool MQTTClient::reconnect(")
        self.assertIn('!isConnectionTestOnly() && !config.willTopic.isEmpty()', reconnect)
        self.assertLess(reconnect.index("if (isConnectionTestOnly()) return true;"),
                        reconnect.index("EventType::EVENT_MQTT_CONNECTED"))
        self.assertIn('if (!isConnectionTestOnly())\n            PeriphExecManager', reconnect)
        self.assertIn("lastErrorCode = -9;", reconnect)
        self.assertIn("if (!isConnectionTestOnly() && reconnectCount <= 3)", source)

    def test_protocol_mutators_and_recovery_loads_share_guard(self):
        source = (ROOT / "src/systems/ConfigStorage.cpp").read_text(encoding="utf-8")
        for name in ("saveJSONConfig", "loadJSONConfig", "saveFactoryJSONConfig", "deleteJSONConfigGenerations",
                     "loadConfigSection", "deleteConfigFile", "formatFileSystem", "compareExchangeMqttConfig"):
            self.assertIn("FastBee::ProtocolConfigGuard guard", method(source, "bool ConfigStorage::"+name+"("), name)


for _case in ("roundtrip", "backup_open", "backup_short", "backup_read", "preserve_existing",
              "legacy", "corrupt", "save_fail", "restore_fail", "cleanup_fail", "conflict",
              "sibling_edit", "busy", "concurrent_save", "malformed", "cas_load_fail",
              "loop_retry", "loop_reboot", "loop_other_client", "loop_matching_probe",
              "loop_busy", "loop_disabled"):
    setattr(MqttTestRecoveryTests, "test_"+_case, lambda self, case=_case: self.check(case))


if __name__ == "__main__":
    unittest.main()
