"""Prepared complete production PCA manager body with real provider/session/bus.

The manager environment, feature admission, native OE and Wire are explicit
seams. This does not verify HTTP, target firmware linking or physical outputs.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PCA9685ManagerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        cls.inputs = [ROOT / p for p in (
            'src/core/PeripheralManagerPwm.cpp', 'src/core/Pca9685ConfigSupport.cpp',
            'include/core/Pca9685ConfigSupport.h', 'include/core/PeripheralConfig.h',
            'include/core/H1ParameterContract.h', 'include/core/H1ParameterJson.h',
            'include/core/NativeOutputControl.h', 'include/core/OrdinaryControlPolicy.h',
            'include/peripherals/PCA9685Provider.h', 'include/peripherals/drivers/PCA9685Device.h',
            'include/peripherals/drivers/PCA9685OutputSession.h', 'include/peripherals/drivers/PCA9685Contract.h',
            'include/core/I2CBusManager.h', 'src/core/I2CBusManager.cpp',
            'test/unit/cpp/pca9685_manager_harness.cpp', 'test/unit/test_pca9685_manager.py')]
        for folder in ('test/mocks', 'test/unit/cpp/pca9685_device_mocks',
                       'test/unit/cpp/mcp23017_device_mocks/freertos'):
            cls.inputs += sorted(p for p in (ROOT / folder).rglob('*') if p.is_file())
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-pca-manager-')
        cls.addClassCleanup(cls.temp.cleanup)
        stage = Path(cls.temp.name)
        source = (ROOT / 'src/core/PeripheralManagerPwm.cpp').read_text(encoding='utf-8-sig')
        # Preserve all production helpers/methods. Only headers are replaced
        # by the harness's environment declarations; no behavior is copied.
        body = source[source.index('\nnamespace {'):]
        (stage / 'pca_manager_under_test.inc').write_text(body, encoding='utf-8')
        cls.binary = stage / ('manager.exe' if os.name == 'nt' else 'manager')
        built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-DFASTBEE_ENABLE_IO_EXPANDER=1',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I' + str(stage), '-I' + str(ROOT / 'test/unit/cpp/pca9685_device_mocks'),
            '-I' + str(ROOT / 'test/unit/cpp/mcp23017_device_mocks'),
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'), '-I' + json_root,
            str(ROOT / 'test/unit/cpp/pca9685_manager_harness.cpp'),
            str(ROOT / 'src/core/Pca9685ConfigSupport.cpp'),
            str(ROOT / 'src/core/I2CBusManager.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('PCA manager inputs changed during tests')

    def check(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=15)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _mode in ('wrong_type', 'lifecycle', 'stale_review', 'expired_review',
              'gate_lost_ready', 'gate_lost_enabled', 'unexpected_pad', 'busy_wake',
              'changed_config', 'release_retry', 'new_identity', 'shutdown_fence',
              'no_context', 'whole_device_conflict', 'invalid_before_admission',
              'emergency_revokes_invocation', 'closed_keeps_failure', 'prepare_pending_invocation'):
    setattr(PCA9685ManagerTests, 'test_' + _mode, lambda self, mode=_mode: self.check(mode))

if __name__ == '__main__':
    unittest.main()
