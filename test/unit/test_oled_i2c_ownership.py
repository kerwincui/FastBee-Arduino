"""OLED admission and U8g2 callbacks against the real shared bus manager."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_panel_runtime_regressions import block
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]

class OledI2COwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT / p for p in ('src/peripherals/LCDManager.cpp',
            'src/core/I2CBusManager.cpp', 'include/core/I2CBusManager.h', 'include/core/DisplayOutputTypes.h',
            'test/unit/cpp/oled_i2c_ownership_harness.cpp')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        source = cls.inputs[0].read_text(encoding='utf-8')
        callbacks = source[source.index('const void* g_lcdI2cOwner'):source.index('}  // namespace')]
        admission = block(source, 'if (pinsValid)')
        probe = 'bool LCDManager::probeI2cDisplay(bool logWarning)' + block(source, 'bool LCDManager::probeI2cDisplay(')
        methods = '\n'.join(signature + block(source, signature) for signature in (
            'bool LCDManager::refresh()', 'bool LCDManager::setContrast(uint8_t contrast)',
            'bool LCDManager::setDisplayOn(bool on)', 'void LCDManager::performMaintenance(const PeripheralConfig& config)'))
        foreground = '\n'.join(cpp_function(source, signature) for signature in (
            'bool LCDManager::matchesConfig(const PeripheralConfig& config) const',
            'bool LCDManager::quiescent(const PeripheralConfig& config) const',
            'FastBee::DisplayWriteResult LCDManager::write(const PeripheralConfig& config,',
            'void LCDManager::clear()', 'bool LCDManager::showCustomText(const String& content)',
            'bool LCDManager::print(const String& text, uint8_t x, uint8_t y, TextAlign align)',
            'bool LCDManager::printLine(const String& text, uint8_t line)',
            'bool LCDManager::printLines(const String lines[], uint8_t lineCount)',
            'bool LCDManager::showSensorPage(int8_t page)', 'bool LCDManager::showSystemInfo()',
            'bool LCDManager::drawLine(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2)',
            'bool LCDManager::setFont(uint8_t fontIndex)', 'uint8_t LCDManager::getFontHeight() const',
            'uint8_t LCDManager::getMaxLines() const', 'uint8_t LCDManager::getSensorPageCount() const'))
        identity_body = cpp_function(source, 'bool LCDManager::initialize(')
        identity_body = identity_body[identity_body.index('{') + 1:identity_body.index('    // 如果已初始化')]
        identity = 'bool LCDManager::identityAdmission(const PeripheralConfig& config) {\n' + identity_body + 'return true;\n}'
        init_start = source.index('    // 初始化显示屏')
        initialization = source[init_start:source.index('    // 设置默认字体', init_start)]
        harness = cls.inputs[-1].read_text(encoding='utf-8').replace('CALLBACKS', callbacks).replace('IDENTITY_ADMISSION', identity).replace('ADMISSION', admission).replace('PROBE', probe).replace('HIGH_LEVEL_METHODS', methods).replace('INITIALIZATION', initialization).replace('FOREGROUND_METHODS', foreground)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-oled-owner-')
        cls.addClassCleanup(cls.temp.cleanup)
        temporary = Path(cls.temp.name)
        cpp = temporary / 'case.cpp'; cpp.write_text(harness, encoding='utf-8')
        cls.exe = temporary / ('case.exe' if os.name == 'nt' else 'case')
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            f'-I{ROOT / "test/unit/cpp/io_expander_provider_mocks"}', f'-I{ROOT / "test/mocks"}',
            f'-I{ROOT / "include"}', str(cpp), str(ROOT / 'src/core/I2CBusManager.cpp'), '-o', str(cls.exe)],
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

for mode in ('foreground_identity', 'foreground_busy', 'foreground_font', 'foreground_throttle',
             'foreground_commands', 'foreground_short', 'foreground_nack', 'foreground_deadline', 'collision', 'reserved', 'normal', 'short', 'nack', 'wrong_address',
             'lost_lease', 'lost_during_transfer', 'null_payload', 'end_without_start', 'probe_lost',
             'refresh_error', 'contrast_error', 'power_error', 'initialization_error',
             'maintenance_error', 'recovery_error', 'refresh_unfinished'):
    setattr(OledI2COwnershipTests, 'test_' + mode, lambda self, mode=mode: self.check_mode(mode))

if __name__ == '__main__':
    unittest.main(verbosity=2)
