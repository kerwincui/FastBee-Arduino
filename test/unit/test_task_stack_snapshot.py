"""Real task snapshot backend/routes with offline SDK, allocator and auth doubles."""
import hashlib
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class TaskStackSnapshotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required for task-stack behavior coverage')
        cls.inputs = [ROOT / name for name in (
            'include/systems/TaskStackSnapshot.h', 'src/systems/TaskStackSnapshot.cpp',
            'include/network/TaskStackSnapshotJson.h',
            'src/network/handlers/TaskStackSnapshotRoute.cpp',
            'src/network/handlers/SystemRouteHandler.cpp',
            'include/systems/HealthMonitor.h', 'src/systems/HealthMonitor.cpp',
            'include/systems/RestartDiagnostics.h', 'src/systems/RestartDiagnostics.cpp',
            'include/network/FixedJsonField.h', 'src/network/FixedJsonField.cpp',
            'test/unit/test_task_stack_snapshot.py', 'include/systems/TaskStackPermanentOwner.h',
            'test/unit/cpp/task_stack_snapshot_harness.cpp.in')]
        cls.extra_inputs = [ROOT / path for path in ('include/systems/TaskStackIpcOwners.h', 'src/systems/TaskStackIpcOwners.cpp')]
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs + cls.extra_inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-task-stacks-')
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
constexpr uint32_t MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2, MALLOC_CAP_DEFAULT=8;
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
size_t heap_caps_get_minimum_free_size(uint32_t);
void* heap_caps_calloc(size_t, size_t, uint32_t);
void heap_caps_free(void*);
''',
            'freertos/FreeRTOS.h': '''#pragma once
