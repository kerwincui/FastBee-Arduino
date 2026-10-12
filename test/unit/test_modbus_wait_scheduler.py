"""Exercise the production RTU receive loop with tick-driven serial delivery."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ModbusWaitSchedulerTests(unittest.TestCase):
    def test_pending_control_preempts_only_background_retries(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        source = (ROOT / 'src/protocols/ModbusHandler.cpp').read_text(encoding='utf8')
        start = source.index('for (uint8_t attempt = 0; attempt <= maxRetries; attempt++) {',
                             source.index('OneShotError ModbusHandler::_executeModbusTransactionRaw'))
        # This fixture executes the retry scheduling prologue. The following
        # ticket admission/receipt code now has its own PM-to-RTU integration
        # suite; it is not part of the background retry scheduling slice.
        end = source.index('// Consume the ordinary permit', start)
        code = r'''
#include <cstdint>
#define LOG_INFOF(...) ((void)0)
static unsigned waits;
void delay(unsigned) { ++waits; }
void yield() {}
constexpr unsigned ONESHOT_CRC_ERROR = 2;
unsigned verify(bool pending, bool background) {
    bool _controlPending = false;
    const bool enforceSlaveBackoff = background;
    const uint8_t maxRetries = 2, expectedSlaveAddr = 1;
    unsigned error = 1, sent = 0;
    waits = 0;
    PRODUCTION_RETRY_START
        ++sent;
        // The first request times out while a control request is queued.
        _controlPending = pending;
    }
    return sent * 10 + waits;
}
int main() {
    if (verify(true, true) != 10) return 1;
    if (verify(false, true) != 32) return 2;
    if (verify(true, false) != 32) return 3;
}
'''.replace('PRODUCTION_RETRY_START', source[start:end])
        with tempfile.TemporaryDirectory(prefix='fastbee-modbus-retry-') as temp:
            temp = Path(temp); cpp = temp / 'retry.cpp'; cpp.write_text(code, encoding='utf8')
            executable = temp / ('retry.exe' if os.name == 'nt' else 'retry')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp),
                                    '-o', str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_no_response_and_valid_frames_release_cpu_each_wait_tick(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        source = (ROOT / 'src/protocols/ModbusHandler.cpp').read_text(encoding='utf8')
        start = source.index('uint8_t respBuf[Protocols::MODBUS_BUFFER_SIZE];',
                             source.index('OneShotError ModbusHandler::_executeModbusTransactionRaw'))
        end = source.index('// 超时且无数据', start)
        code = r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
namespace Protocols { constexpr size_t MODBUS_BUFFER_SIZE = 256; }
static unsigned long now, slept;
static unsigned long tickMs = 1;
unsigned long millis() { return now; }
void yield() { ++now; } // A runnable worker does not unblock idle.
void vTaskDelay(unsigned ticks) { now += ticks * tickMs; slept += ticks; }
struct Serial {
    bool responding; unsigned readCount = 0;
    int available() { return responding && now >= 3 && readCount < 6; }
    int read() { return readCount++ < 6 ? 0x31 : -1; }
};
int verify(bool response, unsigned tick) {
    now = slept = 0; tickMs = tick;
    Serial serial{response}; auto* modbusSerial = &serial;
    const unsigned long timeout = 20, charTimeout = 4;
    PRODUCTION_LOOP
    if (slept == 0) { std::puts("RTU wait never blocked; idle tasks cannot run"); return 1; }
    if (response) {
        if (timedOut || respIdx != 6 || responseOverflow || respBuf[0] != 0x31) return 2;
        if (now < 8 || now > 3 + charTimeout + 2 * tick) return 3;
    } else if (!timedOut || respIdx != 0 || now <= timeout || now > timeout + tick) return 4;
    return 0;
}
int main() {
    for (unsigned tick : {1U, 10U}) for (bool response : {false, true}) {
        int result = verify(response, tick); if (result) return result;
    }
}
'''.replace('PRODUCTION_LOOP', source[start:end])
        with tempfile.TemporaryDirectory(prefix='fastbee-modbus-wait-') as temp:
            temp = Path(temp); cpp = temp / 'wait.cpp'; cpp.write_text(code, encoding='utf8')
            executable = temp / ('wait.exe' if os.name == 'nt' else 'wait')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp),
                                    '-o', str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
