"""Generated target matrix is actual C++ predicate evidence, not device support."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('action_catalog', ROOT / 'scripts/generate-action-contracts.py')
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)


class ActionContractCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (shutil.which('g++') or shutil.which('clang++')):
            raise unittest.SkipTest('Host C++17 compiler required')
        cls.report = catalog.generate()
        cls.text = catalog.render(cls.report)
        cls.actions = {item['id']: item for item in cls.report['actions']}
        cls.types = {item['name']: item['id'] for item in cls.report['types']}

    def test_generated_document_is_current(self):
        catalog.check_report(self.text, ROOT / catalog.OUTPUT)

    def test_all_enum_names_and_compiler_resolved_ids_are_present(self):
        names = catalog.enum_names((ROOT / 'include/core/PeripheralExecution.h').read_text(encoding='utf-8'), 'ExecActionType')
        self.assertEqual(set(names), {item['name'] for item in self.report['actions']})
        self.assertEqual(self.types['SDIO'], 37)
        self.assertEqual(self.types['SCREEN_DISPLAY'], 56)
        self.assertEqual(self.types['MODBUS_DEVICE'], 51)
        self.assertEqual(self.types['DEVICE_EVENT'], 60)

    def test_rtc_and_uart_are_exact_and_distinct(self):
        for action in (35, 36, 37):
            self.assertEqual(self.actions[action]['modes'], [[self.types['DS1302_RTC']]] * 4)
        self.assertEqual(self.actions[29]['modes'], [[self.types['UART']]] * 4)

    def test_passive_pwm_and_expander_exclusion_remain_distinct(self):
        pwm = self.actions[4]['modes']
        digital = self.types['GPIO_DIGITAL_OUTPUT']
        analog = self.types['GPIO_ANALOG_OUTPUT']
        self.assertNotIn(digital, pwm[0]); self.assertIn(digital, pwm[2])
        self.assertNotIn(digital, pwm[3]); self.assertNotIn(analog, pwm[1])
        self.assertIn(self.types['PWM_SERVO'], pwm[0])

    def test_scalar_waveform_catalog_agrees_with_pm_supported_modes(self):
        # These declared combinations also execute in the actual PM waveform
        # harness. Keep AO blink/passive breathe/DAC toggle unadvertised.
        pwm = self.types['GPIO_PWM_OUTPUT']; analog = self.types['GPIO_ANALOG_OUTPUT']
        digital = self.types['GPIO_DIGITAL_OUTPUT']; dac = self.types['DAC']
        self.assertIn(pwm, self.actions[2]['modes'][0])
        self.assertIn(digital, self.actions[2]['modes'][2])
        self.assertNotIn(analog, self.actions[2]['modes'][0])
        self.assertIn(pwm, self.actions[30]['modes'][0])
        self.assertIn(analog, self.actions[30]['modes'][0])
        self.assertIn(digital, self.actions[30]['modes'][2])
        self.assertNotIn(dac, self.actions[30]['modes'][0])
        self.assertNotIn(digital, self.actions[3]['modes'][2])

    def test_thermostat_is_present_with_explicit_control_output_conditions(self):
        thermostat = self.actions[39]
        self.assertEqual(thermostat['name'], 'ACTION_THERMOSTAT')
        self.assertTrue(thermostat['requiresPeripheral'])
        self.assertEqual(thermostat['modes'], [[self.types['GPIO_DIGITAL_OUTPUT']], [], [], []])
        row = next(row for row in self.text.splitlines() if '| ACTION_THERMOSTAT |' in row)
        for condition in ('单引脚', '非反相', '初始 HIGH', '来源与独占'):
            self.assertIn(condition, row)

    def test_nonperipheral_default_true_is_not_type_support(self):
        for action in (6, 7, 8, 9, 15, 18, 20, 21, 22, 23, 31, 32, 33, 34):
            self.assertFalse(self.actions[action]['requiresPeripheral'])
            self.assertEqual(self.actions[action]['modes'], [[], [], [], []])
        generic = next(row for row in self.text.splitlines() if '| ACTION_CALL_PERIPHERAL |' in row)
        self.assertIn('逐命令能力另验', generic)

    def test_enumeration_comments_and_implicit_values_do_not_invent_numbers(self):
        text = 'enum class Demo : uint8_t { A=3, /* missing slot */ B, C=(A + 7), };'
        self.assertEqual(catalog.enum_names(text, 'Demo'), ['A', 'B', 'C'])

    def test_unsupported_enum_syntax_and_duplicates_fail_closed(self):
        for text in ('enum class Demo { A, A };', 'enum class Demo { A, #if FLAG\n B, #endif };',
                     'enum class Demo {};', 'enum class Demo { A }; enum class Demo { B };'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                catalog.enum_names(text, 'Demo')

    def test_scope_and_source_fingerprints_do_not_claim_firmware(self):
        self.assertFalse(self.report['hardwareValidated']); self.assertFalse(self.report['firmwareValidated'])
        self.assertEqual(self.report['sourceHashes'], catalog.source_hashes(ROOT))
        for text in ('不访问总线/设备', '逐命令校验', '退役号 33', '非自动化恢复出厂', '不是完整工具链'):
            self.assertIn(text, self.text)
        self.assertNotIn(str(ROOT), self.text)

    def test_semantics_are_generated_from_production_and_keep_unknown_deadlines(self):
        contracts = {item['scenario']: item for item in self.report['contracts']}
        self.assertEqual(contracts['ADS1115_128SPS']['timeoutBudgetMs'], 66)
        self.assertEqual(contracts['ADS1115_SCAN4_8SPS']['timeoutBudgetMs'], 1200)
        self.assertEqual(contracts['ADS1115_SCAN4_8SPS']['fault'], 'invalidate_whole_scan_on_any_failure')
        self.assertEqual(contracts['ADS1115_128SPS']['timeoutScope'], 'conversion_budget_not_whole_action_deadline')
        self.assertEqual(contracts['DAC']['result'], 'sdk_ack_only')
        self.assertIsNone(contracts['DAC']['timeoutBudgetMs'])
        self.assertEqual(contracts['GPIO_TOGGLE']['idempotence'], 'non_idempotent')
        self.assertEqual(contracts['THERMOSTAT_STATUS']['result'], 'controller_status_only')
        self.assertEqual(contracts['PCA9685_STOP_PARSED']['replay'], 'release_cleanup_only_after_stop')
        self.assertFalse(contracts['PCA9685_UNCLASSIFIED']['covered'])
        self.assertEqual(contracts['PCA9685_UNCLASSIFIED']['schema'], 'unknown')
        self.assertIn('不是完整 JSON Schema', self.text)

    def test_determinism_and_line_endings(self):
        self.assertEqual(self.text, catalog.render(self.report))
        self.assertEqual(catalog.normalize('\ufeffx\r\ny\n'), 'x\ny\n')

    def test_missing_and_stale_check_never_writes(self):
        with tempfile.TemporaryDirectory(prefix='action-catalog-check-') as directory:
            output = Path(directory) / 'report.md'
            with self.assertRaises(RuntimeError): catalog.check_report(self.text, output)
            self.assertFalse(output.exists())
            output.write_text('user-owned content', encoding='utf-8')
            with self.assertRaises(RuntimeError): catalog.check_report(self.text, output)
            self.assertEqual(output.read_text(), 'user-owned content')

    def test_write_only_replaces_generator_owned_documents(self):
        with tempfile.TemporaryDirectory(prefix='action-catalog-write-') as directory:
            output = Path(directory) / 'report.md'
            output.write_text('user-owned content', encoding='utf-8')
            with self.assertRaises(RuntimeError): catalog.write_report(self.text, output)
            self.assertEqual(output.read_text(), 'user-owned content')
            output.write_text(catalog.MARKER + '\nstale\n', encoding='utf-8')
            catalog.write_report(self.text, output)
            catalog.check_report(self.text, output)

    def test_source_drift_rejects_before_rendering(self):
        result = SimpleNamespace(returncode=0, stdout='', stderr='')
        with patch.object(catalog, 'source_hashes', side_effect=[{'a': 'before'}, {'a': 'after'}]), patch.object(catalog.subprocess, 'run', return_value=result):
            with self.assertRaisesRegex(RuntimeError, 'Source changed'):
                catalog.generate()

    def test_compiler_failure_does_not_emit_partial_report(self):
        result = SimpleNamespace(returncode=1, stdout='', stderr='fixture compile error')
        with patch.object(catalog.subprocess, 'run', return_value=result):
            with self.assertRaisesRegex(RuntimeError, 'Contract compilation failed'):
                catalog.generate()


if __name__ == '__main__':
    unittest.main()
