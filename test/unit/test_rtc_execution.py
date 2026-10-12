"""Production typed RTC action, driver GPIO bitstream and admission contract."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class RtcExecutionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler: raise unittest.SkipTest('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-rtc-exec-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binaries = {}
        for compiled in (0,1):
            binary = Path(cls.directory.name)/('rtc%d.exe' % compiled)
            command = [compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                '-DFASTBEE_ENABLE_PERIPH_EXEC=1','-DFASTBEE_ENABLE_DS1302=%d' % compiled]
            command += ['-I'+str(ROOT/path) for path in (
                'test/unit/cpp/rtc_action_mocks','test/unit/cpp/rtc_datetime_mocks',
                'test/mocks','test/unit/cpp/mocks','include')]
            command += [str(ROOT/path) for path in ('test/unit/cpp/rtc_action_harness.cpp',
                'src/core/PeriphExecRtc.cpp','src/peripherals/drivers/DS1302Driver.cpp')]
            command += ['-o',str(binary)]
            result=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=90)
            if result.returncode: raise AssertionError(result.stdout+result.stderr)
            cls.binaries[compiled]=binary
        cls.codec=Path(cls.directory.name)/'rtc-codec.exe'
        json_root=os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            installed=list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not installed:raise AssertionError('Installed ArduinoJson headers required')
            json_root=str(installed[0].parent)
        command=[compiler,'-std=c++17','-Wall','-Wextra','-Werror',
            '-I'+str(ROOT/'test/mocks'),'-I'+json_root,
            '-I'+str(ROOT/'include'),str(ROOT/'test/unit/cpp/rtc_config_contract_harness.cpp'),'-o',str(cls.codec)]
        result=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=90)
        if result.returncode:raise AssertionError(result.stdout+result.stderr)

    def run_case(self,name,compiled=1):
        result=subprocess.run([str(self.binaries[compiled]),name],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_literal_command_only(self): self.run_case('validate')
    def test_disabled_compile_rejects_without_io(self): self.run_case('validate',0)
    def test_busy_stale_target_and_disabled_paths_have_no_io(self): self.run_case('guards')
    def test_start_is_serialized_and_read_back(self): self.run_case('start')
    def test_stop_is_serialized_and_read_back(self): self.run_case('stop')
    def test_repeated_state_does_not_rewrite_clock(self): self.run_case('idempotent')
    def test_invalid_clock_has_no_register_write(self): self.run_case('invalid-time')
    def test_failed_readback_is_not_success(self): self.run_case('verify-failed')
    def test_read_contract_has_no_implicit_write_or_received_value(self): self.run_case('read-validate')
    def test_compiled_out_read_never_reuses_a_value(self): self.run_case('read-validate',0)
    def test_read_guard_errors_are_visible_without_io(self): self.run_case('read-guards')
    def test_read_is_one_burst_without_register_write(self): self.run_case('read-valid')
    def test_read_supports_twelve_hour_calendar(self): self.run_case('read-12hour')
    def test_halted_clock_is_not_a_fresh_valid_sample(self): self.run_case('read-halted')
    def test_invalid_clock_clears_stale_result(self): self.run_case('read-invalid')
    def test_persistence_accepts_39_and_rejects_40(self):
        result=subprocess.run([str(self.codec)],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_read_has_distinct_report_and_boot_read_paths(self):
        source=(ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        run=source[source.index('std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions'):source.index('size_t PeriphExecExecutor::reportRuleCurrentState')]
        self.assertIn('ok = executeRtcReadAction(action, ar);',run)
        boot=source[source.index('size_t PeriphExecExecutor::reportRuleCurrentState'):source.index('bool PeriphExecExecutor::executePeripheralAction')]
        self.assertIn('if (executeRtcReadAction(action, ar)) reportableResults.push_back(ar);',boot)
        self.assertNotIn('executeRtcAction',boot)
        self.assertNotIn('executeRtcSetAction',boot)
    def test_set_requires_explicit_literal_source(self):self.run_case('set-validate')
    def test_set_is_rejected_when_compiled_out(self):self.run_case('set-validate',0)
    def test_set_checks_system_calendar_before_touching_gpio(self):self.run_case('set-invalid-source')
    def test_set_rejects_untracked_source_without_rtc_io(self):self.run_case('set-untracked-source')
    def test_set_rejects_valid_rtc_boot_calendar_without_rtc_io(self):self.run_case('set-rtc-source')
    def test_set_rejects_source_change_before_rtc_write(self):self.run_case('set-source-changed')
    def test_timezone_writer_blocks_copy_without_gpio(self):self.run_case('set-timezone-busy')
    def test_source_expiry_before_write_has_no_register_write(self):self.run_case('set-expired-before')
    def test_stalled_copy_before_write_has_no_register_write(self):self.run_case('set-delayed-before')
    def test_backward_clock_before_write_has_no_register_write(self):self.run_case('set-jump-before')
    def test_source_change_after_write_does_not_claim_rollback(self):self.run_case('set-source-after')
    def test_expiry_after_write_does_not_publish_stale_success(self):self.run_case('set-expired-after')
    def test_delay_after_write_does_not_publish_stale_success(self):self.run_case('set-delayed-after')
    def test_set_busy_has_no_hardware_side_effect(self):self.run_case('set-busy')
    def test_set_invalid_rtc_does_not_guess_old_halt_state(self):self.run_case('set-invalid-rtc')
    def test_set_writes_then_verifies_actual_calendar(self):self.run_case('set-valid')
    def test_set_keeps_paused_clock_paused(self):self.run_case('set-paused')
    def test_set_accepts_one_second_midnight_rollover(self):self.run_case('set-midnight')
    def test_set_maps_sunday_without_zero_weekday(self):self.run_case('set-sunday')
    def test_set_mismatched_readback_never_reports_success(self):self.run_case('set-mismatch')
    def test_set_mismatched_halt_state_never_reports_success(self):self.run_case('set-halt-mismatch')
    def test_set_mismatched_hour_mode_never_reports_success(self):self.run_case('set-mode-mismatch')

    def test_all_live_paths_use_shared_access(self):
        route='\n'.join((ROOT/path).read_text(encoding='utf-8') for path in (
            'src/network/handlers/PeripheralRouteHandler.cpp',
            'src/network/handlers/PeripheralReadRoutes.cpp'))
        self.assertEqual(route.count('FastBee::RtcAccess rtcAccess(id);'),3)
        self.assertNotIn('DS1302Driver::getInstance()',route)
        manager=(ROOT/'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        preflight=(ROOT/'src/network/handlers/PeriphExecRouteHandler.cpp').read_text(encoding='utf-8')
        for source in (manager,preflight):self.assertIn('FastBee::validateRtcAction(action)',source)
        executor=(ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        self.assertIn('return executeRtcAction(action);',executor)

if __name__=='__main__':unittest.main()
