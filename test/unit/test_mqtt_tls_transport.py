"""Real TLS selection/allocation methods with counted, device-free transports."""
import hashlib
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class MqttTlsTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.inputs = [ROOT / name for name in (
            'src/protocols/MQTTClient.cpp', 'src/protocols/ProtocolManager.cpp',
            'src/network/CellularAdapter.cpp', 'src/network/handlers/MqttRouteHandler.cpp',
            'include/network/TlsTransportObservation.h', 'include/core/FeatureOwnerLifetime.h',
            'include/core/FeatureObservationCounter.h', 'include/core/FeatureInstanceRegistry.h',
            'include/core/AsyncExecTypes.h', 'test/unit/cpp/mqtt_tls_transport_harness.cpp.in',
            'test/unit/test_mqtt_tls_transport.py')]
        cls.inputs.append(ROOT / 'include/protocols/MqttSecureIntent.h')
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        mqtt, protocol, cell, route = [p.read_text(encoding='utf-8') for p in cls.inputs[:4]]
        methods = '\n'.join(definition(mqtt, sig) for sig in (
            'bool MQTTClient::setTransportClient(', 'bool MQTTClient::validateTransportSecurity(',
            'void MQTTClient::ensureTlsTransport(', 'void MQTTClient::releaseTlsTransport('))
        # Execute full begin, including packet-buffer allocation/readiness;
        # reconnect executes its real validation prefix before the handshake.
        begin = definition(mqtt, 'bool MQTTClient::begin(')
        reconnect = definition(mqtt, 'bool MQTTClient::reconnect(')
        methods += '\n' + begin
        methods += '\n' + reconnect[:reconnect.index('    _lastMqttsTlsMemoryFailure = false;')] + '    (void)isMqttsReconnect; return true;\n}'
        # Actual historical-config branches from the loader; no filesystem or
        # unrelated credential/authentication code is substituted for them.
        methods += '\nbool MQTTClient::loadLegacySecurity(JsonVariantConst cfg) {\n'
        methods += 'config.scheme = cfg["scheme"] | "mqtt"; config.port = cfg["port"] | 1883;\n'
        methods += definition(mqtt, '    if (!cfg["scheme"].is<const char*>() && config.port == 8883) {')
        methods += '\n' + definition(mqtt, '    if (cfg["downgraded_by_memory"] | false) {') + '\nreturn true;\n}'
        methods += '\n' + '\n'.join(definition(cell, sig) for sig in (
            'Client* CellularAdapter::getClient(', 'Client* CellularAdapter::getSecureClient('))
        methods += '\n' + definition(protocol, 'static bool configureMqttTransport(')
        methods += '\n' + definition(route, 'static Client* selectMqttTestClient(')
        guard = definition(cls.inputs[8].read_text(encoding='utf-8'), 'class RecursiveMutexGuard {') + ';'
        template = cls.inputs[9].read_text(encoding='utf-8').replace('@METHODS@', methods).replace('@GUARD@', guard)
        initial_stopped = re.search(r'MQTTClient::MQTTClient\(\)\s*:\s*isConnected\(false\), stopped\((true|false)\)', mqtt)
        if not initial_stopped:
            raise AssertionError('Review the changed production constructor initialization')
        template = template.replace('@INITIAL_STOPPED@', initial_stopped.group(1))
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-mqtt-tls-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder / 'tls.cpp'
        cpp.write_text(template, encoding='utf-8')
        cls.binaries = {}
        for cellular in (0, 1):
            binary = folder / ('tls-' + str(cellular) + ('.exe' if os.name == 'nt' else ''))
            result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-DFASTBEE_ENABLE_CELLULAR=' + str(cellular),
                '-DFASTBEE_ENABLE_HEALTH_MONITOR=0',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                '-I' + str(ROOT / 'include'), '-I' + str(headers[0].parent),
                str(cpp), '-o', str(binary)], capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[cellular] = binary

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('TLS transport inputs changed during execution')

    def run_case(self, case):
        for cellular, binary in self.binaries.items():
            with self.subTest(cellular=cellular):
                result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('select', 'selection_failure', 'frozen_transport', 'begin_and_reconnect_guard',
              'internal_tls', 'allocation_failure', 'bundle_failure', 'overlap',
              'lost_owner', 'capacity', 'legacy_selector', 'historical_secure',
              'historical_invalid', 'secure_intent_compatibility', 'begin_packet_oom', 'begin_config_failure'):
    setattr(MqttTlsTransportTests, 'test_' + _case, lambda self, mode=_case: self.run_case(mode))


if __name__ == '__main__':
    unittest.main()
