"""Compile the actual patched IDF5 transport with fault-injected SDK doubles.

No live SDK/dependency mutations or device access. These tests do not establish
real HAL timing, physical LEDs, RMT competition or long-strip power safety.
"""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('fastbee_neopixel_patch', ROOT / 'scripts/apply-libdeps-patches.py')
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


class LibraryBuildEnvironment:
    """Only include paths supplied by the real pre-build hook reach this TU."""
    def __init__(self):
        self.includes = []

    def PrependUnique(self, *, CPPPATH):
        self.includes = list(dict.fromkeys(CPPPATH + self.includes))


class NeoPixelPatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='fastbee-neopixel-patch-')
        self.addCleanup(self.temp.cleanup)
        self.library = Path(self.temp.name)
        (self.library / 'library.properties').write_text('version=1.15.5\n', encoding='utf-8')
        self.original, self.patched = PATCH._neopixel_patched_source()
        (self.library / 'esp.c').write_text(self.original, encoding='utf-8')
        # Tiny synthetic ABI fixtures exercise digest validation without copying
        # the whole upstream class into the repository. esp.c is the real source.
        hashes = {}
        for name in ('Adafruit_NeoPixel.cpp', 'Adafruit_NeoPixel.h'):
            content = 'reviewed fixture: ' + name
            (self.library / name).write_text(content, encoding='utf-8')
            hashes[name] = hashlib.sha256(content.encode()).hexdigest()
        patcher = mock.patch.dict(PATCH._NEOPIXEL_UPSTREAM_SHA256, hashes)
        patcher.start(); self.addCleanup(patcher.stop)

    def apply(self):
        return PATCH.apply_to_neopixel_library(self.library, log=lambda _: None)

    def test_applies_once_preserves_license_and_legacy_branch(self):
        self.assertTrue(self.apply()); self.assertFalse(self.apply())
        actual = (self.library / 'esp.c').read_text(encoding='utf-8')
        self.assertEqual(actual, self.patched)
        self.assertEqual(actual.split('#ifdef HAS_ESP_IDF_5')[0], self.original.split('#ifdef HAS_ESP_IDF_5')[0])
        marker = '#include "driver/rmt.h"'
        self.assertEqual(actual[actual.index(marker):], self.original[self.original.index(marker):])

    def test_scons_project_root_does_not_depend_on_file_global(self):
        with mock.patch.object(PATCH, '__file__', None):
            self.assertTrue(PATCH.apply_to_neopixel_library(self.library, project_dir=ROOT, log=lambda _: None))

    def test_exact_pre_lto_patch_upgrades_but_modified_predecessor_is_retained(self):
        previous = self.patched.replace(
            'void espShow(uint16_t pin, uint8_t *pixels, uint32_t numBytes, uint8_t is800KHz)',
            'void espShow(uint8_t pin, uint8_t *pixels, uint32_t numBytes, boolean is800KHz)').replace(
            "  // Match Adafruit_NeoPixel.cpp's C ABI without truncating an invalid pin.\n"
            '  if (pin > UINT8_MAX) return;\n', '')
        self.assertEqual(hashlib.sha256(previous.encode()).hexdigest(), PATCH._NEOPIXEL_PRE_LTO_PATCH_SHA256)
        (self.library / 'esp.c').write_text(previous + '// changed\n', encoding='utf-8')
        with self.assertRaises(PATCH.PatchError): self.apply()
        self.assertEqual((self.library / 'esp.c').read_text(encoding='utf-8'), previous + '// changed\n')
        (self.library / 'esp.c').write_text(previous, encoding='utf-8')
        self.assertTrue(self.apply()); self.assertFalse(self.apply())

    def test_failed_upgrade_restores_exact_previous_patch(self):
        previous = self.patched.replace('uint16_t pin, uint8_t *pixels', 'uint8_t pin, uint8_t *pixels').replace(
            'uint8_t is800KHz) {', 'boolean is800KHz) {').replace(
            "  // Match Adafruit_NeoPixel.cpp's C ABI without truncating an invalid pin.\n"
            '  if (pin > UINT8_MAX) return;\n', '')
        self.assertEqual(hashlib.sha256(previous.encode()).hexdigest(), PATCH._NEOPIXEL_PRE_LTO_PATCH_SHA256)
        (self.library / 'esp.c').write_text(previous, encoding='utf-8')
        real_write = PATCH._write
        def failing_write(path, text):
            if text == self.patched:
                real_write(path, 'interrupted upgrade')
                raise OSError('injected upgrade failure')
            real_write(path, text)
        with mock.patch.object(PATCH, '_write', side_effect=failing_write):
            with self.assertRaises(PATCH.PatchError): self.apply()
        self.assertEqual((self.library / 'esp.c').read_text(encoding='utf-8'), previous)

    def test_canonical_abi_path_handles_spaces_without_a_libdeps_copy(self):
        project = self.library / 'project with spaces'
        header = project / 'include/peripherals/NeoPixelTransport.h'
        header.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / 'include/peripherals/NeoPixelTransport.h', header)
        env = LibraryBuildEnvironment()
        with mock.patch.object(PATCH, '__file__', None):
            PATCH.configure_neopixel_include_path(env, project)
            PATCH.configure_neopixel_include_path(env, project)
        self.assertEqual(env.includes, [str(project.resolve() / 'include')])
        self.assertFalse((self.library / 'peripherals/NeoPixelTransport.h').exists())

    def test_missing_or_changed_abi_rejects_before_build_flags_change(self):
        project = self.library / 'project'
        env = LibraryBuildEnvironment()
        with self.assertRaises(PATCH.PatchError):
            PATCH.configure_neopixel_include_path(env, project)
        header = project / 'include/peripherals/NeoPixelTransport.h'
        header.parent.mkdir(parents=True)
        header.write_text('unreviewed ABI', encoding='utf-8')
        with self.assertRaises(PATCH.PatchError):
            PATCH.configure_neopixel_include_path(env, project)
        self.assertEqual(env.includes, [])

    def test_unknown_version_and_abi_drift_are_nonmutating(self):
        (self.library / 'library.properties').write_text('version=1.15.6\n', encoding='utf-8')
        with self.assertRaises(PATCH.PatchError): self.apply()
        self.assertEqual((self.library / 'esp.c').read_text(encoding='utf-8'), self.original)
        (self.library / 'library.properties').write_text('version=1.15.5\n', encoding='utf-8')
        (self.library / 'Adafruit_NeoPixel.cpp').write_text('unreviewed', encoding='utf-8')
        with self.assertRaises(PATCH.PatchError): self.apply()
        self.assertEqual((self.library / 'esp.c').read_text(encoding='utf-8'), self.original)

    def test_partial_or_altered_patch_is_never_overwritten(self):
        self.apply()
        changed = self.patched.replace('pdMS_TO_TICKS(100)', 'RMT_WAIT_FOR_EVER')
        (self.library / 'esp.c').write_text(changed, encoding='utf-8')
        with self.assertRaises(PATCH.PatchError): self.apply()
        self.assertEqual((self.library / 'esp.c').read_text(encoding='utf-8'), changed)

    def test_write_failure_restores_the_original(self):
        real_write = PATCH._write
        def failing_write(path, text):
            if text == self.patched:
                real_write(path, 'interrupted write')
                raise OSError('injected I/O fault')
            real_write(path, text)
        with mock.patch.object(PATCH, '_write', side_effect=failing_write):
            with self.assertRaises(PATCH.PatchError): self.apply()
        self.assertEqual((self.library / 'esp.c').read_text(encoding='utf-8'), self.original)


class NeoPixelTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler: raise unittest.SkipTest('C++17 compiler required')
        cls.inputs = [ROOT / path for path in (
            'scripts/apply-libdeps-patches.py', 'include/peripherals/NeoPixelTransport.h',
            'patches/libdeps/Adafruit_NeoPixel@1.15.5/esp.upstream.c',
            'patches/libdeps/Adafruit_NeoPixel@1.15.5/esp-idf5.c',
            'test/unit/cpp/neopixel_transport_harness.cpp.in', 'test/unit/test_neopixel_transport.py')]
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-neopixel-backend-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        for name in ('Arduino.h', 'esp_arduino_version.h', 'esp32-hal-periman.h', 'esp_heap_caps.h'):
            (folder / name).write_text('#pragma once\n', encoding='utf-8')
        (folder / 'patched-esp.c').write_text(PATCH._neopixel_patched_source()[1], encoding='utf-8')
        harness = folder / 'backend.cpp'
        harness.write_text(cls.inputs[4].read_text(encoding='utf-8'), encoding='utf-8')
        cls.binary = folder / 'backend.exe'
        build_env = LibraryBuildEnvironment()
        PATCH.configure_neopixel_include_path(build_env, ROOT)
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            f'-I{folder}', *[f'-I{item}' for item in build_env.includes], str(harness), '-o', str(cls.binary)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
        if result.returncode: raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError('NeoPixel transport inputs changed during tests')

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_completed_write_latch_encoding_and_release(self): self.check('ok')
    def test_init_failure_releases_mutex_and_quarantines(self): self.check('init')
    def test_timeout_and_late_completion_never_free_or_reuse_buffer(self): self.check('timeout')
    def test_failed_deinit_retains_pin_and_never_retries_sdk(self): self.check('release')
    def test_wrong_pin_and_reassigned_owner_are_never_detached(self): self.check('owner')
    def test_overflow_and_allocation_failure_preserve_existing_buffer(self): self.check('allocation')
    def test_concurrent_first_constructor_publishes_one_mutex(self): self.check('concurrent')
    def test_mutex_contention_returns_failure_without_hardware_io(self): self.check('busy')
    def test_upstream_abi_and_oversized_pin_never_alias_gpio(self): self.check('compatibility')


