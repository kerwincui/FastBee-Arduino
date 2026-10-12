"""Compile the actual staticPools serializer with the production slab template."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StaticSlabDiagnosticsTests(unittest.TestCase):
    def test_actual_serializer_external_fallback_absence_and_inline_profiles(self):
        compiler = os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'A C++17 compiler is required')
        route = (ROOT / 'src/network/handlers/SystemRouteHandler.cpp').read_text(encoding='utf-8')
        start = route.index('    JsonObject pools = memory["staticPools"].to<JsonObject>();')
        end = route.index('#undef FB_FILL_POOL_STATS', start) + len('#undef FB_FILL_POOL_STATS')
        serializer = route[start:end]
        program = (ROOT / 'test/unit/cpp/static_slab_storage_diagnostics_harness.cpp.in').read_text(encoding='utf-8')
        program = program.replace('@PRODUCTION_POOL_SERIALIZER@', serializer)
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        profiles = [
            ('s3', ['-DARDUINO_ARCH_ESP32','-DCONFIG_IDF_TARGET_ESP32S3','-DBOARD_HAS_PSRAM','-DFASTBEE_USE_PSRAM=1','-DTEST_DYNAMIC'],
                ['external','fallback','unavailable','not-ready']),
            ('s3_defined_zero', ['-DARDUINO_ARCH_ESP32','-DCONFIG_IDF_TARGET_ESP32S3=0','-DBOARD_HAS_PSRAM','-DFASTBEE_USE_PSRAM=1','-DTEST_DYNAMIC'], ['external','fallback','unavailable','not-ready']),
            ('s3_disabled', ['-DARDUINO_ARCH_ESP32','-DCONFIG_IDF_TARGET_ESP32S3','-DBOARD_HAS_PSRAM','-DFASTBEE_USE_PSRAM=0'], ['inline']),
            ('s3_no_board', ['-DARDUINO_ARCH_ESP32','-DCONFIG_IDF_TARGET_ESP32S3','-DFASTBEE_USE_PSRAM=1'], ['inline']),
            ('c6', ['-DARDUINO_ARCH_ESP32','-DCONFIG_IDF_TARGET_ESP32C6','-DFASTBEE_USE_PSRAM=1'], ['inline']),
            ('classic', ['-DARDUINO_ARCH_ESP32','-DCONFIG_IDF_TARGET_ESP32','-DBOARD_HAS_PSRAM','-DFASTBEE_USE_PSRAM=1'], ['inline']),
            ('c3', ['-DARDUINO_ARCH_ESP32','-DCONFIG_IDF_TARGET_ESP32C3','-DBOARD_HAS_PSRAM','-DFASTBEE_USE_PSRAM=1'], ['inline']),
            ('native_with_flags', ['-DCONFIG_IDF_TARGET_ESP32S3','-DBOARD_HAS_PSRAM','-DFASTBEE_USE_PSRAM=1'], ['inline']),
            ('native', [], ['inline']),
        ]
        with tempfile.TemporaryDirectory(prefix='fastbee-static-slab-diagnostics-') as directory:
            temp = Path(directory)
            (temp / 'esp_heap_caps.h').write_text(
                '#pragma once\n#include <cstddef>\n#include <cstdint>\n'
                '#define MALLOC_CAP_8BIT 1U\n#define MALLOC_CAP_SPIRAM 2U\n#define MALLOC_CAP_INTERNAL 4U\n'
                'size_t heap_caps_get_total_size(uint32_t);\nvoid* heap_caps_aligned_alloc(size_t,size_t,uint32_t);\nvoid heap_caps_free(void*);\n', encoding='utf-8')
            cpp = temp / 'diagnostics.cpp'
            cpp.write_text(program, encoding='utf-8')
            for name, flags, modes in profiles:
                with self.subTest(profile=name):
                    binary = temp / (name + ('.exe' if os.name == 'nt' else ''))
                    result = subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                        '-I'+str(temp),'-I'+str(ROOT/'include'),'-I'+str(json_header.parent),*flags,
                        str(cpp),str(ROOT/'src/network/FixedJsonField.cpp'),'-o',str(binary)],
                        capture_output=True,text=True,timeout=60)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    for mode in modes:
                        result = subprocess.run([str(binary),mode],capture_output=True,text=True,timeout=20)
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                        self.assertIn('ACTUAL_POOL_SERIALIZER_PASS',result.stdout)


if __name__ == '__main__':
    unittest.main()
