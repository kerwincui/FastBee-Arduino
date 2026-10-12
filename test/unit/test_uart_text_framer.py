"""Prepared UART text framing regressions; no serial access or rule execution."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "core/UartTextFramer.h"
#include <cassert>
#include <string>
#include <vector>
using F = FastBee::UartTextFramer<160>;
int main(int argc, char** argv) {
    assert(argc == 2); const std::string mode = argv[1];
    F frame; std::vector<std::string> emitted;
    unsigned tooLong = 0, invalid = 0;
    auto consume = [&](F::Result result) {
        if (result == F::Result::Ready) {
            emitted.emplace_back(frame.data(), frame.size()); frame.clear();
        }
        if (result == F::Result::TooLong) ++tooLong;
        if (result == F::Result::InvalidByte) ++invalid;
    };
    auto feed = [&](const std::string& text, uint32_t now) {
        for (unsigned char byte : text) consume(frame.push(byte, now));
    };
    if (mode == "crlf") {
        feed("a\r\nb\n\rc\r", 10);
        assert((emitted == std::vector<std::string>{"a", "b", "c"}));
    } else if (mode == "exact_capacity") {
        feed(std::string(160, 'x'), 10); assert(emitted.empty()); feed("\n", 20);
        assert(emitted.size() == 1 && emitted[0].size() == 160 && !tooLong);
    } else if (mode == "oversized_line") {
        feed(std::string(161, 'x') + "suffix\nOK\n", 10);
        assert(tooLong == 1 && (emitted == std::vector<std::string>{"OK"}));
    } else if (mode == "long_chunks") {
        for (unsigned i = 0; i < 100; ++i) feed(std::string(64, 'x'), i * 100);
        assert(tooLong == 1 && emitted.empty() && frame.discarding());
        feed("\nnew\n", 10001); assert(emitted.size() == 1 && emitted[0] == "new");
    } else if (mode == "nul") {
        feed(std::string("head\0tail\nOK\n", 13), 10);
        assert(invalid == 1 && emitted.size() == 1 && emitted[0] == "OK");
    } else if (mode == "idle_exact") {
        feed("hello", 100); consume(frame.idle(599)); assert(emitted.empty());
        consume(frame.idle(600)); consume(frame.idle(900));
        assert(emitted.size() == 1 && emitted[0] == "hello");
    } else if (mode == "idle_at_zero") {
        feed("hello", 0); consume(frame.idle(500)); assert(emitted.size() == 1);
    } else if (mode == "idle_wrap") {
        feed("hello", UINT32_MAX - 100); consume(frame.idle(398)); assert(emitted.empty());
        consume(frame.idle(399)); assert(emitted.size() == 1);
    } else if (mode == "overflow_idle") {
        feed(std::string(161, 'x'), 100); consume(frame.idle(600));
        assert(emitted.empty() && !frame.discarding());
        feed("OK\n", 601); assert(emitted.size() == 1 && emitted[0] == "OK");
    } else if (mode == "read_failure") {
        feed("old", 1); frame.discard(2); feed("tail\nOK\n", 3);
        assert(emitted.size() == 1 && emitted[0] == "OK");
    } else if (mode == "source_changed") {
        feed("old", 1); frame.discard(2); feed("new-partial", 3);
        consume(frame.idle(503)); assert(emitted.empty());
        feed("fresh\n", 504); assert(emitted.size() == 1 && emitted[0] == "fresh");
    } else if (mode == "utf8_bytes") {
        feed(std::string(157, 'x') + "\xe4\xb8\xad\n", 1);
        assert(emitted.size() == 1 && emitted[0].size() == 160);
        feed(std::string(158, 'x') + "\xe4\xb8\xad\n", 2);
        assert(emitted.size() == 1 && tooLong == 1);
    } else if (mode == "preserve_spaces") {
        feed("  text  \n", 1); assert(emitted.size() == 1 && emitted[0] == "  text  ");
    } else {
        assert(mode == "fragmented"); feed("hel", 1); feed("lo", 100); feed("\n", 200);
        assert(emitted.size() == 1 && emitted[0] == "hello");
    }
}
'''


class UartTextFramerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-uart-text-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'main.cpp'
        source.write_text(HARNESS, encoding='utf-8')
        cls.binary = Path(cls.temp.name) / 'framer.exe'
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT/'include'), str(source), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_scheduler_preserves_framing_and_publication_boundaries(self):
        text = (ROOT/'src/core/PeriphExecScheduler.cpp').read_text(encoding='utf-8')
        start = text.index('void PeriphExecScheduler::checkSerialEvents()')
        body = text[start:text.index('// ========== 数据上报', start)]
        self.assertNotIn('frame.substring', body)
        failed_read = body[body.index('if (!readOk) {'):body.index('if (len > 0) {')]
        self.assertIn('if (generation) Diag::record(Diag::ReadFailure);', failed_read)
        self.assertIn('rx.discard(now); continue;', failed_read)
        self.assertIn('source.configEpoch != epoch', body)
        self.assertIn('if (!len && FastBee::SensorConfigEpoch::current() == epoch', body)
        self.assertLess(body.index('RecursiveMutexGuard sourceGuard'), body.index('readOk = generation && pm.readData('))
        self.assertIn('source.receiveGeneration != generation', body)
        self.assertGreaterEqual(body.count('pm.getUartReceiveGeneration(id) != generation'), 2)
        self.assertIn('frame.length() != inputSize', body)
        self.assertLess(body.index('doc.overflowed()'), body.index('_manager->handlePollData('))
        self.assertIn('_serializeBuffer.length() != expected', body)
        self.assertNotIn('frame.c_str()', body)
        iterator = body[body.index('pm.forEachPeripheral('):body.index('\n    });')]
        self.assertNotIn('dispatchFrame(', iterator)
        self.assertNotIn('readData(', iterator)
        self.assertLess(body.index('Diag::record(Diag::DispatchAttempt)'),
                        body.index('_manager->handlePollData('))
        for event in ('StaleFrame', 'SourceChange', 'SerializationFailure',
                      'OversizedFrame', 'InvalidByteFrame'):
            self.assertIn('Diag::record(Diag::' + event + ')', body)


for _case in ('crlf', 'exact_capacity', 'oversized_line', 'long_chunks', 'nul', 'idle_exact',
              'idle_at_zero', 'idle_wrap', 'overflow_idle', 'read_failure', 'source_changed',
              'utf8_bytes', 'preserve_spaces', 'fragmented'):
    setattr(UartTextFramerTests, 'test_'+_case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
