"""Compile actual owner observers/getters with controlled lifetime and RTOS state.

No device or shared build access. Business transitions are controlled under the
owner lock; the production read paths and first-start synchronization run intact.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]
OWNERS = {
    'peripherals/LCDManager.cpp': ('LCDManager', 'getInstance'),
    'peripherals/drivers/LCD1602Driver.cpp': ('LCD1602Driver', 'getInstance'),
    'peripherals/drivers/IRRemoteDriver.cpp': ('IRRemoteDriver', 'getInstance'),
    'peripherals/drivers/RFLearnDriver.cpp': ('RFLearnDriver', 'getInstance'),
    'core/RuleScriptManager.cpp': ('RuleScriptManager', 'getInstance'),
}


class FeatureOwnerObservationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('A C++17 compiler is required')
        cls.inputs = [ROOT / 'src' / file for file in OWNERS]
        cls.inputs += [ROOT / file for file in ('src/screen/ScreenRuntime.cpp',
            'include/core/FeatureInstanceRegistry.h', 'include/core/RuleScriptLifetime.h', 'include/core/AsyncExecTypes.h',
            'test/unit/cpp/feature_owner_observation_harness.cpp',
            'test/unit/test_feature_owner_observation.py')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-feature-owners-')
        cls.addClassCleanup(cls.temp.cleanup)
        temporary = Path(cls.temp.name)
        snippets = []
        for file, (owner, getter) in OWNERS.items():
            source = (ROOT / 'src' / file).read_text(encoding='utf-8')
            snippets.append(definition(source, f'{owner}& {owner}::{getter}('))
            snippets.append(definition(source, f'FastBee::FeatureInstanceSnapshot {owner}::observeFeatureInstance('))
            if owner == 'LCDManager':
                snippets.append(definition(source, f'{owner}::{owner}('))
            if owner in ('IRRemoteDriver', 'RFLearnDriver'):
                snippets.append(definition(source, f'bool {owner}::ensureMutex('))
        screen = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        snippets.append('namespace FastBee { namespace Screen {')
        snippets.extend(definition(screen, signature) for signature in (
            'bool ensureRuntimeSynchronization(', 'ScreenRuntime& ScreenRuntime::instance(',
            'FeatureInstanceSnapshot ScreenRuntime::observeFeatureInstance('))
        snippets.append('}}')
        (temporary / 'owner_observation_methods.inc').write_text('\n\n'.join(snippets), encoding='utf-8')
        guards = (ROOT / 'include/core/AsyncExecTypes.h').read_text(encoding='utf-8')
        (temporary / 'owner_observation_guard.inc').write_text(
            definition(guards, 'class RecursiveMutexGuard') + ';', encoding='utf-8')
        cls.executables = {}
        for backend in (0, 1):
            executable = temporary / (f'owners-{backend}.exe' if os.name == 'nt' else f'owners-{backend}')
            result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                f'-DFASTBEE_ENABLE_SCREEN_UI={backend}', '-DARDUINO_ARCH_ESP32=1',
                f'-I{ROOT / "include"}', f'-I{temporary}',
                str(ROOT / 'test/unit/cpp/feature_owner_observation_harness.cpp'), '-o', str(executable)],
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.executables[backend] = executable

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Owner observation inputs changed during tests')

    def run_case(self, mode, backend=1):
        result = subprocess.run([str(self.executables[backend]), mode], capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cold_registry_queries_create_no_owner_or_mutex(self): self.run_case('cold')
    def test_business_getters_register_their_actual_owner(self): self.run_case('registration')
    def test_lcd_retained_allocation_and_ir_rf_real_lifecycle(self): self.run_case('owners')
    def test_lcd1602_flag_and_rule_object_are_not_claimed_running(self): self.run_case('scope')
    def test_lock_contention_is_zero_wait_unknown_without_old_counts(self): self.run_case('busy')
    def test_queries_remain_passive_during_owner_transitions(self): self.run_case('concurrent')
    def test_missing_mutex_is_unknown_and_business_retry_can_recover(self): self.run_case('allocation_failure')
    def test_concurrent_ir_rf_getters_publish_one_lifetime_lock(self): self.run_case('owner_sync_concurrent')
    def test_screen_stop_timeout_retains_retiring_until_task_releases(self): self.run_case('screen')
    def test_screen_sync_allocation_retries_without_replacing_live_lock(self): self.run_case('screen_sync')
    def test_concurrent_first_start_publishes_only_one_sync_pair(self): self.run_case('screen_sync_concurrent')
    def test_screen_without_backend_is_explicitly_unknown(self): self.run_case('disabled', backend=0)


if __name__ == '__main__':
    unittest.main()
