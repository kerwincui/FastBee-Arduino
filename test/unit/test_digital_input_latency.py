"""Exercise the actual DI sampler and cooldown policy with a simulated clock/GPIO.

No firmware build, device access, or reimplementation of the debounce algorithm.
The complete production methods are compiled against small hardware substitutes.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def block(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class DigitalInputLatencyTests(unittest.TestCase):
    def test_real_sampler_and_source_cooldown(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('A C++17 compiler is required')
        scheduler = (ROOT / 'src/core/PeriphExecScheduler.cpp').read_text(encoding='utf-8')
        manager = (ROOT / 'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/PeriphExecScheduler.h').read_text(encoding='utf-8')
        config = (ROOT / 'include/core/PeripheralConfig.h').read_text(encoding='utf-8')
        framework = (ROOT / 'src/core/FastBeeFramework.cpp').read_text(encoding='utf-8')
        task = framework[framework.index('addTask("button_event_check"'):]
        interval = int(re.search(r'\}, nullptr, (\d+), TaskPriority::PRIORITY_HIGH', task)[1])
        self.assertEqual(interval, 20)
        constants = '\n'.join(re.findall(
            r'constexpr unsigned long PERIPH_EXEC_(?:HEAVY_)?POLL_TRIGGER_MIN_INTERVAL_MS = \d+;', manager))
        code = (ROOT / 'test/unit/cpp/digital_input_latency_harness.cpp').read_text(encoding='utf-8')
        declarations = '\n'.join(block(header, 'struct ' + name) + ';' for name in
                                 ('ButtonEventConfig', 'ButtonRuntimeState'))
        declarations += '\n' + re.search(
            r'constexpr uint16_t DIGITAL_INPUT_DEFAULT_DEBOUNCE_MS = \d+;', config)[0]
        declarations += '\n' + block(config, 'inline uint16_t effectiveDigitalInputDebounceMs')
        code = code.replace('// PRODUCTION_DECLARATIONS', declarations + '\n' + constants)
        methods = block(scheduler, 'void PeriphExecScheduler::checkButtonEvents()')
        methods += '\n' + block(manager, 'unsigned long PeriphExecManager::getPollTriggerCooldownMs')
        code = code.replace('// PRODUCTION_METHODS', methods)
        code = code.replace('TASK_INTERVAL', str(interval))
        with tempfile.TemporaryDirectory(prefix='fastbee-di-latency-') as tmp:
            source = Path(tmp) / 'test.cpp'
            source.write_text(code, encoding='utf-8')
            executable = Path(tmp) / ('test.exe' if os.name == 'nt' else 'test')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     str(source), '-o', str(executable)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
