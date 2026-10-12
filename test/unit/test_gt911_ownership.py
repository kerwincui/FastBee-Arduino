"""Complete production GT911 backend against the real I2C lease manager."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_panel_runtime_regressions import block

ROOT = Path(__file__).resolve().parents[2]

class Gt911OwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT / p for p in ('src/screen/Gt911TouchBackend.cpp',
            'src/core/I2CBusManager.cpp', 'include/core/I2CBusManager.h',
            'include/core/PeripheralConfig.h', 'include/screen/ScreenBackend.h',
            'test/unit/cpp/gt911_ownership_harness.cpp',
            'test/unit/cpp/gt911_ownership_mocks/Arduino.h',
            'test/unit/cpp/gt911_ownership_mocks/Wire.h')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-gt911-owner-')
        cls.addClassCleanup(cls.temp.cleanup)
        base = Path(cls.temp.name)
        (base / 'screen').mkdir(); (base / 'systems').mkdir(); (base / 'core').mkdir()
        (base / 'config.inc').write_text('struct DisplayConfigParams ' + block(cls.inputs[3].read_text(encoding='utf-8'), 'struct DisplayConfigParams') + ';', encoding='utf-8')
        (base / 'screen/ScreenBackend.h').write_text(cls.inputs[4].read_text(encoding='utf-8').replace('#include "core/PeripheralConfig.h"', ''), encoding='utf-8')
        (base / 'core/FeatureFlags.h').write_text('#pragma once\n', encoding='utf-8')
        (base / 'systems/LoggerSystem.h').write_text('#pragma once\nstruct Logger {template<class...Args> void infof(const char*,Args...) {}}; inline Logger LOGGER;\n', encoding='utf-8')
        cls.exe = base / ('case.exe' if os.name == 'nt' else 'case')
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-DFASTBEE_ENABLE_SCREEN_UI=1', '-DARDUINO_ARCH_ESP32=1', f'-I{base}',
            f'-I{ROOT / "test/unit/cpp/gt911_ownership_mocks"}',
            f'-I{ROOT / "test/unit/cpp/io_expander_provider_mocks"}',
            f'-I{ROOT / "test/mocks"}', f'-I{ROOT / "include"}',
            f'-I{ROOT / "src/screen"}', str(cls.inputs[5]), str(cls.inputs[1]), '-o', str(cls.exe)],
            capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Inputs changed during regression')

    def check_mode(self, mode):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

for mode in ('conflict_primary', 'conflict_alternate', 'reserve_primary', 'reserve_alternate',
             'not_found', 'capacity', 'destructor', 'deferred_destructor', 'reinitialize',
             'lost_primary', 'lost_primary_address', 'lost_alternate', 'lost_alternate_address',
             'unused_destructor', 'invalid', 'begin_busy', 'failed_rebegin'):
    setattr(Gt911OwnershipTests, 'test_' + mode, lambda self, mode=mode: self.check_mode(mode))

if __name__ == '__main__':
    unittest.main(verbosity=2)
