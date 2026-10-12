"""Draft to install as test/unit/test_ordinary_neopixel.py after r5 thaws.

Real PM admission/retirement + NeoPixel control/frame/transport owner functions.
SDK transport is injected; UART generation and other output backends belong to
their dedicated regressions, and physical light output is not asserted here.
"""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_neopixel_transport import definition

ROOT = Path(__file__).resolve().parents[2]


class OrdinaryNeoPixelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('C++17 compiler required')
        paths = [ROOT / name for name in (
            'src/core/PeripheralManager.cpp', 'src/core/PeripheralControlArbitration.cpp',
            'src/core/PeripheralManagerConfigHelpers.h',
            'test/unit/cpp/neopixel_integration_harness.cpp.in',
            'test/unit/cpp/neopixel_ordinary_harness.cpp.in',
            'include/core/OrdinaryControlPolicy.h', 'include/core/NativeOutputControl.h')]
        cls.paths = paths
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in paths]
        pm, common, helpers, base, fragment = [p.read_text(encoding='utf-8') for p in paths[:5]]
        declarations, cases = fragment.split('// @declarations@', 1)[1].split('// @cases@', 1)
        includes = fragment.split('// @includes@', 1)[1].split('// @declarations@', 1)[0]
        base = base[:base.index('static void animate(')]
        base = base.replace('#include <Arduino.h>', '#include <Arduino.h>\n' + includes)
        base = base.replace('PERIPHERAL_ENABLED };', 'PERIPHERAL_ENABLED, PERIPHERAL_INITIALIZED };')
        base = base.replace('struct PeripheralRuntimeState {', '''struct PeripheralRuntimeState {
    OC::Lease ordinaryControl;
    uint32_t ordinaryStopFailures = 0;
    uint64_t ordinaryRetryAfterMs = 0;
''')
        base = base.replace('status = PeripheralStatus::PERIPHERAL_ENABLED;', 'status = PeripheralStatus::PERIPHERAL_RUNNING;')
        base = base.replace('bool controlNeoPixel(const String&, const String&, const String&);',
            'bool controlNeoPixelFrameLocked(const String&, const String&, const String&);\n' + declarations)
        local = '\n'.join(definition(common, signature) for signature in (
            'bool ordinaryTime(', 'bool ordinaryActorCurrent(', 'void fillResult('))
        actual_common = '\n'.join(definition(common, signature) for signature in (
            'bool PeripheralManager::beginOrdinaryControl(',
            'bool PeripheralManager::ordinaryPermitCurrentLocked(',
            'bool PeripheralManager::retireOrdinaryDigitalLocked(',
            'void PeripheralManager::updateOrdinaryControlResultLocked(',
            'bool PeripheralManager::completeOrdinaryControlLocked(',
            'bool PeripheralManager::inspectOrdinaryControlLocked(',
            'bool PeripheralManager::commitOrdinaryControlLocked(',
            'void PeripheralManager::finishOrdinaryControl('))
        owner = pm[pm.index('static Adafruit_NeoPixel* g_neopixelInstance'):]
        owner = owner[:owner.index('\n#endif')]
        owner += '\n' + definition(pm, 'bool fastbeeNeoPixelProjectPinRetained(')
        color_helpers = definition(pm, 'struct NeoPixelRgb {') + ';\n'
        color_helpers += '\n'.join(definition(pm, sig) for sig in (
            'bool parseNeoPixelColor(', 'NeoPixelRgb rainbowColorAt(', 'NeoPixelRgb applyNeoPixelBrightness('))
        color_helpers += '\n' + '\n'.join(definition(helpers, sig) for sig in (
            'inline uint16_t clampNeoPixelCount(', 'inline uint8_t clampNeoPixelBrightness('))
        replacements = {
            '// @helpers@': color_helpers, '// @owner@': owner,
            '// @solid@': definition(pm, 'bool writeNeoPixelSolid('),
            '// @control@': local + '\n' + actual_common + '\n' + '\n'.join(
                definition(pm, sig) for sig in ('bool PeripheralManager::ordinaryNeoPixelQuiescentLocked(',
                    'bool PeripheralManager::controlNeoPixel(', 'bool PeripheralManager::controlNeoPixelFrameLocked(')),
            '// @setup@': 'return false;', '// @teardown@': 'return false;',
            '// @add-tail@': 'return false;', '// @update-stop@': '(void)oldConfig; (void)oldState; (void)hadOldState; (void)errorMsg;',
            '// @rollback-current@': '(void)resultId; (void)errorMsg;',
        }
        for marker, replacement in replacements.items():
            if marker not in base:
                raise AssertionError('Fixture marker missing: ' + marker)
            base = base.replace(marker, replacement)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-ordinary-neopixel-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'test.cpp'
        source.write_text(base + cases, encoding='utf-8')
        cls.binary = source.with_suffix('.exe')
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra',
            f'-I{ROOT / "test/mocks"}', f'-I{ROOT / "include"}', str(source), '-o', str(cls.binary)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.paths]:
            raise AssertionError('NeoPixel ordinary inputs changed')

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for mode in ('missing', 'brightness_invalid', 'brightness', 'stale_epoch', 'fault',
             'continue', 'terminal', 'stop', 'conflict', 'expired', 'invalid_preserves'):
    setattr(OrdinaryNeoPixelTests, 'test_' + mode, lambda self, mode=mode: self.check(mode))

if __name__ == '__main__':
    unittest.main()