class NeoPixelIntegrationTests(unittest.TestCase):
    """Actual control/remove methods and relevant setup/mutation branches.

Unrelated PM validation and peripheral drivers are omitted, not replaced with
success claims. The add tail includes the real failure rollback erasures.
"""
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler: raise unittest.SkipTest('C++17 compiler required')
        cls.inputs = [ROOT / path for path in (
            'src/core/PeripheralManager.cpp', 'src/core/PeripheralManagerDiagnostics.cpp',
            'src/core/PeripheralManagerConfigHelpers.h', 'include/peripherals/NeoPixelTransport.h',
            'include/core/FeatureInstanceRegistry.h', 'test/mocks/Arduino.h',
            'test/unit/cpp/neopixel_integration_harness.cpp.in', 'test/unit/test_neopixel_transport.py',
            'include/core/NeoPixelResourceSnapshot.h', 'include/core/ResourceConsumerIdentity.h')]
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        pm, diagnostics, config_helpers = [path.read_text(encoding='utf-8') for path in cls.inputs[:3]]
        helpers = definition(pm, 'struct NeoPixelRgb {') + ';\n'
        helpers += '\n'.join(definition(pm, signature) for signature in (
            'bool parseNeoPixelColor(', 'NeoPixelRgb rainbowColorAt(', 'NeoPixelRgb applyNeoPixelBrightness('))
        helpers += '\n' + '\n'.join(definition(config_helpers, signature) for signature in (
            'inline uint16_t clampNeoPixelCount(', 'inline uint8_t clampNeoPixelBrightness('))
        owner = pm[pm.index('static Adafruit_NeoPixel* g_neopixelInstance'):]
        owner = owner[:owner.index('\n#endif')]
        owner += '\n' + definition(pm, 'bool fastbeeNeoPixelProjectPinRetained(')
        setup = definition(pm, 'bool PeripheralManager::setupHardware(')
        setup_prefix = setup[setup.index('{') + 1:setup.index('    if (controlOutputIsClaimed')]
        setup_branch = definition(setup, 'if (config.type == PeripheralType::NEO_PIXEL) {')
        teardown = definition(pm, 'bool PeripheralManager::teardownHardware(')
        teardown_branch = definition(teardown, 'if (config.type == PeripheralType::NEO_PIXEL) {')
        add = definition(pm, 'bool PeripheralManager::addPeripheral(const PeripheralConfig& config, String& errorMsg)')
        add_tail = add[add.index('    // 添加外设'):add.rindex('}')]
        update = definition(pm, 'bool PeripheralManager::updatePeripheral(const String& id, const PeripheralConfig& config, String& errorMsg)')
        rollback = definition(pm, 'bool PeripheralManager::restoreMutationSnapshot(')
        rollback = rollback[rollback.index('    auto removeCurrent ='):]
        rollback = rollback[:rollback.index('    _memorySuspendedCount = snapshot.memorySuspendedCount;')]
        # Transport/lifecycle regression below calls the private frame routine.
        # test_ordinary_neopixel exercises the public source/lease boundary.
        code = cls.inputs[6].read_text(encoding='utf-8').replace('controlNeoPixel(', 'controlNeoPixelFrameLocked(')
        replacements = {
            '// @helpers@': helpers, '// @owner@': owner,
            '// @solid@': definition(pm, 'bool writeNeoPixelSolid('),
            '// @control@': definition(pm, 'bool PeripheralManager::controlNeoPixelFrameLocked('),
            '// @remove@': definition(pm, 'bool PeripheralManager::removePeripheral('),
            '// @conflict@': definition(diagnostics, 'bool PeripheralManager::checkPinConflict('),
            '// @setup@': setup_prefix + '\n' + setup_branch + '\nreturn true;\n',
            '// @teardown@': '\n' + teardown_branch + '\nreturn true;\n',
            '// @add-tail@': '\n' + add_tail,
            '// @update-stop@': definition(update, 'if (oldConfig.enabled || (hadOldState && oldState.memoryCleanupPending)) {'),
            '// @rollback-current@': rollback,
            '// @snapshot@': definition(pm, 'void PeripheralManager::snapshotNeoPixelResources('),
        }
        for marker, replacement in replacements.items(): code = code.replace(marker, replacement)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-neopixel-integration-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name); cpp = folder / 'integration.cpp'
        cpp.write_text(code, encoding='utf-8'); cls.binary = folder / 'integration.exe'
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            f'-I{ROOT / "test/mocks"}', f'-I{ROOT / "include"}', str(cpp), '-o', str(cls.binary)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
        if result.returncode: raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError('NeoPixel integration inputs changed during tests')

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_all_animation_and_rainbow_failures_keep_frame_and_report_error(self):
        for action in ('chase', 'theater_chase', 'strobe', 'twinkle', 'fade', 'breathing',
                       'color_wipe', 'fire', 'rainbow', 'cycle', 'next', 'red', 'off'):
            with self.subTest(action=action): self.check('animation:' + action)

    def test_init_failure_rollback_cannot_make_retained_pin_reusable(self): self.check('rollback')
    def test_pre_transmit_oom_failed_teardown_keeps_visible_config_and_pin(self): self.check('rollback_oom')
    def test_pre_transmit_busy_failed_teardown_keeps_visible_config_and_pin(self): self.check('rollback_busy')
    def test_failed_remove_and_update_preserve_configuration_and_object(self): self.check('remove')
    def test_successful_control_advances_and_remove_releases(self): self.check('success')
    def test_snapshot_keeps_real_identity_and_separate_allocation_and_transport_results(self): self.check('snapshot')
    def test_snapshot_busy_is_zero_wait_and_clears_reused_storage(self): self.check('snapshot_busy')
    def test_new_managed_pin_guard_rejects_before_neopixel_traffic(self): self.check('managed_conflict')


if __name__ == '__main__': unittest.main()
