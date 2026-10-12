"""Compile actual ordinary-output PM/checked timer and production dispatch branches.

SDK timer/GPIO calls are counted fault-injection seams. The production PM lease,
retirement, source checks, checked timer adapter and native write guard execute.
Unrelated expander/PWM branches and command parsers have independent harnesses.
No physical output or device access is performed.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'test/unit/cpp'


class OrdinaryControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required for actual ordinary-control PM regression')
        pm = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/PeripheralManager.h').read_text(encoding='utf-8')
        executor = (ROOT / 'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        script = (ROOT / 'src/core/ScriptEngine.cpp').read_text(encoding='utf-8')
        manager = (ROOT / 'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        write = cpp_function(pm, 'bool PeripheralManager::writeGpioStateLocked(')
        read = cpp_function(pm, 'GPIOState PeripheralManager::readGpioStateLocked(')
        read = read[:read.index('    uint8_t pin = config.getPrimaryPin();')] + """
    assert(ordinaryLockDepth); ++nativeReads;
    return !nativeReadOk ? GPIOState::STATE_UNDEFINED : nativeLevel ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
}
"""
        io_globals = pm[pm.index('using IoProviderMap ='):pm.index('bool isMcp23017(')]
        io_globals += cpp_function(pm, 'bool ioChannelPending(')
        io_globals += cpp_function(pm, 'struct IoBlink {') + ';\n'
        io_globals += cpp_function(pm, 'std::map<String, IoBlink>& ioBlinks()')
        io_methods = '\n'.join(cpp_function(pm, signature) for signature in (
            'bool PeripheralManager::ordinaryIoProducerPresentLocked(',
            'bool PeripheralManager::prepareOrdinaryIoProducerLocked(',
            'void PeripheralManager::discardOrdinaryIoReservationLocked(',
            'bool PeripheralManager::retireOrdinaryIoProducerLocked(',
            'bool PeripheralManager::startOrdinaryIoProducerLocked(',
            'bool PeripheralManager::retireOrdinaryIoGroupLocked(',
            'void PeripheralManager::serviceOrdinaryIoProducerLocked(',
        ))
        io_support = (CPP / 'ordinary_io_control_support.cpp.in').read_text(encoding='utf-8')
        io_support = io_support.replace('// @iox_globals@', io_globals).replace('// @iox_methods@', io_methods)
        typed = cpp_function(executor, 'bool PeriphExecExecutor::executePeripheralAction(')
        typed = typed[:typed.index('    if (digitalTargetFound && digitalTarget.isExpanderChannel()) {')] + '    return false;\n}'
        call = cpp_function(executor, 'if (FastBee::isOrdinaryDigitalTarget(*targetCfg)) {')
        call += '\n' + cpp_function(executor, 'if (FastBee::isOrdinaryScalarTarget(*targetCfg)) {')
        named = cpp_function(script, 'if (pm.getPeripheralSnapshot(cmd.strParam, nativeTarget) &&')
        named = 'PeripheralConfig nativeTarget;\n' + named
        named += '\n' + cpp_function(script,
            'if (pm.getPeripheralSnapshot(cmd.strParam, nativeTarget) &&\n'
            '                    FastBee::isOrdinaryScalarTarget(nativeTarget)) {')
        mqtt = cpp_function(manager, 'static bool executeDirectOutputCommand(')
        template = (CPP / 'ordinary_control_harness.cpp.in').read_text(encoding='utf-8')
        source = template.replace('// @write_gpio@', write).replace('// @typed_action@', typed)
        source = source.replace('// @iox_support@', io_support).replace('// @read_gpio@', read)
        source = source.replace('// @call_native@', call).replace('// @script_native@', named)
        source = source.replace('// @mqtt_direct@', mqtt)
        mapping = cpp_function((ROOT / 'include/core/PeripheralConfig.h').read_text(encoding='utf-8'),
                               'inline uint32_t mapAnalogOutputPercent(').removeprefix('inline ')
        source = source.replace('// @scalar_mapping@', mapping)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-ordinary-control-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        (directory / 'core').mkdir()
        mock = (CPP / 'ordinary_control_mocks/core/PeripheralManager.h').read_text(encoding='utf-8')
        context = cpp_function(header, 'struct ActionTickerData {') + ';'
        (directory / 'core/PeripheralManager.h').write_text(
            mock.replace('// @ordinary_context@', context), encoding='utf-8')
        harness = directory / 'ordinary.cpp'
        harness.write_text(source, encoding='utf-8')
        cls.binary = directory / 'ordinary.exe'
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DCONFIG_IDF_TARGET_ESP32=1',
            '-Wno-unused-parameter', '-I' + str(directory), '-I' + str(CPP / 'ordinary_control_mocks'),
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            str(harness), str(ROOT / 'src/core/PeripheralControlArbitration.cpp'),
            str(ROOT / 'src/peripherals/IoExpander.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reload_and_transaction_do_not_resurrect_retired_grants(self):
        pm = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        persistence = (ROOT / 'src/core/PeripheralManagerPersistence.cpp').read_text(encoding='utf-8')
        load = cpp_function(persistence, 'bool PeripheralManager::loadConfiguration(')
        self.assertLess(load.index('!actionTickers.empty()'), load.index('runtimeStates.clear()'))
        self.assertLess(load.index('ordinaryDigitalClaimedLocked(runtime.first)'), load.index('runtimeStates.clear()'))
        transaction = cpp_function(pm, 'bool PeripheralManager::applyMutationTransactional(')
        self.assertLess(transaction.index('ordinaryDigitalClaimedLocked(snapshotId)'), transaction.index('MutationSnapshot snapshot'))
        update = cpp_function(pm, 'bool PeripheralManager::updatePeripheral(const String& id, const PeripheralConfig& config, String& errorMsg)')
        self.assertLess(update.index('ordinaryDigitalClaimedLocked(id)'), update.index('const PeripheralConfig oldConfig'))

    def test_rule_and_worker_keep_trusted_context_to_terminal(self):
        manager = (ROOT / 'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        executor = (ROOT / 'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        queued = cpp_function(manager, 'bool PeriphExecManager::dispatchAsync(')
        worker = cpp_function(manager, 'void PeriphExecManager::executeWorkerJob(')
        run = cpp_function(executor, 'std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(')
        self.assertIn('ctx->ordinaryControl = operation.ordinaryControl', queued)
        self.assertIn('result.ordinaryControl = ctx->ordinaryControl', worker)
        self.assertIn('finishOrdinaryControl(context, !completed || failed)', run)
        self.assertIn('ordinaryControl, &outputResult)', run)
        self.assertIn('rememberOrdinaryControlResult(outputResult, &traceOut->ordinaryControlResult)', run)


for _case in ('iox_high', 'iox_blink', 'iox_interval', 'iox_busy', 'iox_write_failure',
              'wave_passive_high', 'wave_passive_default_ledger', 'wave_passive_pwm',
              'wave_passive_toggle', 'wave_passive_blink', 'wave_passive_stop_failed',
              'wave_passive_start_failed', 'wave_passive_legacy', 'wave_passive_mqtt',
              'wave_passive_script', 'wave_passive_breathe_denied', 'wave_passive_inverted',
              'wave_pwm_toggle', 'wave_pwm_blink', 'wave_ao_toggle', 'wave_ao_blink_denied',
              'wave_unknown_toggle', 'wave_blink_expired', 'wave_blink_stale_uart',
              'wave_blink_write_failed', 'wave_blink_replace', 'wave_blink_no_context',
              'iox_stale_sample', 'iox_stop', 'iox_manual_expiry', 'iox_conflict',
              'iox_source_stale', 'iox_uart_stale', 'iox_pending', 'iox_channel_drift',
              'iox_provider_pointer', 'iox_legacy', 'iox_group_retirement', 'iox_negative_clock',
              'iox_legacy_write', 'iox_toggle', 'iox_dispatch', 'iox_generation_is_not_session',
              'invalid_finish_preserves_queue', 'negative_clock_callback', 'negative_clock_maintenance',
              'scalar_pwm_sync', 'scalar_pwm_ao', 'scalar_dac_sync', 'scalar_dac_failed',
              'scalar_parameters', 'scalar_missing_context', 'scalar_legacy_write', 'scalar_legacy_producer',
              'scalar_pwm_failed', 'scalar_ledger_failed', 'scalar_typed_pwm', 'scalar_typed_breathe',
              'scalar_typed_dac', 'scalar_typed_invalid', 'scalar_call', 'scalar_script', 'scalar_mqtt',
              'scalar_breathe', 'scalar_breathe_replace', 'scalar_breathe_stop_failed',
              'scalar_breathe_16bit', 'scalar_breathe_ao_cycle',
              'scalar_breathe_start_failed', 'scalar_breathe_expired', 'scalar_breathe_stale_rule',
              'scalar_breathe_stale_uart', 'scalar_breathe_stale_screen',
              'negative_clock_finish', 'negative_clock_submit', 'legacy_write_denied', 'begin_limits', 'typed_high', 'typed_blink',
              'typed_interval_rejected', 'typed_missing_context', 'call_blink_stop',
              'call_missing_context', 'call_numeric', 'script_blink_stop', 'script_missing_context',
              'mqtt_direct', 'first_failure_preserved', 'screen_stale', 'rule_stale',
              'config_stale', 'uart_stale', 'dedicated_owner', 'busy', 'disabled', 'suspended',
              'cleanup_pending', 'read_failed', 'write_failed', 'generation_exhausted',
              'low_memory', 'token_exhausted', 'create_failed', 'partial_create_failed',
              'start_failed', 'high_low_same_operation', 'conflict', 'executing_expiry',
              'same_operation_stop', 'replace_blink', 'manual_expiry', 'clock_backwards',
              'stop_failed', 'delete_failed', 'override', 'stale_review', 'target_review',
              'stale_callback', 'blink'):
    setattr(OrdinaryControlTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
