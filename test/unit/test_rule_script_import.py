"""Prepared managed import schema/commit/load behavior with storage fault doubles."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RuleScriptImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-script-import-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name)/'import.exe'
        source = (ROOT/'src/core/RuleScriptManager.cpp').read_text(encoding='utf-8')
        template = (ROOT/'test/unit/cpp/rule_script_import_harness.cpp.in').read_text(encoding='utf-8')
        pairs = {
            '@IMPORT@': ('bool RuleScriptManager::importConfiguration(', 'bool RuleScriptManager::loadConfiguration('),
            '@LOAD@': ('bool RuleScriptManager::loadConfiguration(', '// ========== 模板引擎'),
            '@COPY@': ('bool RuleScriptManager::copyRule(', 'std::vector<RuleScript> RuleScriptManager::getAllRules('),
            '@UPDATE@': ('bool RuleScriptManager::updateRule(', 'bool RuleScriptManager::removeRule('),
            '@ADD@': ('bool RuleScriptManager::addRule(', 'bool RuleScriptManager::updateRule('),
            '@REMOVE@': ('bool RuleScriptManager::removeRule(', 'bool RuleScriptManager::copyRule('),
            '@TOGGLE@': ('bool RuleScriptManager::enableRule(', '// ========== 持久化'),
            '@COMMIT@': ('bool RuleScriptManager::commitRuleChange(', 'bool RuleScriptManager::saveConfiguration('),
        }
        for marker, (start, end) in pairs.items():
            begin = source.index(start)
            template = template.replace(marker, source[begin:source.index(end, begin)])
        fixture = Path(cls.directory.name)/'import.cpp'
        fixture.write_text(template, encoding='utf-8')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson required')
            json_root = str(headers[0].parent)
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
            '-I'+str(ROOT/'include'), '-I'+json_root, str(fixture), '-o', str(cls.binary),
            '-Wl,--wrap=malloc'], capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


for _case in ('commit','save_failure','busy','schema','values','identity_topics','empty_limit','copy_faults',
              'recovery_load','legacy_empty','snapshot_conflict','crud_save_failure','crud_busy','crud_commit',
              'crud_invalid','crud_copy_faults','copy_snapshot_failure','form_choices',
              'bounded_map','node_allocation_failure','load_json_allocation_failure',
              'memory_rejection','startup_limits','structural_backup_recovery','legacy_defaults'):
    setattr(RuleScriptImportTests, 'test_'+_case, lambda self, case=_case:self.check(case))

if __name__ == '__main__':
    unittest.main()
