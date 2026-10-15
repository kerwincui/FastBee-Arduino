"""Execute the actual IR receiver and HTTP methods against storage/lifecycle faults.

The receiver SDK and RTOS are controlled host doubles; the production class,
map parser/editor, snapshots, learning, check, stop and routes are compiled intact.
No physical reception, output or device traffic is performed.
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


class IrReceiverLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        if not Path(compiler).is_file():
            raise unittest.SkipTest('C++17 compiler required')
        cls.inputs = [ROOT / p for p in (
            'include/peripherals/drivers/IRRemoteDriver.h',
            'src/peripherals/drivers/IRRemoteDriver.cpp',
            'src/network/handlers/IrRouteHandler.cpp',
            'test/unit/cpp/ir_receiver_lifecycle_harness.cpp',
            'test/unit/test_ir_receiver_lifecycle.py')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temporary = tempfile.TemporaryDirectory(prefix='fastbee-ir-receiver-')
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        header = cls.inputs[0].read_text(encoding='utf-8')
        declaration = header[header.index('enum class IrLearnState'):header.index('#endif // FASTBEE_ENABLE_IR_REMOTE')]
        (temporary / 'ir_declaration.inc').write_text(declaration.replace('private:', 'public:'), encoding='utf-8')
        source = cls.inputs[1].read_text(encoding='utf-8')
        lines = [line for line in source.splitlines() if not line.startswith('#include ')]
        lines.remove('#if FASTBEE_ENABLE_IR_REMOTE')
        lines.remove('#endif // FASTBEE_ENABLE_IR_REMOTE')
        (temporary / 'ir_driver.inc').write_text('\n'.join(lines), encoding='utf-8')
        route = cls.inputs[2].read_text(encoding='utf-8')
        utils = (ROOT / 'include/network/handlers/HandlerUtils.h').read_text(encoding='utf-8')
        methods = ('namespace HandlerUtils {\n'
                   + cpp_function(utils, 'inline bool parseUnsignedRangeParam(') + '\n'
                   + cpp_function(utils, 'inline int remoteKeyErrorStatus(') + '\n'
                   + cpp_function(utils, 'inline bool sendRemoteKeyListEnvelope(') + '\n}\n')
        methods += route[route.index('namespace {'):route.index('IrRouteHandler::IrRouteHandler')]
        methods += '\n'.join(cpp_function(route, 'void IrRouteHandler::' + name + '(') for name in (
            'handleStatus', 'handleLearnStart', 'handleLearnStatus', 'handleLearnStop',
            'handleListKeys', 'handleSaveKey', 'handleDeleteKey', 'handleClearKeys'))
        (temporary / 'ir_routes.inc').write_text(methods, encoding='utf-8')
        cls.env = dict(os.environ)
        cls.env['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.env.get('PATH', '')
        includes = [temporary, SUPPORT / 'test/mocks', SUPPORT / '.pio/libdeps/native/ArduinoJson/src', ROOT / 'include']
        cls.binaries = {}
        cls.compiler, cls.includes = compiler, includes
        for enabled in (0, 1):
            binary = temporary / f'ir-{enabled}.exe'
            compiled = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                f'-DFASTBEE_ENABLE_PERIPH_EXEC={enabled}', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1',
                *['-I' + str(p) for p in includes], str(cls.inputs[3]), '-o', str(binary)],
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60, env=cls.env)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
            cls.binaries[enabled] = binary
        cls.binary = cls.binaries[1]

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('IR production inputs changed during tests')

    def run_mode(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=15, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_event_store_disabled_without_manager(self):
        result = subprocess.run([str(self.binaries[0]), 'event_store_disabled'], capture_output=True,
            text=True, encoding='utf-8', errors='replace', timeout=15, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ir_disabled_translation_units_need_no_receiver_or_http_sdk(self):
        for source in self.inputs[1:3]:
            result = subprocess.run([self.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DFASTBEE_ENABLE_IR_REMOTE=0', '-DFASTBEE_ENABLE_PERIPH_EXEC=0',
                *['-I' + str(p) for p in self.includes], '-c', str(source),
                '-o', str(Path(self.temporary.name) / (source.stem + '-off.o'))],
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60, env=self.env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _mode in ('upsert_failure', 'update_failure', 'delete_failure', 'clear_failure',
              'transaction_success', 'cas_conflict', 'pending_import', 'concurrent_edit',
              'invalid_load', 'capacity', 'memory_failure', 'learning_capture',
              'timeout_wrap', 'stop_dispatch', 'reentrant_dispatch', 'dispatch_throw',
              'learn_stop_race', 'snapshot_busy', 'http_persistence', 'http_permissions',
              'http_timeout', 'http_snapshot', 'keys_without_receiver'):
    setattr(IrReceiverLifecycleTests, 'test_' + _mode,
            lambda self, mode=_mode: self.run_mode(mode))


if __name__ == '__main__':
    unittest.main()
