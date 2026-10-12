"""Execute allocator-pool sampling and production JSON serialization offline."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class MemoryPoolSnapshotTests(unittest.TestCase):
    def test_capability_separation_absent_psram_and_unavailable_backend(self):
        source = (ROOT / 'src/network/handlers/SystemRouteHandler.cpp').read_text(encoding='utf-8')
        serializer = definition(source, 'void fillMemoryPoolSummary(JsonObject memory)')
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        program = r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include "core/MemoryPoolSnapshot.h"
unsigned calls = 0;
bool psram = true;
#if defined(ESP_PLATFORM)
void heap_caps_get_info(multi_heap_info_t* info, uint32_t caps) {
    ++calls;
    // Distinct values make total-heap fallback and missing INTERNAL visible.
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) *info = {19000, 8000, 7000};
    else if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)) *info = {9000, 3000, 2048};
    else if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) *info = psram ? multi_heap_info_t{7000000, 6000000, 4000000} : multi_heap_info_t{};
    else assert(false && "unreviewed memory pool mask");
}
#endif
unsigned long millis() { return 123456; }
SERIALIZER
int main() {
    for (bool present : {true, false}) {
        psram = present;
        for (unsigned n = 0; n < 1000; ++n) {
            auto sample = FastBee::readMemoryPoolSnapshot();
#if defined(ESP_PLATFORM)
            assert(sample.internal8.observed && sample.internal8.freeBytes == 19000);
            assert(sample.internalDma.freeBytes == 9000 && sample.internalDma.largestBlockBytes == 2048);
            assert(sample.psram8.observed && sample.psram8.freeBytes == (present ? 7000000U : 0U));
#else
            assert(!sample.internal8.observed && !sample.internalDma.observed && !sample.psram8.observed);
#endif
        }
        JsonDocument doc;
        fillMemoryPoolSummary(doc.to<JsonObject>());
        auto pools = doc["capabilityPools"];
        assert(pools["sampledAtUptimeMs"] == 123456);
        assert(pools["atomicSnapshot"] == false && pools["overlappingPools"] == true);
        assert(pools["minimumScope"] == "sum_of_heap_low_watermarks");
#if defined(ESP_PLATFORM)
        assert(pools["internal8"]["free"] == 19000 && pools["internal8"]["minFree"] == 8000);
        assert(pools["internal8"]["largestBlock"] == 7000);
        assert(pools["internalDma"]["free"] == 9000 && pools["internalDma"]["minFree"] == 3000);
        assert(pools["psram8"]["free"] == (present ? 7000000U : 0U));
#else
        for (const auto* key : {"internal8", "internalDma", "psram8"}) {
            assert(pools[key]["observed"] == false);
            assert(pools[key]["free"].isNull() && pools[key]["minFree"].isNull() && pools[key]["largestBlock"].isNull());
        }
#endif
    }
#if defined(ESP_PLATFORM)
    assert(calls == 6006);  // Exactly three allocator queries per snapshot.
#else
    assert(calls == 0);
#endif
}
'''.replace('SERIALIZER', serializer)
        with tempfile.TemporaryDirectory(prefix='fastbee-pool-snapshot-') as directory:
            temp = Path(directory)
            (temp / 'esp_heap_caps.h').write_text(
                '#pragma once\n#include <cstdint>\n'
                '#define MALLOC_CAP_INTERNAL 1U\n#define MALLOC_CAP_8BIT 2U\n'
                '#define MALLOC_CAP_DMA 4U\n#define MALLOC_CAP_SPIRAM 8U\n'
                'struct multi_heap_info_t { uint32_t total_free_bytes, minimum_free_bytes, largest_free_block; };\n'
                'void heap_caps_get_info(multi_heap_info_t*, uint32_t);\n', encoding='utf-8')
            cpp = temp / 'sample.cpp'
            cpp.write_text(program, encoding='utf-8')
            binary = temp / ('sample.exe' if os.name == 'nt' else 'sample')
            for target in [[], ['-DESP_PLATFORM=1']]:
                with self.subTest(target=target):
                    result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', *target,
                        '-I' + str(temp), '-I' + str(ROOT / 'include'), '-I' + str(json_header.parent),
                        str(cpp), str(ROOT / 'src/network/FixedJsonField.cpp'), '-o', str(binary)], capture_output=True, text=True, timeout=45)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
