"""Prepared session admission/lock regressions. No device or filesystem import."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "core/ConfigImportSession.h"
#include <cassert>
#include <string>
using S = FastBee::ConfigImportSession;
const char* a = "0123456789abcdef0123456789abcdef";
const char* b = "abcdef0123456789abcdef0123456789";
int main(int argc, char** argv) {
    assert(argc == 2); std::string mode = argv[1]; S s;
    if (mode == "decimal") {
        uint32_t value = 777;
        for (auto input : {"", "-1", "+1", "1.0", "1x", " 1", "01", "129", "9999999"}) {
            assert(!S::decimal(input, 128, value)); assert(value == 777);
        }
        assert(S::decimal("0", 128, value) && value == 0);
        assert(S::decimal("128", 128, value) && value == 128); return 0;
    }
    if (mode == "lock") {
        { S::Guard first(s); assert(first); S::Guard second(s); assert(!second); }
        S::Guard third(s); assert(third); return 0;
    }
    assert(s.inspect(a, "device.json", 0, 2, 3, 100) == S::Admission::Reject);
    assert(s.begin(a, "device.json", 2, 7, 100));
    if (mode == "competing_begin") {
        assert(!s.begin(b, "features.json", 1, 2, 101));
        assert(std::string(s.name) == "device.json" && std::string(s.session) == a); return 0;
    }
    if (mode == "identity_order_size") {
        assert(s.inspect(b, "device.json", 0, 2, 3, 101) == S::Admission::Reject);
        assert(s.inspect(a, "features.json", 0, 2, 3, 101) == S::Admission::Reject);
        assert(s.inspect(a, "device.json", 0, 3, 3, 101) == S::Admission::Reject);
        assert(s.inspect(a, "device.json", 1, 2, 3, 101) == S::Admission::Reject);
        assert(s.inspect(a, "device.json", 0, 2, 7, 101) == S::Admission::Reject);
        assert(s.inspect(a, "device.json", 0, 2, 8, 101) == S::Admission::Reject); return 0;
    }
    assert(s.inspect(a, "device.json", 0, 2, 3, 101) == S::Admission::Append);
    s.appended(3, 101);
    if (mode == "last_replay") {
        assert(s.inspect(a, "device.json", 0, 2, 3, 102) == S::Admission::ReplayLast);
        assert(s.lastOffset == 0 && s.lastLength == 3 && s.received == 3);
        assert(s.inspect(a, "device.json", 0, 2, 4, 102) == S::Admission::Reject); return 0;
    }
    if (mode == "expired_and_replaced") {
        assert(s.inspect(a, "device.json", 1, 2, 4, 60101) == S::Admission::Reject);
        assert(!s.busy(60101)); assert(s.begin(b, "device.json", 1, 2, 60101));
        assert(s.inspect(a, "device.json", 0, 2, 3, 60102) == S::Admission::Reject); return 0;
    }
    if (mode == "failed_no_replay") {
        s.failed(); assert(s.inspect(a, "device.json", 0, 2, 3, 102) == S::Admission::Reject);
        assert(s.inspect(a, "device.json", 1, 2, 4, 102) == S::Admission::Reject); return 0;
    }
    if (mode == "timer_wrap") {
        s.failed(); assert(s.begin(b, "device.json", 1, 2, UINT32_MAX - 10));
        assert(s.busy(20)); assert(!s.busy(60000)); return 0;
    }
    assert(mode == "commit_and_invalidate");
    assert(s.inspect(a, "device.json", 1, 2, 3, 102) == S::Admission::Reject);
    assert(s.inspect(a, "device.json", 1, 2, 4, 102) == S::Admission::Append);
    s.appended(4, 102); s.committed();
    assert(s.inspect(a, "device.json", 1, 2, 4, 103) == S::Admission::ReplayLast);
    assert(s.lastOffset == 3 && s.received == 7);
    assert(s.inspect(a, "device.json", 0, 2, 3, 103) == S::Admission::Reject);
    s.invalidate(); assert(s.inspect(a, "device.json", 1, 2, 4, 103) == S::Admission::Reject);
}
'''


class ConfigImportSessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-import-session-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'main.cpp'
        source.write_text(HARNESS, encoding='utf-8')
        cls.binary = Path(cls.temp.name) / 'session.exe'
        result = subprocess.run([compiler, '-std=c++17', '-I', str(ROOT/'include'),
            str(source), '-o', str(cls.binary)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_routes_hold_guard_and_compare_replay_bytes(self):
        text = (ROOT/'src/network/handlers/SystemRouteHandler.cpp').read_text(encoding='utf-8')
        # D2 重构后 lambda 业务下沉到 handle* 方法，按处理器函数体截取
        handlers = {
            'import': 'handleConfigTransferImport',
            'import-begin': 'handleConfigTransferImportBegin',
            'import-chunk': 'handleConfigTransferImportChunk',
        }
        def route(name):
            value = text[text.index('void SystemRouteHandler::'+handlers[name]+'('):]
            return value[:value.index('\n}\n')]
        for name in ('import', 'import-begin', 'import-chunk'):
            block = route(name)
            self.assertLess(block.index('requireAuth'), block.index('importGuard(ordinaryImport)'))
            self.assertLess(block.index('requireDeveloperMode'), block.index('importGuard(ordinaryImport)'))
            self.assertLess(block.index('importGuard(ordinaryImport)'), block.index('LittleFS.'))
        block = route('import-chunk')
        self.assertLess(block.index('ordinaryImport.inspect('), block.index('LittleFS.open'))
        self.assertIn('existing.seek(ordinaryImport.lastOffset)', block)
        self.assertIn('existing.read() == static_cast<uint8_t>(chunk[i])', block)
        self.assertIn('if (!replay && index == total - 1)', block)
        self.assertNotIn('cleanupStaleConfigFiles()', block)
        self.assertNotIn('LittleFS.remove(', block)


for _case in ('decimal', 'lock', 'competing_begin', 'identity_order_size', 'last_replay',
              'expired_and_replaced', 'failed_no_replay', 'timer_wrap', 'commit_and_invalidate'):
    setattr(ConfigImportSessionTests, 'test_'+_case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
