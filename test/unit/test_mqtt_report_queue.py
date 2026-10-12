"""Final-gate preparation: actual MQTT queue methods, mocked platform/transport only."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MqttReportQueueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-mqtt-report-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'reports.exe'
        header = (ROOT / 'include/protocols/MQTTClient.h').read_text(encoding='utf-8')
        source = (ROOT / 'src/protocols/MQTTClient.cpp').read_text(encoding='utf-8')
        template = (ROOT / 'test/unit/cpp/mqtt_report_queue_harness.cpp.in').read_text(encoding='utf-8')
        parts = {
            '@SLOTS@': header[header.index('enum class MqttPayloadEncoding'):header.index('// 主题类型枚举')],
            '@PUBLIC@': header[header.index('    struct QueueStats'):header.index('    bool publishNtpSync')],
            '@PRIVATE@': header[header.index('    MqttSlot _reportSlots'):header.index('    void mqttCallback')],
            '@DECODE@': source[source.index('static uint16_t mqttModbusCrc16'):source.index('enum class MqttModbusPayloadEncoding')],
            '@METHODS@': source[source.index('void MQTTClient::processQueuedReports()'):source.index('String MQTTClient::getStatus()')],
            '@TOPIC_ADMISSION@': source[source.index('bool MQTTClient::queueTopicMessage('):source.index('bool MQTTClient::publishToTopicImpl(')],
            '@TOPIC_PUBLISH@': source[source.index('bool MQTTClient::publishToTopicImpl('):source.index('int MQTTClient::findEnabledPublishTopicIndex(')],
        }
        for marker, value in parts.items():
            template = template.replace(marker, value)
        fixture = Path(cls.directory.name) / 'reports.cpp'
        fixture.write_text(template, encoding='utf-8')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson required')
            json_root = str(headers[0].parent)
        built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'include'), '-I'+json_root, str(fixture),
            str(ROOT/'src/network/FixedJsonField.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


for _case in ('test_isolation', 'test_retains_existing', 'test_counter_saturation',
              'empty', 'null', 'nul', 'oversize', 'cast_wrap', 'exact512', 'retained',
              'cooldown', 'wrap', 'allocation', 'corruption', 'overflow_pin', 'producers',
              'consumers', 'reentrant', 'instance_clock', 'saturation', 'throttle_invalid',
              'retire_once', 'retire_retained', 'retire_empty_preserves', 'retire_unknown',
              'retire_reasons', 'retire_saturation', 'retire_concurrent', 'retire_json_unavailable',
              'throttle_concurrent', 'json', 'json_unavailable', 'raw', 'heap', 'foreground',
              'topic_offline', 'topic_transform_frozen', 'topic_reject', 'topic_packet', 'topic_pinned', 'topic_test_isolation',
              'topic_route_retain', 'topic_flush_guards', 'topic_transform_failure'):
    setattr(MqttReportQueueTests, 'test_'+_case, lambda self, case=_case:self.check(case))


class MqttRetirementIntegrationTests(unittest.TestCase):
    def test_record_after_worker_drain_and_before_slot_clear(self):
        source = (ROOT/'src/protocols/MQTTClient.cpp').read_text(encoding='utf-8')
        body = source[source.index('MQTTClient::~MQTTClient()'):source.index('bool MQTTClient::loadMqttConfig')]
        self.assertLess(body.index('shutdown()'), body.index('recordReportQueueRetirement()'))
        self.assertLess(body.index('recordReportQueueRetirement()'), body.index('_reportSlots[i].clear()'))

    def test_manager_records_reason_only_when_destroying(self):
        source = (ROOT/'src/protocols/ProtocolManager.cpp').read_text(encoding='utf-8')
        for reason in ('ConfigurationDisabled', 'Rebuild', 'NetworkSwitch', 'Shutdown', 'InitializationFailed'):
            self.assertIn('setReportRetirementReason(FastBee::MqttRetirementReason::'+reason+')', source)
        start = source.index('bool ProtocolManager::finishMqttStop()')
        stop = source[start:source.index('void ProtocolManager::requestMqttRestartAsync()', start)]
        self.assertNotIn('setReportRetirementReason', stop)
        self.assertNotIn('mqttClient.reset()', stop)

    def test_initialization_retains_inflight_old_instance(self):
        source = (ROOT/'src/protocols/ProtocolManager.cpp').read_text(encoding='utf-8')
        init = source[source.index('bool ProtocolManager::initMQTT(void* config)'):]
        self.assertLess(init.index('if (!mqttClient->stop()) return false'),
                        init.index('mqttClient.reset()'))
        self.assertLess(init.index('mqttClient.reset()'), init.index('new (std::nothrow) MQTTClient()'))


if __name__ == '__main__':
    unittest.main()
