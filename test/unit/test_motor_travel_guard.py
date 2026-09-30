"""Offline pulse-domain guard tests; no calibration or device movement implied."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MotorTravelGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-motor-travel-')
        directory = Path(cls.temp.name)
        cls.binary = directory / 'guard.exe'
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        cls.env = dict(os.environ)
        cls.env['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.env.get('PATH', '')
        source = directory / 'guard.cpp'
        source.write_text(r'''
#include "core/MotorTravelGuard.h"
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdlib>
using FastBee::MotorTravelGuard;
int main(int argc, char** argv) {
    assert(argc == 2);
    const int mode = std::atoi(argv[1]);
    MotorTravelGuard guard;
    if (mode == 0) {
        assert(!guard.available(true, 10) && !guard.reserve(true, 10));
        assert(!guard.configure(0, 0) && !guard.confirm(0));
        assert(!guard.configure(10, -10) && !guard.confirm(0));
        assert(guard.configure(0, 1000) && !guard.available(true, 10));
        assert(guard.confirm(500) && guard.available(true, 10) == 10);
        assert(!guard.confirm(1001) && !guard.referenced);
        assert(!guard.available(false, 10));
    } else if (mode == 1) {
        assert(guard.configure(-100, 100) && guard.confirm(90));
        assert(guard.available(true, 50) == 10);
        assert(!guard.reserve(true, 50) && guard.lower == 90 && guard.upper == 90);
        assert(guard.reserve(true, 10) && guard.lower == 90 && guard.upper == 100);
        assert(!guard.available(true, 1));
        assert(guard.available(false, 200) == 190);
        assert(guard.reserve(false, 190) && guard.lower == -100 && guard.upper == 100);
        assert(!guard.available(true, 1) && !guard.available(false, 1));
    } else if (mode == 2) {
        assert(guard.configure(0, 1000) && guard.confirm(500));
        assert(guard.reserve(true, 100));
        // An early stop or stalled move may leave the shaft at any point 500..600.
        assert(guard.lower == 500 && guard.upper == 600);
        assert(guard.reserve(false, 100));
        // Reversing must not falsely cancel the uncertain forward displacement.
        assert(guard.lower == 400 && guard.upper == 600);
        assert(!guard.reserve(false, 401));
        assert(guard.available(false, 600) == 400);
        guard.invalidate();
        assert(!guard.available(true, 1) && !guard.available(false, 1));
    } else if (mode == 3) {
        assert(guard.configure(-1000, 1000) && guard.confirm(0));
        assert(guard.reserve(true, 100));
        assert(guard.configure(-1000, 1000) && guard.referenced);
        assert(guard.configure(-1000, 999) && !guard.referenced);
        assert(!guard.reserve(true, 1));
        assert(guard.confirm(0));
        guard = MotorTravelGuard{};
        assert(guard.configure(-1000, 1000) && !guard.referenced);
        assert(!guard.reserve(false, 1));
    } else if (mode == 4) {
        assert(guard.configure(INT_MIN, INT_MAX) && guard.confirm(INT_MAX - 2));
        assert(guard.available(true, 65535) == 2);
        assert(guard.reserve(true, 2) && guard.upper == INT_MAX);
        assert(!guard.reserve(true, 1));
        assert(guard.confirm(INT_MIN + 2));
        assert(guard.available(false, 65535) == 2);
        assert(guard.reserve(false, 2) && guard.lower == INT_MIN);
        assert(!guard.reserve(false, 1));
        assert(guard.confirm(0) && guard.available(true, 65535) == 65535);
        assert(!guard.reserve(true, 0));
    } else if (mode == 5) {
        uint32_t random = 1234567;
        auto next = [&]() { random = random * 1664525U + 1013904223U; return random; };
        for (int episode = 0; episode < 10000; ++episode) {
            guard = MotorTravelGuard{};
            assert(guard.configure(-10000, 10000));
            int64_t actual = static_cast<int>(next() % 18001) - 9000;
            assert(guard.confirm(static_cast<int32_t>(actual)));
            for (int move = 0; move < 100; ++move) {
                bool forward = (next() & 1U) != 0;
                const uint16_t requested = static_cast<uint16_t>(1 + next() % 3000);
                const uint16_t allowed = guard.available(forward, requested);
                if (!allowed) continue;
                assert(guard.reserve(forward, allowed));
                const int64_t moved = next() % (static_cast<uint32_t>(allowed) + 1U);
                actual += forward ? moved : -moved;
                assert(actual >= guard.minimum && actual <= guard.maximum);
                assert(actual >= guard.lower && actual <= guard.upper);
            }
        }
    } else { return 2; }
}
''', encoding='ascii')
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                 '-I' + str(ROOT / 'include'), str(source), '-o', str(cls.binary)],
                                capture_output=True, text=True, env=cls.env, timeout=60)
        if result.returncode:
            cls.temp.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_mode(self, mode):
        result = subprocess.run([str(self.binary), str(mode)], capture_output=True,
                                text=True, env=self.env, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unknown_and_invalid_reference_fail_closed(self):
        self.run_mode(0)

    def test_directional_bounds_and_exhausted_budget(self):
        self.run_mode(1)

    def test_early_stop_and_reverse_do_not_restore_uncertain_travel(self):
        self.run_mode(2)

    def test_config_change_and_restart_require_new_reference(self):
        self.run_mode(3)

    def test_signed_extremes_zero_and_u16_request(self):
        self.run_mode(4)

    def test_random_partial_motion_stays_inside_reserved_bounds(self):
        self.run_mode(5)


if __name__ == '__main__':
    unittest.main()
