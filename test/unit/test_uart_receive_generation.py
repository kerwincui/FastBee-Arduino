"""Prepared hardware-stream identity and buffered-frame recovery regressions."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <atomic>
#include <cstdint>
// Reach the allocator only in this isolated host fixture to exercise exhaustion.
#define private public
#include "core/UartReceiveGeneration.h"
#undef private
#include "core/UartTextFramer.h"
#include <cassert>
#include <string>
int main(int argc, char** argv) {
    assert(argc == 2); const std::string mode = argv[1];
    FastBee::UartReceiveGeneration first, second;
    assert(!first.begin(0) && !first.begin(3));
    assert(first.current(0) == 0 && first.current(255) == 0);
    assert(first.begin(1)); const auto initial = first.current(1);
    assert(initial != 0);
    if (mode == "exhaustion") {
        FastBee::UartReceiveGeneration::counter().store(UINT32_MAX - 1);
        assert(second.begin(2)); assert(second.current(2) == UINT32_MAX - 1);
        assert(!first.begin(1)); assert(first.current(1) == initial);
        second.end(2); assert(!second.begin(2) && second.current(2) == 0);
        assert(FastBee::UartReceiveGeneration::counter().load() == UINT32_MAX);
    } else {
        assert(mode == "same_config_restart");
        FastBee::UartTextFramer<160> frame;
        frame.push('o', 10); frame.push('l', 10); frame.push('d', 10);
        assert(first.begin(1)); // Re-begin without releasing the controller lease.
        assert(first.current(1) != initial);
        frame.discard(20); // Production resets the partial frame on mismatch.
        assert(frame.push('n', 21) == decltype(frame)::Result::None);
        assert(frame.push('\n', 21) == decltype(frame)::Result::None);
        assert(frame.size() == 0);
        frame.push('O', 22); frame.push('K', 22);
        assert(frame.push('\n', 22) == decltype(frame)::Result::Ready);
        assert(std::string(frame.data()) == "OK");
        const auto beforeStop = first.current(1); first.end(1);
        assert(first.current(1) == 0 && first.begin(1));
        assert(first.current(1) != beforeStop);
        assert(second.begin(1) && second.current(1) != first.current(1));
    }
}
'''


class UartReceiveGenerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-uart-generation-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'main.cpp'
        source.write_text(HARNESS, encoding='utf-8')
        cls.binary = Path(cls.temp.name) / 'identity.exe'
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT/'include'), str(source), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise RuntimeError(built.stdout + built.stderr)

    def check(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_same_config_restart(self):
        self.check('same_config_restart')

    def test_exhaustion_never_reuses_generation_or_changes_existing_stream(self):
        self.check('exhaustion')

if __name__ == '__main__':
    unittest.main()
