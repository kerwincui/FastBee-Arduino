"""Run the complete production history service with real JSON and faulty storage."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PointHistoryPersistenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-history-persistence-')
        cls.binary = Path(cls.temp.name) / ('history.exe' if os.name == 'nt' else 'history')
        includes = [ROOT / 'test/unit/fixtures/point_history', ROOT / 'test/mocks',
                    ROOT / '.pio/libdeps/esp32s3-F16R8/ArduinoJson/src', ROOT / 'include']
        result = subprocess.run([
            compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DFASTBEE_ENABLE_POINT_HISTORY=1', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
            *[f'-I{path}' for path in includes],
            '-include', str(ROOT / 'test/unit/cpp/fixed_json_document_prelude.h'),
            str(ROOT / 'test/unit/cpp/point_history_persistence_harness.cpp'),
            str(ROOT / 'src/network/FixedJsonField.cpp'),
            '-o', str(cls.binary)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            cls.temp.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def scenario(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_real_json_roundtrip_restores_ten_samples_and_boot_identity(self):
        self.scenario('roundtrip')

    def test_continuous_sampling_flushes_at_deadline_without_idle_writes(self):
        self.scenario('deadline')

    def test_dirty_data_rides_recent_log_flush_only_inside_coalesce_window(self):
        self.scenario('coalesce')

    def test_flush_windows_survive_millis_wraparound(self):
        self.scenario('wraparound')

    def test_sample_arriving_during_write_remains_dirty_and_is_saved_next(self):
        self.scenario('during_write')

    def test_truncated_primary_restores_backup_ignoring_incomplete_temporary(self):
        self.scenario('backup_restore')

    def test_malformed_json_schema_numbers_and_limits_restore_no_fake_samples(self):
        self.scenario('malformed')

    def test_failed_writes_keep_primary_and_dirty_state_then_recover(self):
        for mode in ['open', 'short', 'corrupt', 'rename_old', 'rename_new', 'remove_backup']:
            with self.subTest(failure=mode):
                self.scenario(mode)

    def test_power_interruption_between_renames_restores_backup(self):
        self.scenario('power_interruption')

    def test_unit_and_mapping_change_persist_only_new_generation(self):
        self.scenario('revision')

    def test_capacity_matches_configured_or_restored_sources_and_stays_bounded(self):
        self.scenario('capacity')

    def test_config_conflict_concurrent_writer_and_failed_save_preserve_state(self):
        self.scenario('config_transaction')

    def test_config_rejects_valid_json_corruption_nul_suffix_and_revision_wrap(self):
        self.scenario('config_integrity')


if __name__ == '__main__':
    unittest.main()
