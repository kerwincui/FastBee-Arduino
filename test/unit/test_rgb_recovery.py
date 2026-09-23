"""Run the actual recovery state machine against deterministic DMA/IRQ mocks."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RgbRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which('g++')
        if not cls.compiler:
            raise unittest.SkipTest('g++ is required')
        cls.temporary = tempfile.TemporaryDirectory(prefix='fastbee-rgb-recovery-')
        cls.directory = Path(cls.temporary.name)
        cls.environment = dict(os.environ)
        cls.environment['PATH'] = str(Path(cls.compiler).parent) + os.pathsep + cls.environment.get('PATH', '')
        driver = (ROOT / 'src/screen/RgbPanelDriver.c').read_text(encoding='utf-8')
        sections = []
        for name in ('STATE', 'IMPLEMENTATION'):
            sections.append(driver.split(f'/* FASTBEE_RGB_RECOVERY_{name}_BEGIN */', 1)[1]
                            .split(f'/* FASTBEE_RGB_RECOVERY_{name}_END */', 1)[0])
        (cls.directory / 'rgb_recovery_production.inc').write_text('\n'.join(sections), encoding='utf-8')
        cls.binary = cls.directory / 'recovery.exe'
        command = [cls.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-DFASTBEE_RGB_RESTART_ON_DEMAND=1', '-DFASTBEE_USE_PSRAM=1',
                   '-DFASTBEE_ENABLE_SCREEN_UI=1', '-DCONFIG_IDF_TARGET_ESP32S3=1',
                   f'-I{ROOT / "include"}', f'-I{cls.directory}',
                   str(ROOT / 'test/unit/cpp/rgb_recovery_harness.cpp'), '-o', str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True, env=cls.environment, timeout=60)
        if result.returncode:
            cls.temporary.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_production_recovery_scenarios(self):
        for scenario in ('one_startup_not_periodic', 'latched_cooldown', 'persistent_events_bounded',
                         'request_error_retry', 'reset_error_not_applied', 'start_error_not_applied',
                         'manual_bypasses_grace_cooldown', 'timer_wrap', 'unrelated_dma_ignored',
                         'detach_and_failed_attach', 'events_while_request_in_flight',
                         'failed_manual_retry_is_bounded'):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.binary), scenario], capture_output=True,
                                        text=True, env=self.environment, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_disabled_header_is_self_contained_and_has_no_link_dependency(self):
        source = self.directory / 'disabled.cpp'
        source.write_text('''#include "screen/RgbRecovery.h"
#include <cassert>
int main() { FastBeeRgbRecoveryStats s{}; s.active = true;
assert(fastbee_rgb_recovery_attach(nullptr, 0)); fastbee_rgb_recovery_service(3000);
fastbee_rgb_recovery_detach(nullptr); assert(!fastbee_rgb_recovery_request());
assert(!fastbee_rgb_recovery_get_stats(&s) && !s.active && s.requested == 0); }
''', encoding='utf-8')
        variants = ([], ['-DCONFIG_IDF_TARGET_ESP32C3=1'],
                    ['-DCONFIG_IDF_TARGET_ESP32S3=1', '-DFASTBEE_USE_PSRAM=1', '-DFASTBEE_RGB_RESTART_ON_DEMAND=0'],
                    ['-DCONFIG_IDF_TARGET_ESP32S3=1', '-DFASTBEE_USE_PSRAM=0', '-DFASTBEE_RGB_RESTART_ON_DEMAND=1'],
                    ['-DCONFIG_IDF_TARGET_ESP32S3=1', '-DFASTBEE_USE_PSRAM=1', '-DFASTBEE_RGB_RESTART_ON_DEMAND=1', '-DFASTBEE_ENABLE_SCREEN_UI=0'])
        for index, flags in enumerate(variants):
            with self.subTest(flags=flags):
                binary = self.directory / f'disabled-{index}.exe'
                result = subprocess.run([self.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                         f'-I{ROOT / "include"}', *flags, str(source), '-o', str(binary)],
                                        capture_output=True, text=True, env=self.environment, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True,
                                        env=self.environment, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_actual_wrapper_compiles_with_installed_s3_sdk(self):
        packages = Path.home() / '.platformio/packages'
        sdk = packages / 'framework-arduinoespressif32-libs/esp32s3'
        toolchain = packages / 'toolchain-xtensa-esp-elf/bin'
        compiler = toolchain / 'xtensa-esp32s3-elf-gcc.exe'
        nm = toolchain / 'xtensa-esp32s3-elf-nm.exe'
        if not compiler.exists() or not sdk.exists():
            self.skipTest('Installed S3 SDK required for target compile check')
        command = [str(compiler), '-Os', '-DESP_PLATFORM',
                   '-DSOC_MMU_PAGE_SIZE=CONFIG_MMU_PAGE_SIZE', '-DSOC_XTAL_FREQ_MHZ=CONFIG_XTAL_FREQ']
        command += shlex.split((sdk / 'flags/c_flags').read_text())
        command += ['-iprefix', str(sdk / 'include') + '/']
        command += shlex.split((sdk / 'flags/includes').read_text())
        command += ['-I', str(sdk / 'qio_opi/include'), '-I', str(ROOT / 'include')]
        for psram, enabled in ((1, 1), (0, 1), (1, 0)):
            with self.subTest(psram=psram, enabled=enabled):
                target = self.directory / f'wrapper-{psram}-{enabled}.o'
                result = subprocess.run(command + [f'-DFASTBEE_USE_PSRAM={psram}',
                                        f'-DFASTBEE_ENABLE_SCREEN_UI={enabled}',
                                        '-DFASTBEE_RGB_RESTART_ON_DEMAND=1', '-c',
                                        str(ROOT / 'src/screen/RgbPanelDriver.c'), '-o', str(target)],
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                symbols = subprocess.check_output([str(nm), '-S', str(target)], text=True)
                self.assertEqual(' T fastbee_rgb_recovery_service' in symbols, bool(psram and enabled))


if __name__ == '__main__':
    unittest.main()
