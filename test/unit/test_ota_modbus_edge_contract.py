import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PROTOCOL_ROUTE = (
    ROOT / "src/network/handlers/ProtocolRouteHandler.cpp"
).read_text(encoding="utf-8")
MODBUS_HANDLER = (ROOT / "src/protocols/ModbusHandler.cpp").read_text(
    encoding="utf-8"
)
MQTT_CLIENT = (ROOT / "src/protocols/MQTTClient.cpp").read_text(encoding="utf-8")
OTA_MANAGER = (ROOT / "src/network/OTAManager.cpp").read_text(encoding="utf-8")


def function_body(source, signature, next_signature=None):
    start = source.index(signature)
    end = source.index(next_signature, start) if next_signature else len(source)
    return source[start:end]


def modbus_crc(frame):
    crc = 0xFFFF
    for value in frame:
        crc ^= value
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if crc & 1 else crc >> 1
    return crc


def valid_rtu_frame(frame):
    if not 4 <= len(frame) <= 256:
        return False
    crc = modbus_crc(frame[:-2])
    return frame[-2:] == bytes((crc & 0xFF, crc >> 8))


def classify_rtu_payload(payload):
    ascii_frame = None
    try:
        if len(payload) >= 8 and len(payload) % 2 == 0:
            ascii_frame = bytes.fromhex(payload.decode("ascii"))
    except (UnicodeDecodeError, ValueError):
        pass

    if valid_rtu_frame(payload):
        return "binary", payload
    if ascii_frame is not None and valid_rtu_frame(ascii_frame):
        return "hex-ascii", ascii_frame
    return "invalid", b""


class OtaModbusEdgeContractTests(unittest.TestCase):
    def test_modbus_transfer_type_is_strict_at_api_and_runtime_boundaries(self):
        normalize = function_body(
            PROTOCOL_ROUTE,
            "void normalizeProtocolConfig",
            "bool loadProtocolConfigDocument",
        )
        save = function_body(
            PROTOCOL_ROUTE,
            "void ProtocolRouteHandler::handleSaveProtocolConfig",
        )
        sanitize = function_body(
            MODBUS_HANDLER,
            "void ModbusHandler::sanitizeConfig",
            "bool ModbusHandler::begin(const String&",
        )

        self.assertIn("transferType != 0 && transferType != 1", normalize)
        self.assertIn('requestedTransferType != "0"', save)
        self.assertIn('requestedTransferType != "1"', save)
        self.assertIn("ctx->sendError(request, 400", save)
        self.assertNotIn(
            'GPI("modbusRtu_transferType"', save,
            "invalid or non-numeric transfer types must not silently become JSON mode",
        )
        self.assertIn("config.transferType != 0 && config.transferType != 1", sanitize)
        self.assertIn("config.transferType = 0;", sanitize)

    def test_raw_payload_classifier_uses_crc_and_prefers_valid_binary(self):
        decoder = function_body(
            MQTT_CLIENT,
            "static MqttModbusPayloadEncoding decodeModbusRtuPayload",
            "static uint8_t currentModbusTransferType",
        )
        ascii_probe = decoder.index("const bool hexAsciiValid = decodeModbusRtuHex")
        binary_probe = decoder.index("if (mqttModbusFrameHasValidCrc(payload, length))")
        binary_result = decoder.index("MqttModbusPayloadEncoding::BINARY", binary_probe)
        ascii_result = decoder.index("if (hexAsciiValid)", binary_result)
        self.assertLess(ascii_probe, binary_probe)
        self.assertLess(binary_probe, binary_result)
        self.assertLess(binary_result, ascii_result)

        encoding, frame = classify_rtu_payload(b"010300010001D5CA")
        self.assertEqual("hex-ascii", encoding)
        self.assertEqual(bytes.fromhex("010300010001D5CA"), frame)

        # This is a valid six-byte RTU ADU whose every byte is also an ASCII
        # HEX character. Character-only detection would corrupt it to 0A B3 6E.
        ascii_shaped_binary = b"0Ab36E"
        self.assertTrue(valid_rtu_frame(ascii_shaped_binary))
        encoding, frame = classify_rtu_payload(ascii_shaped_binary)
        self.assertEqual("binary", encoding)
        self.assertEqual(ascii_shaped_binary, frame)

        self.assertEqual(("invalid", b""), classify_rtu_payload(b"010300010001D5CB"))

    def test_ota_begin_is_serialized_before_update_begin(self):
        begin = function_body(
            OTA_MANAGER,
            "bool OTAManager::beginOTA",
            "size_t OTAManager::writeData",
        )
        async_start = function_body(
            OTA_MANAGER,
            "bool OTAManager::startOTAAsync",
            "String OTAManager::getOTAStatus",
        )

        claim = begin.index("s_otaSessionStarting = true")
        update_begin = begin.index("Update.begin")
        session_active = begin.index("otaInProgress = true", update_begin)
        release = begin.index("s_otaSessionStarting = false", session_active)
        self.assertLess(claim, update_begin)
        self.assertLess(update_begin, session_active)
        self.assertLess(session_active, release)
        self.assertEqual(4, begin.count("releaseStartClaim();"))
        self.assertIn(
            "otaInProgress || s_otaSessionStarting || s_otaUrlTaskActive",
            async_start,
        )


if __name__ == "__main__":
    unittest.main()
