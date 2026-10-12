"""Actual IPC owner state/backend and scalar snapshot under delayed SDK callbacks."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class IpcTaskOwnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-ipc-owner-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        mocks = {
            'Arduino.h': '#pragma once\nunsigned long millis();\n',
            'esp_attr.h': '#pragma once\n#define DRAM_ATTR\n',
            'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
            'esp_heap_caps.h': '#pragma once\n#include <cstddef>\n#include <cstdint>\nconstexpr uint32_t MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2;\nsize_t heap_caps_get_free_size(uint32_t);\nsize_t heap_caps_get_largest_free_block(uint32_t);\nvoid* heap_caps_calloc(size_t,size_t,uint32_t);\nvoid heap_caps_free(void*);\n',
            'freertos/FreeRTOS.h': '#pragma once\n#ifndef configNUMBER_OF_CORES\n#define configNUMBER_OF_CORES 2\n#endif\n#ifndef configUSE_TRACE_FACILITY\n#define configUSE_TRACE_FACILITY 1\n#endif\n#define INCLUDE_uxTaskGetStackHighWaterMark 1\n#define INCLUDE_xTaskGetSchedulerState 1\n#define INCLUDE_xTaskGetIdleTaskHandle 1\n#define CONFIG_FREERTOS_SMP 0\n#define configUSE_TIMERS 1\n#define CONFIG_ESP_IPC_ENABLE 1\n#define CONFIG_ESP_IPC_USES_CALLERS_PRIORITY 1\nconstexpr int taskSCHEDULER_NOT_STARTED=0,pdFALSE=0;\nusing UBaseType_t=unsigned;\nusing TaskHandle_t=void*;\n',
            'freertos/task.h': '#pragma once\n#include "FreeRTOS.h"\n#include <cstdint>\nconstexpr unsigned eRunning=0;\nstruct TaskStatus_t{void*xHandle;const char*pcTaskName;unsigned xTaskNumber,eCurrentState,uxCurrentPriority,uxBasePriority,ulRunTimeCounter;void*pxStackBase;uint32_t usStackHighWaterMark;};\nvoid vTaskGetInfo(void*,TaskStatus_t*,int,unsigned);\nint xPortGetCoreID();\nunsigned uxTaskGetNumberOfTasks();\nunsigned uxTaskGetSystemState(TaskStatus_t*,unsigned,unsigned*);\nunsigned uxTaskGetStackHighWaterMark(void*);\nint xTaskGetSchedulerState();\n',
            'freertos/idf_additions.h': '#pragma once\n#include "task.h"\nvoid* xTaskGetIdleTaskHandleForCore(int);\n',
            'freertos/timers.h': '#pragma once\n#include "task.h"\nvoid* xTimerGetTimerDaemonTaskHandle();\n',
            'esp_private/esp_ipc.h': '#pragma once\n#include <cstdint>\nconstexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=0x103;\nint esp_ipc_call_nonblocking(uint32_t,void(*)(void*),void*);\n',
        }
        for name, contents in mocks.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding='utf8')
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        cls.sources = [ROOT / path for path in ('include/systems/TaskStackIpcOwners.h',
            'src/systems/TaskStackIpcOwners.cpp', 'src/systems/TaskStackSnapshot.cpp',
            'include/systems/TaskStackSnapshot.h', 'include/network/TaskStackSnapshotJson.h',
            'include/systems/TaskStackPermanentOwner.h', 'test/unit/cpp/ipc_task_owner_harness.cpp',
            'src/network/handlers/TaskStackSnapshotRoute.cpp')]
        cls.pins = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.sources]
        cls.binaries = {}
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        driver = directory / 'ipc_task_owner.cpp'
        code = cls.sources[-2].read_text(encoding='utf8')
        code = code.replace('// @route@', definition(cls.sources[-1].read_text(encoding='utf8'),
                            'void setupTaskStackSnapshotRoute('))
        driver.write_text(code, encoding='utf8')
        for variant, flags in (('s3', ['-DESP_PLATFORM=1', '-DCONFIG_IDF_TARGET_ESP32S3=1']),
                ('single', ['-DESP_PLATFORM=1', '-DCONFIG_IDF_TARGET_ESP32S3=1', '-DconfigNUMBER_OF_CORES=1']),
                ('no_trace', ['-DESP_PLATFORM=1', '-DCONFIG_IDF_TARGET_ESP32S3=1', '-DconfigUSE_TRACE_FACILITY=0']),
                ('c3', ['-DESP_PLATFORM=1', '-DCONFIG_IDF_TARGET_ESP32C3=1']), ('host', [])):
            binary = directory / (variant + '.exe')
            result = subprocess.run([compiler, '-std=c++17', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1', *flags,
                '-I'+str(directory), '-I'+str(ROOT/'include'), '-I'+str(json_header.parent),
                str(driver), str(cls.sources[1]), str(cls.sources[2]),
                str(ROOT/'src/network/FixedJsonField.cpp'), '-o', str(binary)], capture_output=True,
                text=True, encoding='utf8', errors='replace', timeout=90)
            if result.returncode:
                raise AssertionError(f'{variant}: compiler exit {result.returncode}\n' + result.stdout + result.stderr)
            cls.binaries[variant] = binary

    @classmethod
    def tearDownClass(cls):
        if cls.pins != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.sources]:
            raise AssertionError('IPC production input drift')

    def check(self, case, variant='s3'):
        result = subprocess.run([str(self.binaries[variant]), case], capture_output=True,
                                text=True, encoding='utf8', errors='replace', timeout=45)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_callback_can_preempt_submitter(self): self.check('immediate')
    def test_only_later_explicit_post_retries_nonqueued_busy_slot(self): self.check('busy')
    def test_sdk_rejected_context_is_unknown(self): self.check('sdk_reject')
    def test_pending_not_reused_and_late_mapping_not_backfilled(self): self.check('pending')
    def test_wrong_cpu_identity_stays_unknown(self): self.check('wrong_core')
    def test_duplicate_creation_numbers_stay_unknown(self): self.check('duplicate')
    def test_real_backend_snapshot_and_json_keep_original_watermarks(self): self.check('capture')
    def test_memory_denial_submits_no_ipc_or_scan(self): self.check('memory')
    def test_severe_memory_denial_submits_no_ipc_or_scan(self): self.check('severe')
    def test_negative_capture_clock_submits_no_ipc_or_scan(self): self.check('capture_clock')
    def test_clock_reversal_remains_unknown(self): self.check('clock')
    def test_pending_deadline_handles_millisecond_counter_wrap(self): self.check('wrap')
    def test_competing_submitters_and_readers_never_publish_torn_identity(self): self.check('stress')
    def test_single_core_does_not_submit_or_allocate_ipc_storage(self): self.check('unsupported', 'single')
    def test_c3_does_not_submit_or_allocate_ipc_storage(self): self.check('unsupported', 'c3')
    def test_no_trace_does_not_submit_or_allocate_ipc_storage(self): self.check('unsupported', 'no_trace')
    def test_host_does_not_submit_or_allocate_ipc_storage(self): self.check('unsupported', 'host')
    def test_real_route_observe_only_after_boot_submits_no_ipc(self): self.check('route_observe_cold')
    def test_real_route_observe_only_reuses_ready_identity(self): self.check('route_observe_ready')
    def test_real_route_observe_only_never_retries_rejected_context(self): self.check('route_observe_busy')
    def test_real_route_default_preserves_explicit_diagnostic(self): self.check('route_default')
    def test_real_route_rejects_unknown_empty_duplicate_and_file_parameters(self): self.check('route_invalid')
    def test_real_route_observe_only_preserves_auth_and_developer_checks(self): self.check('route_auth')


if __name__ == '__main__':
    unittest.main()