#ifndef configUSE_TRACE_FACILITY
#define configUSE_TRACE_FACILITY 1
#endif
#define INCLUDE_uxTaskGetStackHighWaterMark 1
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
unsigned uxTaskGetStackHighWaterMark(void*);
int xTaskGetSchedulerState();
'''}
        mocks['freertos/idf_additions.h'] = '#pragma once\n#include "task.h"\nTaskHandle_t xTaskGetIdleTaskHandleForCore(int);\n'
        mocks['freertos/timers.h'] = '#pragma once\n#include "task.h"\nTaskHandle_t xTimerGetTimerDaemonTaskHandle();\n'
        for name, contents in mocks.items():
            (folder / name).write_text(contents, encoding='utf-8')
        route = cls.inputs[3].read_text(encoding='utf-8')
        health_header = cls.inputs[5].read_text(encoding='utf-8')
        health = cls.inputs[6].read_text(encoding='utf-8')
        restart_header = cls.inputs[7].read_text(encoding='utf-8')
        restart = cls.inputs[8].read_text(encoding='utf-8')
        code = cls.inputs[-1].read_text(encoding='utf-8')
        code = code.replace('// @route@', definition(route, 'void setupTaskStackSnapshotRoute('))
        code = code.replace('// @stack_info@', definition(health_header, 'struct TaskStackInfo {') + ';')
        code = code.replace('// @health@', '\n'.join(definition(health, signature) for signature in (
            'void HealthMonitor::logTaskStackWatermarks()',
            'size_t HealthMonitor::getTaskStackInfo(')))
        code = code.replace('// @restart_type@', definition(restart_header, 'struct PreRestartSnapshot {') + ';')
        code = code.replace('// @restart@', definition(restart, 'void RestartDiagnostics::collectCurrentState('))
        cpp = folder / 'task_stacks.cpp'
        cpp.write_text(code, encoding='utf-8')
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        cls.binaries = {}
        for name, flags in (('sdk', ['-DESP_PLATFORM=1']),
                            ('no_trace', ['-DESP_PLATFORM=1', '-DconfigUSE_TRACE_FACILITY=0']),
                            ('host', []), ('single', ['-DESP_PLATFORM=1', '-DconfigNUMBER_OF_CORES=1']), ('smp', ['-DESP_PLATFORM=1', '-DCONFIG_FREERTOS_SMP=1'])):
            binary = folder / (name + '.exe')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                *flags, f'-I{folder}', f'-I{ROOT / "include"}', f'-I{json_header.parent}',
                str(cpp), str(cls.inputs[1]), str(ROOT / 'src/systems/TaskStackIpcOwners.cpp'), str(ROOT / 'src/network/FixedJsonField.cpp'),
                '-o', str(binary)], capture_output=True,
                text=True, encoding='utf-8', errors='replace', timeout=90)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[name] = binary

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs + cls.extra_inputs]:
            raise AssertionError('Task-stack inputs changed during tests')

    def check(self, mode, variant='sdk'):
        result = subprocess.run([str(self.binaries[variant]), mode], capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_permanent_owners_and_creation_numbers(self): self.check('owners')
    def test_single_core_never_queries_idle1(self): self.check('owners', 'single')
    def test_null_timer_remains_unknown(self): self.check('null_timer')
    def test_scheduler_not_started_never_calls_permanent_getters(self): self.check('not_started')
    def test_smp_idle_api_unavailable_is_honest(self): self.check('owners', 'smp')
    def test_dangling_dynamic_handle_is_not_dereferenced(self): self.check('dynamic')
    def test_actual_sdk_scalars_do_not_dereference_deleted_task_pointers(self): self.check('capture')
    def test_full_capacity_response_remains_bounded(self): self.check('capacity')
    def test_capacity_precheck_refuses_without_sdk_scan(self): self.check('too_many')
    def test_sdk_zero_and_task_growth_are_unknown_not_empty(self): self.check('sdk_zero')
    def test_sdk_over_capacity_does_not_publish_partial_rows(self): self.check('sdk_over')
    def test_changed_counts_preserve_capture_time_scalar_evidence(self): self.check('changed')
    def test_zero_wait_singleflight_and_release_after_reentry(self): self.check('busy')
    def test_first_capture_at_zero_and_rate_limit(self): self.check('cooldown')
    def test_cooldown_across_millis_wrap(self): self.check('wrap')
    def test_internal_free_budget_cannot_use_psram(self): self.check('low_free')
    def test_internal_largest_budget(self): self.check('small_block')
    def test_severe_rejects_even_with_plentiful_heap(self): self.check('severe')
    def test_sdk_scratch_oom_releases_other_allocation(self): self.check('scratch_oom')
    def test_scalar_copy_oom_frees_sdk_scratch(self): self.check('scalar_oom')
    def test_negative_clock_prevents_sdk_scan(self): self.check('clock_negative')
    def test_clock_reversal_does_not_publish_unmeasured_data(self): self.check('clock_reverse')
    def test_duration_is_measured_without_claiming_hard_timeout(self): self.check('duration')
    def test_route_auth_and_developer_denials_do_not_scan(self): self.check('auth')
    def test_route_missing_monitor_does_not_scan(self): self.check('no_monitor')
    def test_actual_route_posts_once_and_reports_cooldown(self): self.check('route')
    def test_response_json_oom_refuses_partial_success(self): self.check('json_oom')
    def test_legacy_health_reads_only_calling_task(self): self.check('legacy')
    def test_sdk_without_trace_is_unknown(self): self.check('unavailable', 'no_trace')
    def test_non_esp_backend_is_unknown(self): self.check('unavailable', 'host')
    def test_host_current_task_is_unknown_not_zero(self): self.check('legacy', 'host')

    def test_full_scan_callers_are_explicit_post_and_gated_health_sampler(self):
        source = (ROOT / 'src/network/handlers/SystemRouteHandler.cpp').read_text(encoding='utf-8')
        self.assertIn('FastBee::setupTaskStackSnapshotRoute(server, ctx);', source)
        self.assertNotIn('captureTaskStackSnapshot(', source)
        health = (ROOT / 'src/systems/HealthMonitor.cpp').read_text(encoding='utf-8')
        # D6 \u9636\u6bb5\u4e00\uff1aHealthMonitor \u5185\u552f\u4e00\u8c03\u7528\u70b9\u5c31\u662f\u95e8\u63a7\u91c7\u6837\u65b9\u6cd5\u672c\u8eab\u3002
        method = definition(health, 'void HealthMonitor::logFullTaskStackWatermarks()')
        self.assertEqual(method.count('captureTaskStackSnapshot('), 1)
        self.assertEqual(health.count('captureTaskStackSnapshot('), 1)
        self.assertIn('captureTaskStackSnapshot(isMemorySevere() || isMemoryCritical(), false)', method)
        self.assertIn('FULL_STACK_SAMPLE_INTERVAL_MS', method)
        header = (ROOT / 'include/systems/HealthMonitor.h').read_text(encoding='utf-8')
        match = re.search(r'FULL_STACK_SAMPLE_INTERVAL_MS\s*=\s*(\d+)UL', header)
        self.assertIsNotNone(match)
        # \u786c\u6027\u7ea6\u675f\uff1auxTaskGetSystemState \u4f1a\u6682\u505c\u6682\u505c\u6b62\u8c03\u5ea6\u5668\uff0c\u91c7\u6837\u95f4\u9694\u987b\u5728 5-10min \u7a97\u53e3\u5185\u3002
        self.assertGreaterEqual(int(match.group(1)), 300000)
        self.assertLessEqual(int(match.group(1)), 600000)
        for path in ('src/systems/HealthMonitor.cpp', 'src/systems/RestartDiagnostics.cpp'):
            text = (ROOT / path).read_text(encoding='utf-8')
            self.assertNotIn('xTaskGetHandle(', text)
        restart = (ROOT / 'src/systems/RestartDiagnostics.cpp').read_text(encoding='utf-8')
        self.assertNotIn('captureTaskStackSnapshot(', restart)
        for name in ('loopTaskWatermark', 'asyncTcpWatermark', 'mqttReconnWatermark'):
            self.assertIn(f'FastBee::setFixedJsonField(pre, "{name}", nullptr);', source)


if __name__ == '__main__':
    unittest.main()
