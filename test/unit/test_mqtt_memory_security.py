"""Compile real memory protection paths; configuration is a read-only fixture.

MQTT ownership uses the production lease and stop paths. The restart/backend,
clock and platform services are mocks; this is not broker or MCU evidence.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class MqttMemorySecurityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.inputs = [ROOT / name for name in (
            'src/systems/HealthMonitor.cpp', 'src/protocols/ProtocolManager.cpp',
            'include/protocols/MqttSecureIntent.h', 'include/protocols/MqttClientLease.h',
            'include/protocols/MqttMemoryDeadline.h',
            'include/protocols/MqttReportRetirement.h',
            'include/core/InternalAtomicWord.h', 'test/unit/test_mqtt_memory_security.py',
            'include/network/FixedJsonField.h', 'src/network/FixedJsonField.cpp',
            'test/unit/cpp/mqtt_memory_security_harness.cpp.in')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        health, protocol = [p.read_text(encoding='utf-8') for p in cls.inputs[:2]]
        methods = '#include "network/FixedJsonField.h"\n' + '\n'.join(definition(health, signature) for signature in (
            'void HealthMonitor::loadPersistedMqttsDowngrade(',
            'void HealthMonitor::clearMqttMemoryOverrideState(',
            'void HealthMonitor::downgradeMqttsToMqtt(',
            'void HealthMonitor::disableMqttForMemory(',
            'void HealthMonitor::restoreMqttsAfterMemory(',
            'bool HealthMonitor::refreshMqttRecoveryAdmission(',
            'void HealthMonitor::fillDegradationJson('))
        methods += '\n' + '\n'.join(definition(protocol, signature) for signature in (
            'FastBee::MqttClientLease ProtocolManager::acquireMQTTClient(',
            'bool ProtocolManager::stopMQTT(', 'bool ProtocolManager::finishMqttStop('))
        normal_entry = health[health.index('    const bool pendingNormalRecovery ='):]
        normal_entry = normal_entry[:normal_entry.index('    // 等级变化时执行降级/恢复措施')]
        methods += ('\nvoid HealthMonitor::pollNormalMqttRecovery() {\n'
                    'const bool periphExecWorkerRecoveryPending = false;\n'
                    'const auto newLevel = MemoryGuardLevel::NORMAL;\n' + normal_entry + '\n}')
        methods += ('\nbool ProtocolManager::initializeReplacement() {\n' +
                    definition(protocol, '    if (!mqttClient->begin(500)) {') + '\nreturn true;\n}')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-mqtt-memory-security-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder / 'memory_security.cpp'
        cpp.write_text(cls.inputs[-1].read_text(encoding='utf-8').replace('@METHODS@', methods), encoding='utf-8')
        cls.binaries = {}
        for mqtt in (0, 1):
            binary = folder / ('memory-' + str(mqtt) + ('.exe' if os.name == 'nt' else ''))
            result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-DFASTBEE_ENABLE_MQTT=' + str(mqtt),
                '-DFASTBEE_ENABLE_STORAGE_CACHE=0', '-DFASTBEE_ENABLE_PERIPH_EXEC=0',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                '-I' + str(ROOT / 'include'), '-I' + str(headers[0].parent),
                str(cpp), str(ROOT / 'src/network/FixedJsonField.cpp'), '-o', str(binary)], capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[mqtt] = binary

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('MQTT memory-security inputs changed during execution')

    def run_case(self, case, mqtt=1):
        result = subprocess.run([str(self.binaries[mqtt]), case], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_compiled_out(self):
        self.run_case('compiled_out', 0)


for _case in ('severe_preserves', 'critical_preserves', 'stop_contention', 'absent_owner',
              'load_failure', 'disabled_intent', 'historical_secure', 'historical_incomplete',
              'historical_disabled', 'restart_failure', 'deferred', 'ready_mismatch',
              'changed_plain_intent', 'storage_unavailable', 'clock_wrap', 'diagnostics', 'deferred_begin_failure'):
    setattr(MqttMemorySecurityTests, 'test_' + _case, lambda self, mode=_case: self.run_case(mode))


if __name__ == '__main__':
    unittest.main()
