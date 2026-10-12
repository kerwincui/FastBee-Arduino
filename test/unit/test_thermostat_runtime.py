"""Production runtime methods with deterministic claim/lock/observation seams."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class ThermostatRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-thermostat-runtime-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        source = (ROOT / 'src/core/PeriphExecThermostat.cpp').read_text(encoding='utf-8')
        # Compile every real method, removing only the production dependency
        # includes and feature wrapper; the harness supplies explicit seams.
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.MULTILINE)
        source = source.replace('#if FASTBEE_ENABLE_PERIPH_EXEC\n', '', 1)
        if not source.rstrip().endswith('#endif'):
            raise AssertionError('Production runtime feature wrapper changed')
        source = source.rstrip()[:-len('#endif')]
        (directory / 'thermostat_runtime_methods.inc').write_text(source, encoding='utf-8')
        # Compile the actual channel selection functions as well. Only the
        # peripheral snapshot is a seam; aliases/defaults are not hand-coded.
        channels = (ROOT / 'src/core/SensorConfigSupport.cpp').read_text(encoding='utf-8')
        executor = (ROOT / 'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        channel_header = (ROOT / 'include/core/SensorConfigSupport.h').read_text(encoding='utf-8')
        channel_declaration = re.search(r'bool resolveSensorChannel\([\s\S]*?;', channel_header).group(0)
        (directory / 'thermostat_sensor_channels.inc').write_text(
            'namespace FastBee {\nnamespace {\n' +
            definition(channels, 'struct StoredFloatVisitor') + ';\n' +
            definition(channels, 'bool jsonStoresFloat(') + '\n' +
            definition(channels, 'bool configNumber(') + '\n}\n' +
            definition(channels, 'bool parseSensorMode(') + '\n' +
            channel_declaration + '\n' + definition(channels, 'bool resolveSensorChannel(') + '\n}\n' +
            definition(executor, 'String PeriphExecExecutor::guessSensorDataField(') + '\n' +
            definition(executor, 'String PeriphExecExecutor::resolveChannelField('), encoding='utf-8')
        # The frozen pre-fix producer binding is compiled too: the humidity
        # case must demonstrate old acceptance as well as new rejection.
        before = (ROOT / 'test/unit/cpp/thermostat_source_binding_before.inc').read_text(encoding='utf-8')
        if before.count('PeriphExecManager::thermostatSourceBindingLocked(') != 1:
            raise AssertionError('Frozen source-binding fixture changed')
        (directory / 'thermostat_source_binding_legacy_generated.inc').write_text(
            before.replace('PeriphExecManager::thermostatSourceBindingLocked(',
                           'PeriphExecManager::legacyThermostatSourceBindingLocked('), encoding='utf-8')
        cls.binary = directory / 'runtime.exe'
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-declarations', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
            '-DARDUINOJSON_ENABLE_STD_STRING=1', '-I' + str(directory),
            '-I' + str(ROOT / 'test/unit/cpp/thermostat_mocks'), '-I' + str(ROOT / 'test/mocks'),
            '-I' + str(ROOT / 'include'), '-I' + str(headers[0].parent),
            str(ROOT / 'test/unit/cpp/thermostat_runtime_harness.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _mode in ('shutdown', 'shutdown-lock', 'partial-claim', 'source-epoch',
              'source-calibration', 'memory', 'diagnostics', 'source-semantics',
              'source-humidity-inert', 'source-generic-humidity-inert', 'source-unavailable',
              'source-mode-parser', 'source-alias-collision', 'source-scaling-types'):
    setattr(ThermostatRuntimeTests, 'test_' + _mode.replace('-', '_'),
            lambda self, mode=_mode: self.check(mode))
