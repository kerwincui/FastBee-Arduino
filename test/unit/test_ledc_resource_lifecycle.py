"""Actual PM LEDC ledger and native output setup/cleanup; SDK is substituted."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class LedcResourceLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        ledger = (ROOT / 'src/core/PeripheralManagerLedc.cpp').read_text(encoding='utf-8')
        manager = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(definition(ledger, signature) for signature in (
            'bool PeripheralManager::attachManagedLedc(', 'bool PeripheralManager::detachManagedLedc(',
            'bool PeripheralManager::writeScriptPwm(', 'bool PeripheralManager::releaseScriptPwm(',
            'void PeripheralManager::snapshotLedcResources('))
        methods += '\n' + '\n'.join(definition(manager, signature) for signature in (
            'bool PeripheralManager::setupGPIOPin(', 'bool PeripheralManager::setupPWMPin(',
            'bool PeripheralManager::attachServoSignal(', 'bool PeripheralManager::setupServoPin('))
        servo = (ROOT / 'src/core/PeripheralManagerServoControl.cpp').read_text(encoding='utf-8')
        methods += '\n' + definition(servo, 'bool PeripheralManager::releaseServoSignal(')
        teardown = definition(manager, 'bool PeripheralManager::teardownHardware(')
        start = teardown.index('// GPIO 输出清理')
        end = teardown.index('// DAC 清理', start)
        methods += '\nbool PeripheralManager::teardownNativeOutput(const PeripheralConfig& config) {\n' + teardown[start:end] + 'return true;\n}'
        harness = (ROOT / 'test/unit/cpp/ledc_resource_harness.cpp.in').read_text(encoding='utf-8')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-ledc-resource-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder / 'ledc.cpp'
        cpp.write_text(harness.replace('// @production@', methods), encoding='utf-8')
        cls.binary = folder / ('ledc.exe' if os.name == 'nt' else 'ledc')
        built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'include'), str(cpp), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_success_identity_capacity_and_owner_reuse(self): self.run_case('ledger')
    def test_rejected_attach_does_not_publish_or_mutate(self): self.run_case('admission')
    def test_failed_detach_retains_target_and_blocks_reuse(self): self.run_case('retained')
    def test_snapshot_is_passive_and_real_busy_lock_is_unknown(self): self.run_case('passive')
    def test_pwm_write_failure_and_teardown_keep_failed_cleanup(self): self.run_case('pwm')
    def test_buzzer_write_failure_and_teardown_keep_failed_cleanup(self): self.run_case('buzzer')
    def test_servo_write_and_release_keep_failed_cleanup(self): self.run_case('servo')
    def test_script_actual_attach_write_failures_and_gpio_switch(self): self.run_case('script')
    def test_external_detach_is_not_hidden_by_automatic_reattach(self): self.run_case('lost')


if __name__ == '__main__':
    unittest.main()
