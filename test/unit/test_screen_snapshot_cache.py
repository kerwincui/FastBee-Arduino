"""Exercise the production snapshot/cache paths with real ArduinoJson pools."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def definition(source, marker):
    start = source.index(marker)
    opening = source.index('{', start + len(marker))
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError(marker)


class ScreenSnapshotCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        fallback = Path('D:/msys64/ucrt64/bin/g++.exe')
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            raise RuntimeError('A C++17 compiler is required')
        cls.compiler = compiler
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-screen-snapshot-')
        temporary = Path(cls.directory.name)
        cls.environment = dict(os.environ)
        cls.environment['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.environment.get('PATH', '')
        manager = (ROOT / 'src/core/ScreenPanelManager.cpp').read_text(encoding='utf-8')
        dependencies = (ROOT / 'include/core/PeriphExecManager.h').read_text(encoding='utf-8')
        observation_types = dependencies[dependencies.index('enum class ObservationQuality'):dependencies.index('// 前向声明')]
        (temporary / 'snapshot_observation_types.inc').write_text(observation_types, encoding='utf-8')
        helpers = [
            definition(manager, 'std::vector<ObservationSnapshot> panelObservationSnapshots('),
            definition(manager, 'class PanelLock') + ';',
            definition(manager, 'template <size_t N>\nvoid copyText'),
            definition(manager, 'template <typename Snapshot>\nbool fillPanelSystemSource'),
            definition(manager, 'const char* panelAsyncExecErrorName('),
            definition(manager, 'bool panelAsyncExecStatusIsTerminal('),
            definition(manager, 'bool parseKind('),
            definition(manager, 'bool parseVisual('),
            definition(manager, 'ScreenValueQuality qualityFromObservation('),
            definition(manager, 'ScreenValueQuality qualityFromText('),
            definition(manager, 'String jsonScalar('),
        ]
        begin = manager.index('namespace {\n\n#if defined(ARDUINO_ARCH_ESP32)')
        end = manager.index('bool ScreenPanelManager::dispatchAction(', begin)
        (temporary / 'screen_snapshot_production.inc').write_text(
            '\n'.join(helpers) + '\n' + manager[begin:end], encoding='utf-8')
        cls.binaries = {}
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        for psram in (0, 1):
            binary = temporary / f'snapshot-{psram}.exe'
            result = subprocess.run([
                compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-Wno-class-memaccess', '-fstack-usage', '-DUNIT_TEST',
                '-DARDUINO_ARCH_ESP32=1', f'-DFASTBEE_USE_PSRAM={psram}',
                '-DFASTBEE_ENABLE_POINT_HISTORY=1', '-DFASTBEE_ENABLE_REMOTE_RESOURCES=1',
                '-DARDUINOJSON_ENABLE_STD_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
                *[f'-I{path}' for path in (temporary, ROOT / 'test/mocks', json_header.parent, ROOT / 'include')],
                str(ROOT / 'test/unit/cpp/screen_snapshot_cache_harness.cpp'),
                '-o', str(binary)], capture_output=True, text=True, encoding='utf-8',
                errors='replace', env=cls.environment, timeout=60)
            if result.returncode:
                cls.directory.cleanup()
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[psram] = binary

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def scenario(self, name):
        for psram, binary in self.binaries.items():
            with self.subTest(psram=psram):
                result = subprocess.run([str(binary), name], capture_output=True, text=True,
                                        env=self.environment, timeout=15)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_layout_is_parsed_once_and_hidden_cards_are_not_refreshed(self):
        self.scenario('reuse')

    def test_all_eight_pages_switch_without_rebuilding_or_losing_metadata(self):
        self.scenario('pages')

    def test_sensor_stale_error_unknown_and_unit_transitions_clear_old_values(self):
        self.scenario('sensor')

    def test_weather_and_remote_pending_are_refreshed_and_cleared(self):
        self.scenario('weather')

    def test_recent_history_and_series_refresh_without_old_samples(self):
        self.scenario('history')

    def test_only_visible_unfinished_operations_copy_recent_results(self):
        self.scenario('operations')

    def test_config_revision_output_rebind_and_invalidate_rebuild_safely(self):
        self.scenario('revision')

    def test_parse_or_psram_failure_preserves_old_snapshot_and_can_recover(self):
        self.scenario('failure')

    def test_web_full_snapshot_remains_independent_and_refreshes_every_page(self):
        self.scenario('web')

    def test_cards_and_chart_share_one_sample_when_publisher_advances_mid_refresh(self):
        self.scenario('consistent')

    def test_complete_manager_native_translation_unit_and_unity_cases(self):
        temporary = Path(self.directory.name)
        main = temporary / 'native-panel-main.cpp'
        main.write_text('''#include <unity.h>
void setUp() {}
void tearDown() {}
void test_screen_panel_manager_group();
int main() { UNITY_BEGIN(); test_screen_panel_manager_group(); return UNITY_END(); }
''', encoding='utf-8')
        unity = ROOT / '.pio/libdeps/native/Unity/src'
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        binary = temporary / 'native-panel.exe'
        result = subprocess.run([
            self.compiler, '-std=c++17', '-DUNIT_TEST', '-include', 'cmath',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-DARDUINOJSON_ENABLE_STD_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
            *[f'-I{path}' for path in (ROOT / 'test/mocks', ROOT / 'test/helpers', ROOT / 'include', json_header.parent, unity)],
            str(main), str(ROOT / 'test/test_screen_panel_manager.cpp'), str(unity / 'unity.c'),
            '-o', str(binary)], capture_output=True, text=True, encoding='utf-8',
            errors='replace', env=self.environment, timeout=60)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                env=self.environment, timeout=15)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('9 Tests 0 Failures 0 Ignored', result.stdout)


if __name__ == '__main__':
    unittest.main()
