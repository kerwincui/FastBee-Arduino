"""Execute the production fixed slab template with capability mocks."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StaticSlabStorageTests(unittest.TestCase):
    def run_profiles(self, profiles):
        compiler = os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'A C++17 compiler is required')
        cpp = ROOT / 'test/unit/cpp/static_slab_storage_harness.cpp'
        with tempfile.TemporaryDirectory(prefix='fastbee-static-slab-') as directory:
            temp = Path(directory)
            (temp / 'ArduinoJson.h').write_text('#pragma once\nclass JsonDocument {};\n', encoding='utf-8')
            (temp / 'esp_heap_caps.h').write_text(
                '#pragma once\n#include <cstddef>\n#include <cstdint>\n'
                '#define MALLOC_CAP_8BIT 1U\n#define MALLOC_CAP_SPIRAM 2U\n#define MALLOC_CAP_INTERNAL 4U\n'
                'size_t heap_caps_get_total_size(uint32_t);\n'
                'void* heap_caps_aligned_alloc(size_t, size_t, uint32_t);\n'
                'void heap_caps_free(void*);\n', encoding='utf-8')
            for name, flags, dynamic in profiles:
                with self.subTest(profile=name):
                    binary = temp / (name + ('.exe' if os.name == 'nt' else ''))
                    command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(temp), '-I' + str(ROOT / 'include'), *flags, str(cpp), '-o', str(binary)]
                    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('TEMPLATE_BEHAVIOR_PASS', result.stdout)
                    self.assertIn('GLOBAL_DESTRUCTOR_SAFE', result.stdout)
                    self.assertIn('dynamic=' + str(int(dynamic)), result.stdout)

    def test_external_storage_fallback_ownership_and_lifetime(self):
        self.run_profiles([
            ('s3_psram', ['-DARDUINO_ARCH_ESP32', '-DCONFIG_IDF_TARGET_ESP32S3',
                '-DFASTBEE_USE_PSRAM=1', '-DBOARD_HAS_PSRAM', '-DTEST_DYNAMIC'], True),
            ('s3_defined_zero', ['-DARDUINO_ARCH_ESP32', '-DCONFIG_IDF_TARGET_ESP32S3=0', '-DBOARD_HAS_PSRAM', '-DFASTBEE_USE_PSRAM=1', '-DTEST_DYNAMIC'], True),
            ('s3_project_default', ['-DARDUINO_ARCH_ESP32', '-DCONFIG_IDF_TARGET_ESP32S3',
                '-DBOARD_HAS_PSRAM', '-DTEST_DYNAMIC'], True),
        ])

    def test_inline_profiles_and_missing_board_support(self):
        self.run_profiles([
            ('s3_disabled', ['-DARDUINO_ARCH_ESP32', '-DCONFIG_IDF_TARGET_ESP32S3', '-DFASTBEE_USE_PSRAM=0'], False),
            ('s3_without_board', ['-DARDUINO_ARCH_ESP32', '-DCONFIG_IDF_TARGET_ESP32S3', '-DFASTBEE_USE_PSRAM=1'], False),
            ('c6', ['-DARDUINO_ARCH_ESP32', '-DCONFIG_IDF_TARGET_ESP32C6', '-DFASTBEE_USE_PSRAM=1'], False),
            ('classic', ['-DARDUINO_ARCH_ESP32', '-DCONFIG_IDF_TARGET_ESP32', '-DFASTBEE_USE_PSRAM=1'], False),
            ('c3', ['-DARDUINO_ARCH_ESP32', '-DCONFIG_IDF_TARGET_ESP32C3', '-DBOARD_HAS_PSRAM', '-DFASTBEE_USE_PSRAM=1'], False),
            ('native_with_flags', ['-DCONFIG_IDF_TARGET_ESP32S3', '-DBOARD_HAS_PSRAM', '-DFASTBEE_USE_PSRAM=1'], False),
            ('native', [], False),
        ])


if __name__ == '__main__':
    unittest.main()
