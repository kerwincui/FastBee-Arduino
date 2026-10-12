"""Link the real disabled OTA translation unit and its actual owning pointer."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OtaDisabledLifetimeTests(unittest.TestCase):
    def test_disabled_profile_still_links_unique_ptr_deletion(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        self.assertTrue(headers, 'Project ArduinoJson dependency required')
        with tempfile.TemporaryDirectory(prefix='fastbee-ota-disabled-') as directory:
            temp = Path(directory)
            harness = temp / 'disabled.cpp'
            harness.write_text('''#include "network/OTAManager.h"
#include <memory>
#include <cassert>
static void destroyOwner(OTAManager* pointer) {
    // Mirrors the framework's unconditional unique_ptr member/deleter. No OTA
    // constructor, upload, task, network or physical device is invoked.
    std::unique_ptr<OTAManager> owner(pointer);
    assert(!owner);
}
int main() {
    auto before = FastBee::FeatureInstanceRegistry::instance().observe("ota");
    destroyOwner(nullptr);
    auto after = FastBee::FeatureInstanceRegistry::instance().observe("ota");
    assert(!before.observed() && !after.observed());
}
''', encoding='utf-8')
            exe = temp / ('disabled.exe' if os.name == 'nt' else 'disabled')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DFASTBEE_ENABLE_OTA=0', '-I' + str(ROOT / 'test/mocks'),
                '-I' + str(ROOT / 'include'), '-I' + str(headers[0].parent),
                str(ROOT / 'src/network/OTAManager.cpp'), str(harness), '-o', str(exe)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

if __name__ == '__main__':
    unittest.main()
