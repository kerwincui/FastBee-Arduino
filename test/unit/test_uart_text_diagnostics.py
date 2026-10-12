"""Prepared real boot counters and JSON regressions; never opens UARTs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <atomic>
#include <cstddef>
#include <cstdint>
// Test-only access to seed saturation, never a production counter reset API.
#define private public
#include "core/UartTextDiagnostics.h"
#undef private
#include "network/UartTextStatus.h"
#include <cassert>
#include <string>
#include <thread>
#include <vector>
using D = FastBee::UartTextDiagnostics;
extern void recordFromOtherUnit();
int main(int argc, char** argv) {
    assert(argc == 2); const std::string mode = argv[1];
    if (mode == "zero") {
        const auto s = D::snapshot(); assert(!s.lowerBounds);
        for (auto v : s.values) assert(v == 0);
    } else if (mode == "independent") {
        for (size_t i = 0; i < D::Count; ++i)
            for (size_t n = 0; n <= i; ++n) D::record(static_cast<D::Event>(i));
        const auto s = D::snapshot(); assert(!s.lowerBounds);
        for (size_t i = 0; i < D::Count; ++i) assert(s.values[i] == i + 1);
    } else if (mode == "unknown") {
        D::record(D::Count); D::record(static_cast<D::Event>(SIZE_MAX));
        for (auto v : D::snapshot().values) assert(v == 0);
    } else if (mode == "cross_unit") {
        D::record(D::ReadFailure); recordFromOtherUnit();
        assert(D::snapshot().values[D::ReadFailure] == 2);
    } else if (mode == "concurrent") {
        std::vector<std::thread> writers;
        for (unsigned i = 0; i < 8; ++i) writers.emplace_back([] {
            for (unsigned n = 0; n < 10000; ++n) {
                D::record(D::DispatchAttempt); D::record(D::ReadFailure);
                // Concurrent snapshots must not reset or invent counts.
                const auto s = D::snapshot(); assert(!s.lowerBounds);
                assert(s.values[D::DispatchAttempt] <= 80000);
            }
        });
        for (auto& writer : writers) writer.join();
        const auto s = D::snapshot();
        assert(s.values[D::DispatchAttempt] == 80000 && s.values[D::ReadFailure] == 80000);
        assert(s.values[D::StaleFrame] == 0);
    } else if (mode == "saturation") {
        D::counters()[D::OversizedFrame].store(UINT32_MAX - 2);
        std::vector<std::thread> writers;
        for (unsigned i = 0; i < 8; ++i) writers.emplace_back([] {
            for (unsigned n = 0; n < 100; ++n) D::record(D::OversizedFrame);
        });
        for (auto& writer : writers) writer.join();
        assert(D::snapshot().values[D::OversizedFrame] == UINT32_MAX);
        assert(D::snapshot().lowerBounds);
        D::record(D::ReadFailure); assert(D::snapshot().values[D::ReadFailure] == 1);
    } else {
        assert(mode == "json");
        for (size_t i = 0; i < D::Count; ++i) D::counters()[i].store(i + 1);
        const char* names[] = {"dispatchAttempts", "oversizedFrames", "invalidByteFrames",
            "readFailures", "sourceChanges", "staleFrames", "serializationFailures",
            "hardwareFifoFaults", "hardwareBufferFaults", "hardwareFrameFaults",
            "hardwareParityFaults", "hardwareBreakFaults"};
        auto check = [&](bool saturated) {
            const String json = FastBee::uartTextStatusJson(); JsonDocument doc;
            assert(!deserializeJson(doc, json.c_str()));
            assert(doc["available"] == true && doc["scope"] == "boot" && doc["unit"] == "events");
            assert(doc["peerVerified"] == false && doc["maxFrameBytes"] == 160 && doc["idleBoundaryMs"] == 500);
            assert(doc["countsAreLowerBounds"].as<bool>() == saturated);
            for (size_t i = 0; i < D::Count; ++i)
                assert(doc[names[i]].as<uint32_t>() == (saturated && i == 0 ? UINT32_MAX : i + 1));
            assert(doc["hardwareErrorScope"] == "first_observed_generic_stream_fault");
            assert(doc["hardwareErrorRecovery"] == "explicit_restart");
            assert(doc["hardwareErrorDeliveryConfirmed"] == false);
            assert(doc.size() == 22); // Metadata and counters only; no raw payload or IDs.
        };
        check(false); D::counters()[0].store(UINT32_MAX); check(true);
    }
}
'''
OTHER_UNIT = r'''
#include "core/UartTextDiagnostics.h"
void recordFromOtherUnit() {
    FastBee::UartTextDiagnostics::record(FastBee::UartTextDiagnostics::ReadFailure);
}
'''


class UartTextDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-uart-diagnostics-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'main.cpp'
        source.write_text(HARNESS, encoding='utf-8')
        other = Path(cls.temp.name) / 'other.cpp'
        other.write_text(OTHER_UNIT, encoding='utf-8')
        cls.binary = Path(cls.temp.name) / 'diagnostics.exe'
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), '-I'+json_root,
            str(source), str(other), '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('zero', 'independent', 'unknown', 'cross_unit', 'concurrent', 'saturation', 'json'):
    setattr(UartTextDiagnosticsTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
