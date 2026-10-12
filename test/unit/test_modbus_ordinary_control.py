"""Actual PM reservation -> handler -> raw RTU, with counted UART/RTOS seams.

No mirrored admission algorithm: compile the complete production backend and
handler adapter, actual raw transaction, CRC/echo wrapper and context issuer.
Fault hooks run while acquiring the bus or at the transport boundary.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]

class ModbusOrdinaryControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        header = (ROOT/'include/protocols/ModbusHandler.h').read_text(encoding='utf-8')
        types = '\n'.join(cpp_function(header, marker)+';' for marker in
            ('enum ModbusException', 'enum OneShotError', 'struct ModbusDelayReceipt', 'struct OneShotResult', 'struct CoilDelayTask', 'struct OrdinaryCoilDelayTask'))
        handler = (ROOT/'src/protocols/ModbusHandler.cpp').read_text(encoding='utf-8')
        actual = '\n'.join(cpp_function(handler, marker) for marker in (
            'static unsigned long modbusRtuFrameGapMs(', 'uint16_t ModbusHandler::calculateCRC(',
            'bool ModbusHandler::validateFrame(', 'OneShotResult ModbusHandler::_executeModbusTransaction(',
            'OneShotError ModbusHandler::_executeModbusTransactionRaw('))
        adapter = (ROOT/'src/protocols/ModbusOrdinaryControl.cpp').read_text(encoding='utf-8')
        adapter = adapter[adapter.index('namespace {'):adapter.rindex('#endif')]
        backend = (ROOT/'src/core/PeripheralControlModbus.cpp').read_text(encoding='utf-8')
        backend = backend[backend.index('#if FASTBEE_ENABLE_MODBUS'):]
        common = (ROOT/'src/core/PeripheralControlArbitration.cpp').read_text(encoding='utf-8')
        issuer = cpp_function(common, 'bool ordinaryActorCurrent(') + '\n' + cpp_function(common,
            'bool PeripheralManager::beginOrdinaryControl(')
        uart = cpp_function((ROOT/'src/core/PeripheralManagerUART.cpp').read_text(encoding='utf-8'),
            'bool PeripheralManager::isUartEventOriginCurrent(')
        template = (ROOT/'test/unit/cpp/modbus_ordinary_harness.cpp').read_text(encoding='utf-8')
        source = template.replace('// @types@', types).replace('// @transport@', actual)
        source = source.replace('// @backend@', backend).replace('// @adapter@', adapter)
        source = source.replace('// @issuer@', issuer+'\n'+uart)
        delay = ''
        for name in ('ModbusDelay.cpp', 'ModbusOrdinaryDelay.cpp'):
            body = (ROOT/'src/protocols'/name).read_text(encoding='utf-8')
            delay += body[body.index('namespace {'):body.rindex('#endif')]+'\n'
        source = source.replace('// @delay@', delay)
        guard = cpp_function((ROOT/'include/core/AsyncExecTypes.h').read_text(encoding='utf-8'), 'class RecursiveMutexGuard')+';'
        source = source.replace('// @recursive_guard@', guard)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-modbus-ordinary-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder/'ordinary.cpp'; cpp.write_text(source, encoding='utf-8')
        cls.binary = folder/'ordinary.exe'
        build = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), '-I'+str(json_header.parent), str(cpp), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if build.returncode:
            raise AssertionError(build.stdout+build.stderr)

    def check(self, scenario):
        run = subprocess.run([str(self.binary), scenario], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)

    def test_actual_boundary_wiring(self):
        common = (ROOT/'src/core/PeripheralControlArbitration.cpp').read_text(encoding='utf-8')
        self.assertIn('finishOrdinaryModbusLocked(context, cancelled)', cpp_function(common,
            'void PeripheralManager::finishOrdinaryControl('))
        self.assertIn('serviceOrdinaryModbusLocked()', cpp_function(common,
            'void PeripheralManager::serviceOrdinaryControlExpiry('))
        executor = (ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        action = cpp_function(executor, 'bool PeriphExecExecutor::executeModbusPollAction(')
        self.assertIn('ctrlArr && mode == FastBee::ModbusActionMode::ExecuteRule', action)
        self.assertIn('submitOrdinaryModbusRelay(', action)
        route = cpp_function((ROOT/'src/network/handlers/ModbusRouteHandler.cpp').read_text(encoding='utf-8'),
            'void ModbusRouteHandler::handleModbusCoilControl(')
        self.assertIn('beginHttpModbusRelayOutput', route)
        self.assertNotIn('writeCoilOnce', route)
        self.assertNotIn('writeRegisterOnce', route)
        self.assertNotIn('readRegistersOnce', route)  # Toggle belongs inside the same bus lock.

for _case in ('delay_software', 'delay_software_http', 'delay_register', 'delay_receipt_mismatch', 'delay_context_binding', 'delay_config_retire', 'delay_handler_retire', 'delay_rule_retire', 'delay_uart_retire', 'delay_deadline_retire', 'delay_newer_write', 'delay_cancel', 'delay_timeout', 'delay_status', 'delay_bus_busy', 'delay_cleanup_pending', 'delay_hardware', 'delay_hardware_timeout', 'delay_hardware_unattempted', 'delay_hardware_source_retire', 'delay_hardware_handler_replace', 'delay_hardware_maintenance', 'delay_hardware_other_slave', 'delay_hardware_clock_unknown', 'delay_hardware_window_expiry', 'delay_hardware_nc', 'delay_hardware_fc16', 'delay_hardware_window_mismatch', 'delay_hardware_overflow', 'delay_hardware_admission_failure'):
    setattr(ModbusOrdinaryControlTests, 'test_'+_case,
            lambda self, scenario=_case: self.check(scenario))

for _case in ('frame_fc05', 'frame_fc06', 'frame_fc15', 'frame_fc16', 'frame_fc16_max', 'frame_pwm', 'frame_pid', 'frame_unmapped', 'frame_ambiguous', 'frame_bad_bytecount', 'frame_range_overflow', 'frame_unused_coil_bits', 'frame_bad_raw_capacity', 'frame_vendor_toggle', 'frame_vendor_delay255', 'frame_vendor_mixed', 'frame_vendor_unknown', 'frame_maintenance_cannot_write_ordinary', 'frame_maintenance_ordinary_http_zero', 'frame_maintenance_terminal', 'frame_maintenance', 'frame_maintenance_baud', 'frame_maintenance_auth', 'frame_maintenance_ordinary_busy', 'frame_maintenance_worker_busy', 'frame_ordinary_maintenance_busy', 'frame_toggle', 'frame_toggle_retired', 'frame_toggle_timeout', 'frame_raw_exception', 'frame_wrong_echo', 'frame_short_write', 'frame_source_wait', 'frame_handler_wait', 'frame_mutated_frame', 'frame_deadline_wait', 'frame_prefix_failure', 'frame_snapshot_pwm', 'frame_worker_multi_frame'):
    setattr(ModbusOrdinaryControlTests, 'test_'+_case,
            lambda self, scenario=_case: self.check(scenario))

for _case in ('lazy_empty', 'lazy_stable_capacity', 'lazy_oom_relay', 'lazy_oom_frame', 'lazy_oom_worker', 'lazy_oom_stop',
              'success', 'register', 'same_slave_alias', 'different_slave', 'source_conflict',
              'same_actor_replace', 'review_mismatch', 'expiry_during_bus_wait', 'config_during_bus_wait',
              'rule_during_bus_wait', 'screen_during_bus_wait', 'uart_during_bus_wait',
              'handler_generation', 'handler_instance', 'endpoint_mismatch', 'wrong_function',
              'cancel_during_bus_wait', 'cancel_during_response', 'terminal_during_response',
              'timeout', 'crc', 'exception', 'short_write', 'short_echo', 'wrong_echo', 'wrong_fc',
              'travel_rejected', 'busy', 'prefix_then_failure', 'failure_retained', 'no_context',
              'toggle', 'toggle_read_failed', 'toggle_retired_after_read', 'ambiguous_endpoint',
              'gate_once', 'gate_revoked', 'sequence_exhausted', 'retired_replay', 'clock_reverse',
              'allocation_before_write', 'allocation_after_write', 'mixed_slave_mapping', 'terminal_lock_unavailable',
              'worker_lock_transient', 'worker_lock_current', 'worker_lock_sustained', 'worker_lock_deadline', 'worker_lock_wrap', 'worker_lock_cancel', 'worker_lock_stop',
              'worker_cancel_queued',
              'worker_caller_terminal',
              'worker_timeout_running',
              'worker_source_between_writes',
              'worker_screen_replaced',
              'worker_handler_replaced',
              'worker_late_terminal',
              'stop_after_queued',
              'stop_during_response',
              'stop_parameter_denied',
              'stop_same_invocation',
              'motor_fc16',
              'motor_fc16_short_echo',
              'motor_fc16_wrong_echo', 'deadline_during_bus_wait', 'deadline_wrap_expired', 'deadline_wrap_current'):
    setattr(ModbusOrdinaryControlTests, 'test_'+_case,
            lambda self, scenario=_case: self.check(scenario))

for _case in ('mixed_nonoverlap', 'mixed_source_conflict', 'spaces_distinct',
              'motor_shared_command', 'motor_disjoint', 'relay_wait_alias', 'motor_wait_alias',
              'frame_wait_alias', 'disabled_alias', 'other_slave_alias', 'unrelated_invalid_profile',
              'value_alias_high', 'value_alias_low', 'pwm_resolution_alias', 'motor_fc_alias',
              'invalid_motor_alias', 'pid_pv_alias', 'pid_internal_pv', 'relay_internal_batch',
              'relay_internal_timer', 'timer_other_alias', 'batch_toggle_alias',
              'vendor_relay_alias', 'same_type_relay', 'same_type_motor', 'fc16_suffix_alias'):
    setattr(ModbusOrdinaryControlTests, 'test_mapping_'+_case,
            lambda self, scenario='mapping_'+_case: self.check(scenario))

if __name__ == '__main__':
    unittest.main()
