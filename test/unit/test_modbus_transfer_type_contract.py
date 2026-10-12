import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ROUTE = (ROOT / "src/network/handlers/ProtocolRouteHandler.cpp").read_text(encoding="utf-8")
HANDLER = (ROOT / "src/protocols/ModbusHandler.cpp").read_text(encoding="utf-8")


def function_body(source, signature, next_signature):
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


class ModbusTransferTypeContractTests(unittest.TestCase):
    def test_save_api_strictly_accepts_only_json_or_raw(self):
        save = ROUTE[ROUTE.index("void ProtocolRouteHandler::handleSaveProtocolConfig"):]
        validation = save.index('requestedTransferType != "0" && requestedTransferType != "1"')
        assignment = save.index('FastBee::setFixedJsonField(doc, "modbusRtu", "transferType", currentTransferType);')
        persistence = save.index('ConfigStorage::getInstance().saveProtocolConfig(doc)')

        self.assertLess(validation, assignment)
        self.assertLess(assignment, persistence)
        self.assertNotIn('LittleFS.open(PROTOCOL_CONFIG_PATH, "w")', save)
        self.assertIn("ctx->sendError(request, 400", save[validation:assignment])
        self.assertIn('requestedTransferType == "1" ? 1 : 0', save[validation:assignment])

    def test_loaded_and_runtime_configs_fall_back_to_json(self):
        normalize = function_body(
            ROUTE,
            "void normalizeProtocolConfig",
            "bool loadProtocolConfigDocument",
        )
        self.assertIn("transferType != 0 && transferType != 1", normalize)
        self.assertIn('FastBee::setFixedJsonField(doc, "modbusRtu", "transferType", 0);', normalize)

        sanitize = function_body(
            HANDLER,
            "void ModbusHandler::sanitizeConfig",
            "bool ModbusHandler::initializePins",
        )
        self.assertIn("config.transferType != 0 && config.transferType != 1", sanitize)
        self.assertIn("config.transferType = 0;", sanitize)


if __name__ == "__main__":
    unittest.main()
