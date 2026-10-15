"""D9 contract: MQTT publish path no longer heap-allocates whole-payload Strings."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / 'src/protocols/MQTTClient.cpp').read_text(encoding='utf-8')
HDR = (ROOT / 'include/protocols/MQTTClient.h').read_text(encoding='utf-8')


def region(source, start, end):
    i = source.index(start)
    return source[i:source.index(end, i)]


FLUSH = region(SRC, 'void MQTTClient::processQueuedReports()',
               'void MQTTClient::setMinReportInterval')
IMPL = region(SRC, 'bool MQTTClient::publishToTopicImpl(size_t topicIndex, const char* message',
              'int MQTTClient::findEnabledPublishTopicIndex(')
INFO = region(SRC, 'bool MQTTClient::publishDeviceInfo()',
              'bool MQTTClient::publishMonitorData()')
NTP = region(SRC, 'bool MQTTClient::publishNtpSync()',
             'bool MQTTClient::subscribe(const String& topic)')
EVENT = region(SRC, 'bool MQTTClient::publishDeviceEvent(',
               'bool MQTTClient::publishNtpSync()')
GUARD = region(SRC, 'struct MqttPublishBufferGuard',
               '// Keep the SSE payload self-describing')
ENCODE = region(SRC, 'bool MQTTClient::queueReportDataEncoded(',
                'bool MQTTClient::rejectInvalidReport()')


class MqttPublishStagingContractTests(unittest.TestCase):
    def test_flush_path_publishes_slot_bytes_without_string_copy(self):
        # 热点路径：消费槽固定缓冲直发，不构造 String 整包副本
        self.assertNotIn('String pendingPayload', FLUSH)
        self.assertIn('const char* pendingBytes = _pendingReportSlot.payload;', FLUSH)
        self.assertIn('publishReportData(pendingBytes, pendingLength)', FLUSH)
        self.assertIn('publishModbusRtuReport(pendingBytes, pendingLength)', FLUSH)
        self.assertIn('publishPreparedTopic(_pendingReportSlot.topicIndex, pendingBytes, pendingLength)', FLUSH)
        # 分配失败降级路径随 String 副本一起消除
        self.assertNotIn('allocationDeferred', FLUSH)

    def test_impl_publishes_caller_buffer_with_explicit_length(self):
        # 直发路径：长度化 publish，无 String payload / truncatedPayload 副本
        self.assertIn('const uint8_t* outBytes = reinterpret_cast<const uint8_t*>(message);', IMPL)
        self.assertIn('mqttClient.publish(fullTopic.c_str(), outBytes,', IMPL)
        self.assertNotIn('String payload = message;', IMPL)
        self.assertNotIn('String truncatedPayload', IMPL)
        # 规则脚本转换仍是唯一 String 分支，且 prepared 直发结构性绕过
        self.assertIn('const bool bypassTransform = prepared ||', IMPL)

    def test_staging_buffer_is_static_pool_guarded_not_task_stack(self):
        # 固定缓冲来自静态池且 acquire/release 在临界区内（跨任务互斥），不落任务栈
        self.assertIn('g_mqttPublishPool.acquire();', GUARD)
        self.assertIn('g_mqttPublishPool.release(buffer);', GUARD)
        self.assertEqual(GUARD.count('portENTER_CRITICAL(&g_mqttPublishPoolMux);'), 2)
        self.assertEqual(GUARD.count('portEXIT_CRITICAL(&g_mqttPublishPoolMux);'), 2)
        self.assertIn('static portMUX_TYPE g_mqttPublishPoolMux', SRC)
        self.assertIn('FastBee::StringBufferPool<512, 4> g_mqttPublishPool;', SRC)

    def test_serialize_paths_use_staging_with_string_fallback(self):
        for name, body in (('deviceInfo', INFO), ('ntpSync', NTP), ('deviceEvent', EVENT)):
            self.assertIn('MqttPublishBufferGuard staging;', body, name)
        # 池耗尽/超容回退仅保留在低频路径
        self.assertIn('serializeJson(doc, staging.buffer, MQTT_PUBLISH_STAGING_CAPACITY)', INFO)
        self.assertIn('snprintf(staging.buffer, MQTT_PUBLISH_STAGING_CAPACITY,', NTP)

    def test_oversize_payload_rejected_not_truncated(self):
        # 入队侧既有拒绝
        self.assertIn('length > MQTT_SLOT_MAX_PAYLOAD', ENCODE)
        # 设备事件：上界超容拒绝而非截断 JSON
        self.assertIn('const size_t worstCase = 40U + 2U * (eventId.length() + value.length());', EVENT)
        self.assertIn('if (worstCase > MQTT_PUBLISH_STAGING_CAPACITY) {', EVENT)
        self.assertIn('MQTT: DEVICE_EVENT payload exceeds %u bytes, rejected eventId=%s', EVENT)

    def test_header_declares_zero_copy_overloads(self):
        for decl in (
            'bool publishToTopic(size_t topicIndex, const char* message, size_t length);',
            'bool publishPreparedTopic(size_t topicIndex, const char* message, size_t length);',
            'bool publishReportData(const char* payload, size_t length);',
            'bool publishModbusRtuReport(const char* hexPayload, size_t length);',
            'bool publishToTopicImpl(size_t topicIndex, const char* message, size_t length, bool prepared);',
        ):
            self.assertIn(decl, HDR)


if __name__ == '__main__':
    unittest.main()
