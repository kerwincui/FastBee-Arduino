"""Two whole resident MQTT methods versus originals, real JSON and allocation faults."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ['FASTBEE_TEST_ROOT']) if os.environ.get('FASTBEE_TEST_ROOT') else next(
    parent for parent in HERE.parents if (parent / 'platformio.ini').is_file())
SUPPORT = Path(os.environ.get('FASTBEE_TEST_SUPPORT_ROOT', ROOT))
sys.path.insert(0, str(SUPPORT / 'test/unit'))
from test_periph_exec_fixed_json import definition, local_includes

SIGNATURES = ('bool MQTTClient::publishDeviceInfo(', 'void MQTTClient::_notifyStatusChange(')
COUNTS = (12, 33)


def draft(source, expected):
    candidate, count = re.subn(r'\b(doc|data|summary)\["([^"\n]+)"\]\s*=\s*([^;]+);',
        lambda m: f'FastBee::setFixedJsonField({m[1]}, "{m[2]}", {m[3]});', source)
    if count != expected:
        raise AssertionError(f'Reviewed MQTT assignment count changed: {count} != {expected}')
    return candidate


class MqttResidentJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        if not Path(compiler).is_file():
            raise RuntimeError('C++17 compiler required')
        library = Path(os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE',
                                     SUPPORT / '.pio/libdeps/native/ArduinoJson/src'))
        if not (library / 'ArduinoJson.h').is_file():
            raise RuntimeError('Registered real ArduinoJson headers required')
        source_path = ROOT / 'src/protocols/MQTTClient.cpp'
        header_path = ROOT / 'include/protocols/MQTTClient.h'
        oracle_path = HERE / 'cpp/mqtt_resident_json_oracle.inc'
        harness_path = HERE / 'cpp/mqtt_resident_json_harness.cpp.in'
        source = source_path.read_text(encoding='utf-8')
        oracle = oracle_path.read_text(encoding='utf-8')
        if hashlib.sha256(oracle.encode()).hexdigest() != '33b4f9f1d4dc412e82eb88b889bd51db0af6e05a200c25a61b070a98d3cffca9':
            raise AssertionError('Frozen MQTT original changed')
        original = [definition(oracle, signature) for signature in SIGNATURES]
        candidates = [draft(body, expected) for body, expected in zip(original, COUNTS)]
        current = [definition(source, signature) for signature in SIGNATURES]
        for actual, before, after in zip(current, original, candidates):
            if actual not in (before, after):
                raise AssertionError('MQTT method differs outside reviewed 45 fixed writes')
        header = header_path.read_text(encoding='utf-8')
        types = '\n\n'.join(definition(header, item) + ';' for item in (
            'enum class MqttTopicType', 'enum class MqttAuthType', 'struct MqttPublishTopic',
            'struct MqttSubscribeTopic', 'struct MQTTConfig'))
        queue_types = '\n'.join(definition(header, item) + ';' for item in ('struct QueueStats', 'struct QueueSnapshot'))
        slots = re.findall(r'^static constexpr uint8_t MQTT_REPORT_SLOTS = [^\n]+', header, re.M)
        if len(slots) != 1:
            raise AssertionError('MQTT queue slot declaration changed')
        queue_path = ROOT / 'include/protocols/MqttReportQueueStatus.h'
        queue = definition(queue_path.read_text(encoding='utf-8'), 'inline void addMqttReportQueueStatus(')
        globals_text = '\n\n'.join(definition(source, item) for item in (
            'const char* mqttStatusErrorName(', 'const char* mqttStatusErrorClass(',
            'const char* mqttStatusErrorMessage('))
        field_names = ('isConnected', 'stopped', 'lastErrorCode', 'reconnectCount',
            '_lastMqttsTlsMemoryFailure', '_disabledByMemory', '_stoppedByMemory', '_webServerPaused')
        fields = []
        for name in field_names:
            matches = re.findall(r'^\s*std::atomic<[^>]+>\s+' + re.escape(name) + r'[^;]*;', header, re.M)
            if len(matches) != 1:
                raise AssertionError('Atomic field declaration changed: ' + name)
            fields.append(matches[0].strip())
        renamed = []
        for bodies, name in ((original, 'MqttOracle'), (current, 'MqttCurrent'), (candidates, 'MqttDraft')):
            renamed.extend(body.replace('MQTTClient::', name + '::', 1) for body in bodies)
        program = harness_path.read_text(encoding='utf-8')
        for marker, content in {
            '@TYPES@': slots[0] + '\n' + types,
            '@QUEUE_TYPES@': queue_types,
            '@ATOMIC_FIELDS@': '\n'.join(fields),
            '@QUEUE_WRITER@': 'namespace FastBee {\n' + queue + '\n}',
            '@ERROR_FUNCTIONS@': globals_text,
            '@METHODS@': '\n\n'.join(renamed),
        }.items():
            if program.count(marker) != 1:
                raise AssertionError(marker)
            program = program.replace(marker, content)
        helper = ROOT / 'src/network/FixedJsonField.cpp'
        diagnostics = ROOT / 'src/protocols/MqttDiagnosticJson.cpp'
        prelude = SUPPORT / 'test/unit/cpp/fixed_json_document_prelude.h'
        mock = SUPPORT / 'test/mocks/Arduino.h'
        roots = [ROOT / 'include', SUPPORT / 'test/unit/cpp', library]
        cls.inputs = {Path(__file__), source_path, header_path, oracle_path, harness_path, queue_path, mock, prelude,
                      SUPPORT / 'test/unit/test_periph_exec_fixed_json.py'}
        cls.inputs.update(local_includes([helper, diagnostics, ROOT / 'include/protocols/MqttReportRetirement.h',
            ROOT / 'include/protocols/MqttReconnectLifetime.h', ROOT / 'include/core/SystemConstants.h',
            SUPPORT / 'test/unit/cpp/document_json_probe.h'], roots))
        cls.inputs.update(library.rglob('*.h')); cls.inputs.update(library.rglob('*.hpp'))
        cls.hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}
        cls.addClassCleanup(cls.check_inputs)
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-mqtt-resident-json-')
        cls.addClassCleanup(cls.directory.cleanup)
        folder = Path(cls.directory.name)
        (folder / 'Arduino.h').write_text(mock.read_text(encoding='utf-8'), encoding='utf-8')
        cpp = folder / 'mqtt.cpp'; cpp.write_text(program, encoding='utf-8')
        cls.binary = folder / ('mqtt.exe' if os.name == 'nt' else 'mqtt')
        cls.env = dict(os.environ)
        cls.env['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.env.get('PATH', '')
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DUNIT_TEST', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
            '-DARDUINOJSON_ENABLE_STD_STRING=1', '-I' + str(folder), *['-I' + str(p) for p in roots],
            '-include', str(prelude), str(cpp), str(helper), str(diagnostics), '-o', str(cls.binary)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120, env=cls.env)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def check_inputs(cls):
        if cls.hashes != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}:
            raise AssertionError('MQTT source/oracle/helper/fixture inputs changed')

    def check(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=180, env=self.env)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + mode, ran.stdout)


for _mode in ('info-values', 'info-oom', 'status-values', 'status-oom'):
    setattr(MqttResidentJsonTests, 'test_' + _mode.replace('-', '_'), lambda self, mode=_mode: self.check(mode))

if __name__ == '__main__':
    unittest.main()
