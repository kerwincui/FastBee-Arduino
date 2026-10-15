"""D6 phase-1 periodic full task-stack sampler: gate, wrap, skip and report.

Compiles the real TaskStackSnapshot backend plus the real
HealthMonitor::logFullTaskStackWatermarks body with an offline SDK/heap double.
No hardware access or long-duration stability claim is made here.
"""
import hashlib
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


def strip_cpp_line_comments(text):
    kept = []
    for line in text.splitlines():
        cut = line.find('//')
        kept.append(line if cut < 0 else line[:cut])
    return '\n'.join(kept)


class TaskStackPeriodicSamplingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required for periodic sampler coverage')
        cls.inputs = [ROOT / name for name in (
            'include/systems/HealthMonitor.h', 'src/systems/HealthMonitor.cpp',
            'include/systems/TaskStackSnapshot.h', 'src/systems/TaskStackSnapshot.cpp',
            'include/systems/TaskStackIpcOwners.h', 'include/systems/TaskStackPermanentOwner.h',
            'test/unit/cpp/task_stack_periodic_sampling_harness.cpp.in')]
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-periodic-stacks-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        (folder / 'freertos').mkdir()
        mocks = {
            'Arduino.h': '#pragma once\n#include <cstdint>\nunsigned long millis();\n',
            'esp_attr.h': '#pragma once\n#define DRAM_ATTR\n',
            'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
            'esp_heap_caps.h': '''#pragma once
#include <cstddef>
#include <cstdint>
constexpr uint32_t MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2;
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
void* heap_caps_calloc(size_t, size_t, uint32_t);
void heap_caps_free(void*);
''',
            'freertos/FreeRTOS.h': '''#pragma once
#ifndef configUSE_TRACE_FACILITY
#define configUSE_TRACE_FACILITY 1
#endif
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetIdleTaskHandle 1
#ifndef configNUMBER_OF_CORES
#define configNUMBER_OF_CORES 2
#endif
#ifndef CONFIG_FREERTOS_SMP
#define CONFIG_FREERTOS_SMP 0
#endif
#define configUSE_TIMERS 1
constexpr int taskSCHEDULER_NOT_STARTED=0;
using UBaseType_t=unsigned;
using TaskHandle_t=void*;
''',
            'freertos/task.h': '''#pragma once
#include "FreeRTOS.h"
#include <cstdint>
struct TaskStatus_t {
    void* xHandle; const char* pcTaskName; unsigned xTaskNumber;
    unsigned eCurrentState, uxCurrentPriority, uxBasePriority, ulRunTimeCounter;
    void* pxStackBase; uint32_t usStackHighWaterMark;
};
unsigned uxTaskGetNumberOfTasks();
unsigned uxTaskGetSystemState(TaskStatus_t*, unsigned, unsigned*);
int xTaskGetSchedulerState();
'''}
        mocks['freertos/idf_additions.h'] = '#pragma once\n#include "task.h"\nTaskHandle_t xTaskGetIdleTaskHandleForCore(int);\n'
        mocks['freertos/timers.h'] = '#pragma once\n#include "task.h"\nTaskHandle_t xTimerGetTimerDaemonTaskHandle();\n'
        for name, contents in mocks.items():
            (folder / name).write_text(contents, encoding='utf-8')

        header = cls.inputs[0].read_text(encoding='utf-8')
        health = cls.inputs[1].read_text(encoding='utf-8')
        interval = re.search(
            r'static constexpr unsigned long FULL_STACK_SAMPLE_INTERVAL_MS = \d+UL;', header)
        threshold = re.search(
            r'static constexpr unsigned long FULL_STACK_LOW_WATERMARK_BYTES = \d+UL;', header)
        if not interval or not threshold:
            raise AssertionError('HealthMonitor.h lost the D6 sampler constants')
        code = cls.inputs[-1].read_text(encoding='utf-8')
        code = code.replace('// @INTERVAL@', interval.group(0))
        code = code.replace('// @THRESHOLD@', threshold.group(0))
        code = code.replace('// @sampler@', definition(health, 'void HealthMonitor::logFullTaskStackWatermarks()'))
        cpp = folder / 'periodic_stacks.cpp'
        cpp.write_text(code, encoding='utf-8')
        cls.binaries = {}
        for name, flags in (('sdk', ['-DESP_PLATFORM=1']), ('host', [])):
            binary = folder / (name + '.exe')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                *flags, f'-I{folder}', f'-I{ROOT / "include"}',
                str(cpp), str(cls.inputs[3]), '-o', str(binary)],
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[name] = binary

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError('Periodic sampler inputs changed during tests')

    def check(self, mode, variant='sdk'):
        result = subprocess.run([str(self.binaries[variant]), mode], capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_interval_gate_fires_once_per_window(self):
        self.check('interval_gate')

    def test_millis_wraparound_keeps_interval_gate(self):
        self.check('wrap')

    def test_low_watermark_tasks_reported_with_severity(self):
        self.check('low_report')

    def test_threshold_boundary_is_strictly_below_1kb(self):
        self.check('boundary')

    def test_all_tasks_above_threshold_logs_debug_summary(self):
        self.check('ok_report')

    def test_memory_pressure_skips_scan_and_advances_gate(self):
        self.check('skip_severe')

    def test_internal_free_gate_skips_scan(self):
        self.check('skip_low_free')

    def test_internal_largest_block_gate_skips_scan(self):
        self.check('skip_small_block')

    def test_non_esp_backend_reports_unavailable_without_crash(self):
        self.check('unavailable', 'host')

    def test_sampler_source_is_observe_only_zero_alloc_and_gated(self):
        health = (ROOT / 'src/systems/HealthMonitor.cpp').read_text(encoding='utf-8')
        method = definition(health, 'void HealthMonitor::logFullTaskStackWatermarks()')
        code = strip_cpp_line_comments(method)
        self.assertEqual(code.count('captureTaskStackSnapshot('), 1)
        self.assertIn('captureTaskStackSnapshot(isMemorySevere() || isMemoryCritical(), false)', code)
        self.assertNotIn('String', code)
        self.assertNotIn(' new', code)
        self.assertNotIn('malloc', code)
        self.assertNotIn('heap_caps', code)
        self.assertIn('FULL_STACK_SAMPLE_INTERVAL_MS', code)
        header = (ROOT / 'include/systems/HealthMonitor.h').read_text(encoding='utf-8')
        match = re.search(r'FULL_STACK_SAMPLE_INTERVAL_MS = (\d+)UL', header)
        self.assertIsNotNone(match)
        # 计划 D6 硬性约束：uxTaskGetSystemState 采样间隔须在 5-10min 窗口内。
        self.assertGreaterEqual(int(match.group(1)), 300000)
        self.assertLessEqual(int(match.group(1)), 600000)


if __name__ == '__main__':
    unittest.main()
