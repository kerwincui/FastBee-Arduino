"""Actual rule owner lifecycle, raw disabled MQTT payloads and concurrent retirement."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class RuleScriptLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-rule-lifecycle-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / ('lifecycle.exe' if os.name == 'nt' else 'lifecycle')
        source = (ROOT/'src/core/RuleScriptManager.cpp').read_text(encoding='utf-8')
        header = (ROOT/'include/core/RuleScriptManager.h').read_text(encoding='utf-8')
        fixture = (ROOT/'test/unit/cpp/rule_script_lifecycle_harness.cpp.in').read_text(encoding='utf-8')
        for marker, signature in {
            '@OBSERVE@': 'FastBee::FeatureInstanceSnapshot RuleScriptManager::observeFeatureInstance(',
            '@INITIALIZE@': 'bool RuleScriptManager::initialize(',
            '@CLEANUP@': 'void RuleScriptManager::cleanupIfIdle(',
            '@STOP@': 'bool RuleScriptManager::stop(',
            '@REPORT@': 'bool RuleScriptManager::tryApplyReportTransform(',
        }.items():
            fixture = fixture.replace(marker, definition(source, signature))
        fixture = fixture.replace('@ACCESS_GUARD@', definition(header, 'class AccessGuard') + ';')
        path = Path(cls.directory.name) / 'lifecycle.cpp'
        path.write_text(fixture, encoding='utf-8')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson required')
            json_root = str(headers[0].parent)
        built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-declarations', '-I'+str(ROOT/'include'), '-I'+json_root,
            str(path), str(ROOT/'src/utils/SharedJsonSerializer.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_protocol_callers_respect_disabled_policy_and_fixed_protocol_exceptions(self):
        mqtt = (ROOT/'src/protocols/MQTTClient.cpp').read_text(encoding='utf-8')
        queue = definition(mqtt, 'bool MQTTClient::queueTopicMessage(')
        self.assertIn('isUserEnabled("ruleScript")', queue)
        self.assertIn('copyCheckedText(payload, message.c_str(), message.length(), MQTT_SLOT_MAX_PAYLOAD)', queue)
        receive = definition(mqtt, 'void MQTTClient::mqttCallback(')
        self.assertIn('!isOtaProtocol && !isRawModbusCommand &&', receive)
        self.assertIn('shouldEnable("ruleScript")', receive)
        route = (ROOT/'src/network/handlers/RuleScriptRouteHandler.cpp').read_text(encoding='utf-8')
        self.assertIn('RuleScriptManager::AccessGuard access(mgr)', route)
        self.assertNotIn('mgr.getMutex()', route)


for _case in ('disabled_raw', 'initialize_once', 'retire_with_reader', 'cancel_initialization',
              'failure_recovery', 'policy_denied', 'disabled_loaded_raw',
              'concurrent_retirement', 'concurrent_initialization'):
    setattr(RuleScriptLifecycleTests, 'test_'+_case, lambda self, case=_case:self.check(case))

if __name__ == '__main__':
    unittest.main()
