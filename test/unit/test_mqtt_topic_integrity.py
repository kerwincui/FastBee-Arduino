"""Prepared source/helper behavior checks; run at the consolidated gate."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MqttTopicIntegrityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-topic-integrity-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'topics.exe'
        source = (ROOT/'src/core/RuleScriptManager.cpp').read_text(encoding='utf-8')
        start = source.index('bool RuleScriptManager::tryApplyReportTransform(')
        end = source.index('// ========== MQTT', start)
        template = (ROOT/'test/unit/cpp/mqtt_topic_integrity_harness.cpp.in').read_text(encoding='utf-8')
        fixture = Path(cls.directory.name) / 'topics.cpp'
        fixture.write_text(template.replace('@MANAGER@', source[start:end]), encoding='utf-8')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson required')
            json_root = str(headers[0].parent)
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-declarations', '-I'+str(ROOT/'include'), '-I'+json_root,
            str(fixture), str(ROOT/'src/utils/SharedJsonSerializer.cpp'),
            '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


for _case in ('paths', 'ota', 'topic_bounds', 'topic_faults', 'templates', 'template_bounds',
              'template_raw', 'template_faults', 'manager'):
    setattr(MqttTopicIntegrityTests, 'test_'+_case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
