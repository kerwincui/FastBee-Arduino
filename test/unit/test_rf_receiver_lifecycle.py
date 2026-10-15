"""Actual RF RX driver, ISR generation, storage commit and HTTP fault paths.

This suite does not cover TX or physical RF. Parent runs it with the centralized
gate; the actual FileUtils commit body and production driver/handlers are used.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]
SUPPORT = Path(os.environ.get('FASTBEE_TEST_SUPPORT_ROOT', ROOT))


class RfReceiverLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        if not Path(compiler).is_file():
            raise unittest.SkipTest('C++17 compiler required')
        cls.inputs = [ROOT / p for p in (
            'include/peripherals/drivers/RFLearnDriver.h',
            'src/peripherals/drivers/RFLearnDriver.cpp',
            'src/network/handlers/RfRouteHandler.cpp',
            'test/unit/cpp/rf_receiver_lifecycle_harness.cpp',
            'test/unit/test_rf_receiver_lifecycle.py', 'src/utils/FileUtils.cpp',
            'src/core/PeripheralManager.cpp', 'include/core/FeatureInstanceRegistry.h',
            'test/unit/fixtures/periph_exec_config/LittleFS.h',
            'include/utils/ConsumedStringJsonReader.h')]
        cls.inputs += [ROOT / 'include/utils/SharedJsonDeserializer.h', ROOT / 'src/utils/SharedJsonDeserializer.cpp']
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temporary = tempfile.TemporaryDirectory(prefix='fastbee-rf-receiver-')
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        header = cls.inputs[0].read_text(encoding='utf-8')
        declaration = header[header.index('enum class RfLearnState'):header.index('#endif // FASTBEE_ENABLE_RF_LEARN')]
        (temporary / 'rf_declaration.inc').write_text(declaration.replace('private:', 'public:'), encoding='utf-8')
        lines = [line for line in cls.inputs[1].read_text(encoding='utf-8').splitlines() if not line.startswith('#include ')]
        lines.remove('#if FASTBEE_ENABLE_RF_LEARN')
        lines.remove('#endif // FASTBEE_ENABLE_RF_LEARN')
        (temporary / 'rf_driver.inc').write_text('\n'.join(lines), encoding='utf-8')
        route = cls.inputs[2].read_text(encoding='utf-8')
        utils = (ROOT / 'include/network/handlers/HandlerUtils.h').read_text(encoding='utf-8')
        methods = ('namespace HandlerUtils {\n'
                   + cpp_function(utils, 'inline bool parseUnsignedRangeParam(') + '\n'
                   + cpp_function(utils, 'inline int remoteKeyErrorStatus(') + '\n'
                   + cpp_function(utils, 'inline bool sendRemoteKeyListEnvelope(') + '\n}\n')
        methods += route[route.index('namespace {'):route.index('RfRouteHandler::RfRouteHandler')]
        methods += '\n'.join(cpp_function(route, 'void RfRouteHandler::' + name + '(') for name in (
            'handleStatus', 'handleLearnStart', 'handleLearnStatus', 'handleLearnStop',
            'handleListKeys', 'handleSaveKey', 'handleDeleteKey', 'handleClearKeys'))
        (temporary / 'rf_routes.inc').write_text(methods, encoding='utf-8')
        (temporary / 'rf_file_commit.inc').write_text(cpp_function(cls.inputs[5].read_text(encoding='utf-8'),
            'bool FileUtils::commitAtomicWrite('), encoding='utf-8')
        manager = cls.inputs[6].read_text(encoding='utf-8')
        (temporary / 'rf_pm_init.inc').write_text(cpp_function(manager,
            'if (config.params.rf.mode == RF_MODE_RX) {'), encoding='utf-8')
        stop_start = manager.index('    if (config.type == PeripheralType::RF_MODULE && config.params.rf.mode == RF_MODE_RX &&')
        stop_end = manager.index('#endif', stop_start)
        (temporary / 'rf_pm_stop.inc').write_text(manager[stop_start:stop_end], encoding='utf-8')
        cls.compiler = compiler
        cls.env = dict(os.environ)
        cls.env['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.env.get('PATH', '')
        cls.includes = [temporary, ROOT / 'test/unit/fixtures/periph_exec_config', SUPPORT / 'test/mocks',
                        SUPPORT / '.pio/libdeps/native/ArduinoJson/src', ROOT / 'include']
        cls.binaries = {}
        for events in (0, 1):
            binary = temporary / f'rf-rx-{events}.exe'
            result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-DFASTBEE_ENABLE_RF_LEARN=1', f'-DFASTBEE_ENABLE_PERIPH_EXEC={events}', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1',
                *['-I' + str(p) for p in cls.includes], str(cls.inputs[3]), str(ROOT / 'src/utils/SharedJsonDeserializer.cpp'), '-o', str(binary)],
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60, env=cls.env)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[events] = binary

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('RF RX input drift')

    def run_mode(self, mode, events=1):
        result = subprocess.run([str(self.binaries[events]), mode], capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=15, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rx_without_rule_manager_keeps_learning_and_own_storage(self):
        self.run_mode('learning', events=0)
        self.run_mode('transaction_success', events=0)

    def test_disabled_translation_units(self):
        for source in self.inputs[1:3]:
            result = subprocess.run([self.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DFASTBEE_ENABLE_RF_LEARN=0', '-DFASTBEE_ENABLE_PERIPH_EXEC=0',
                *['-I' + str(p) for p in self.includes], '-c', str(source),
                '-o', str(Path(self.temporary.name) / (source.stem + '-off.o'))],
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60, env=self.env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _mode in ('open_failure', 'short_write', 'rename_failure', 'update_failure', 'delete_failure',
              'clear_failure', 'transaction_success', 'source_conflict', 'concurrent_edit',
              'invalid_load', 'not_mounted', 'key_validation', 'capacity', 'learning',
              'timeout_wrap', 'old_isr_generation', 'generation_exhausted', 'singleton_identity', 'pm_lifecycle', 'stop_dispatch',
              'reentrant_dispatch', 'dispatch_throw', 'snapshot_busy', 'http_failure',
              'http_parameters', 'http_permissions', 'http_snapshot', 'memory_failure'):
    setattr(RfReceiverLifecycleTests, 'test_' + _mode, lambda self, mode=_mode: self.run_mode(mode))


if __name__ == '__main__':
    unittest.main()
