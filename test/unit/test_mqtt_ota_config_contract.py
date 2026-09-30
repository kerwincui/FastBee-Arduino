import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HANDLER = (ROOT / "src/network/handlers/ProtocolRouteHandler.cpp").read_text(encoding="utf-8")
MQTT_RUNTIME = (ROOT / "src/protocols/MQTTClient.cpp").read_text(encoding="utf-8")
FULL_LOADER = (ROOT / "web-src/modules/runtime/protocol/protocol-config.js").read_text(encoding="utf-8")
LITE_LOADER = (ROOT / "web-src/modules/runtime/protocol/protocol-lite-config.js").read_text(encoding="utf-8")
MQTT_UI = (ROOT / "web-src/modules/runtime/protocol/mqtt-config.js").read_text(encoding="utf-8")


def function_body(source, signature, next_signature):
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


class MqttOtaConfigContractTests(unittest.TestCase):
    def test_compact_and_full_get_apply_the_same_legacy_ota_migration(self):
        compact = function_body(
            HANDLER,
            "void sendCompactMqttConfigFromFile",
            "void sendRedactedProtocolConfigFromFile",
        )
        full = function_body(
            HANDLER,
            "void sendRedactedProtocolConfigFromFile",
            "void copyModbusTaskSummary",
        )
        self.assertIn("ensureFastBeeOtaTopics(mqttIn);", compact)
        self.assertIn("ensureFastBeeOtaTopics(mqtt);", full)
        self.assertIn('source["mqtt"].as<JsonObject>()', compact)
        self.assertIn('config["mqtt"].as<JsonObject>()', full)
        self.assertIn("if (mqttIn.isNull())", compact)
        self.assertIn("if (mqtt.isNull())", full)
        self.assertNotIn(
            'JsonObject mqttIn = source["mqtt"].to<JsonObject>();', compact
        )
        self.assertNotIn(
            'JsonObject mqtt = config["mqtt"].to<JsonObject>();', full
        )

    def test_legacy_migration_matches_runtime_complete_no_prefix_topics(self):
        helper = function_body(
            HANDLER,
            "void ensureFastBeeOtaTopic(",
            "void ensureFastBeeOtaTopics(",
        )
        for topic in (
            "/{deviceNum}/http/upgrade/reply",
            "/{deviceNum}/http/upgrade/set",
        ):
            self.assertIn(topic, HANDLER)
            self.assertIn(topic, MQTT_RUNTIME)
        self.assertIn('topic["autoPrefix"] = false;', helper)
        default_pos = helper.index("JsonObject topic = topics.add")
        self.assertNotIn('topic["enabled"]', helper[:default_pos])
        self.assertIn('topic["enabled"] = true;', helper[default_pos:])

    def test_runtime_ota_matching_uses_resolved_paths_and_removes_duplicates(self):
        normalizer = function_body(
            MQTT_RUNTIME,
            "void MQTTClient::normalizeFastBeeOtaTopics()",
            "void MQTTClient::mqttCallback",
        )
        lookup = function_body(
            MQTT_RUNTIME,
            "int MQTTClient::findFastBeeOtaReplyTopicIndex() const",
            "bool MQTTClient::hasUsableFastBeeOtaReplyTopic() const",
        )
        self.assertGreaterEqual(normalizer.count("buildFullTopicWithType"), 4)
        self.assertIn("config.publishTopics.erase", normalizer)
        self.assertIn("config.subscribeTopics.erase", normalizer)
        self.assertIn("buildFullTopicWithType", lookup)
        self.assertIn("if (!topic.enabled) return -1;", lookup)

    def test_post_save_normalizes_rebuilt_ota_topic_arrays(self):
        save = HANDLER[HANDLER.index("void ProtocolRouteHandler::handleSaveProtocolConfig"):]
        publish_rebuild = save.index('doc["mqtt"]["publishTopics"].to<JsonArray>()')
        subscribe_rebuild = save.index('doc["mqtt"]["subscribeTopics"].to<JsonArray>()')
        normalize = save.index('ensureFastBeeOtaTopics(doc["mqtt"].as<JsonObject>());')
        self.assertLess(publish_rebuild, normalize)
        self.assertLess(subscribe_rebuild, normalize)

    def test_selecting_ota_type_fills_fastbee_topics_without_prefix(self):
        listener = function_body(
            MQTT_UI,
            "_bindMqttBinaryOtaTopicGuard(container) {",
            "_syncMqttBinaryOtaTopicState(item, options) {",
        )
        self.assertIn("Number(target.value) === 5", listener)
        self.assertIn("topicInput.value = options.fastBeeOtaTopic", listener)
        self.assertIn("autoPrefixInput.checked = false", listener)
        self.assertIn("/{deviceNum}/http/upgrade/reply", MQTT_UI)
        self.assertIn("/{deviceNum}/http/upgrade/set", MQTT_UI)

    def test_ota_base_url_is_owned_by_device_ota_and_uses_shared_validation(self):
        self.assertNotIn("mqtt-ota-base-url", FULL_LOADER)
        self.assertNotIn("mqtt-ota-base-url", LITE_LOADER)
        self.assertNotIn("data.mqtt_otaBaseUrl", FULL_LOADER)
        ota = (ROOT / "web-src/modules/runtime/device-ota.js").read_text(encoding="utf-8")
        self.assertIn("section=ota&compact=true", ota)
        self.assertIn("{ mqtt_otaBaseUrl: baseUrl }", ota)
        validation = function_body(
            HANDLER,
            "bool validateOtaBaseUrl",
            "void normalizeProtocolConfig",
        )
        self.assertIn("FastBeeOtaProtocol::validateBaseUrl(value, error)", validation)

    def test_binary_ota_is_disabled_in_api_and_runtime(self):
        self.assertIn("disableUnsupportedBinaryOtaTopics(mqttIn);", HANDLER)
        self.assertIn("disableUnsupportedBinaryOtaTopics(mqtt);", HANDLER)
        self.assertIn("OTA_BINARY publish is not supported; topic forced disabled", MQTT_RUNTIME)
        self.assertIn("OTA_BINARY subscribe is not supported; topic forced disabled", MQTT_RUNTIME)


if __name__ == "__main__":
    unittest.main()
