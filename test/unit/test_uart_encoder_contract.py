import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CONFIG_H = (ROOT / "include/core/PeripheralConfig.h").read_text(encoding="utf-8")
MANAGER_H = (ROOT / "include/core/PeripheralManager.h").read_text(encoding="utf-8")
MANAGER_CPP = "\n".join(
    (ROOT / path).read_text(encoding="utf-8")
    for path in (
        "src/core/PeripheralManager.cpp",
        "src/core/PeripheralManagerIo.cpp",
        "src/core/PeripheralManagerPersistence.cpp",
    )
)
ROUTE_CPP = (ROOT / "src/network/handlers/PeripheralRouteHandler.cpp").read_text(encoding="utf-8")
EXEC_CPP = (ROOT / "src/core/PeriphExecExecutor.cpp").read_text(encoding="utf-8")
PERIPHERALS_JS = (ROOT / "web-src/modules/runtime/peripherals.js").read_text(encoding="utf-8")
PERIPHERAL_IO_JS = (ROOT / "web-src/modules/runtime/peripheral-io.js").read_text(encoding="utf-8")
PERIPHERAL_SAVE_JS = (ROOT / "web-src/modules/runtime/peripheral-save.js").read_text(encoding="utf-8")
EXEC_FORM_JS = (ROOT / "web-src/modules/runtime/periph-exec-form.js").read_text(encoding="utf-8")
EXEC_MODBUS_JS = (ROOT / "web-src/modules/runtime/periph-exec-modbus.js").read_text(encoding="utf-8")
EXEC_SENSOR_JS = (ROOT / "web-src/modules/runtime/periph-exec-sensor.js").read_text(encoding="utf-8")
MODALS_HTML = (ROOT / "web-src/pages/modals.html").read_text(encoding="utf-8")
PERIPHERAL_IO_HTML = (ROOT / "web-src/pages/fragments/peripheral-io.html").read_text(encoding="utf-8")


def function_body(source: str, signature_pattern: str) -> str:
    match = re.search(signature_pattern, source)
    if not match:
        raise AssertionError(f"function not found: {signature_pattern}")
    opening = source.find("{", match.end())
    if opening < 0:
        raise AssertionError(f"function body not found: {signature_pattern}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature_pattern}")


class UartContractTests(unittest.TestCase):
    def test_uart_defaults_and_all_framing_variants_exist(self):
        for token in (
            "UART_DEFAULT_BAUD_RATE",
            "UART_DEFAULT_DATA_BITS",
            "UART_DEFAULT_STOP_BITS",
            "UART_DEFAULT_PARITY",
            "uartFramingName",
        ):
            self.assertIn(token, CONFIG_H)

        framing = function_body(MANAGER_CPP, r"\buartSerialConfig\s*\(")
        for bits in range(5, 9):
            for parity in ("N", "O", "E"):
                for stops in (1, 2):
                    self.assertIn(f"SERIAL_{bits}{parity}{stops}", framing)

    def test_uart0_is_reserved_and_capacity_is_checked_before_mutation(self):
        self.assertNotIn("isUsbSerialPins", MANAGER_CPP)
        setup = function_body(
            MANAGER_CPP, r"\bPeripheralManager::setupUartHardware\s*\("
        )
        self.assertRegex(setup, r"used\s*\[\s*0\s*\]\s*=\s*true")
        self.assertIn("for (uint8_t i = 1", setup)

        capacity = function_body(
            MANAGER_CPP, r"\bPeripheralManager::validateUartCapacity\s*\("
        )
        self.assertIn("CHIP_UART_COUNT - 1", capacity)
        self.assertIn("UART_RESOURCE_EXHAUSTED", capacity)

        preflight_functions = (
            r"\bPeripheralManager::addPeripheral\s*\(\s*const PeripheralConfig&\s+config\s*,\s*String&",
            r"\bPeripheralManager::updatePeripheral\s*\(\s*const String&\s+id\s*,\s*const PeripheralConfig&\s+config\s*,\s*String&",
            r"\bPeripheralManager::enablePeripheral\s*\(\s*const String&\s+id",
        )
        for pattern in preflight_functions:
            body = function_body(MANAGER_CPP, pattern)
            self.assertIn("validateUartCapacity", body)

    def test_uart_persistence_runtime_and_bounded_io_contract(self):
        save = function_body(
            MANAGER_CPP, r"\bPeripheralManager::saveConfiguration\s*\("
        )
        load = function_body(
            MANAGER_CPP, r"\bPeripheralManager::loadConfiguration\s*\("
        )
        for field in ("baudRate", "dataBits", "stopBits", "parity"):
            self.assertIn(f'params["{field}"]', save)
            self.assertIn(f'params["{field}"]', load)

        self.assertIn("saturatingAddUint32", MANAGER_CPP)
        for field in (
            "bytesSent",
            "bytesReceived",
            "isConnected",
            "port",
            "rxAvailable",
        ):
            self.assertIn(field, MANAGER_H + MANAGER_CPP)

        parser = function_body(ROUTE_CPP, r"\bparseStrictHex\s*\(")
        self.assertIn("capacity", parser)
        self.assertIn("INVALID_HEX", parser)
        self.assertIn("PAYLOAD_TOO_LARGE", parser)
        write = function_body(
            ROUTE_CPP, r"\bPeripheralRouteHandler::handleWritePeripheral\s*\("
        )
        self.assertIn("uint8_t bytes[UART_API_MAX_BYTES]", write)
        self.assertNotIn("std::vector<uint8_t> bytes", write)

        read = function_body(
            ROUTE_CPP, r"\bPeripheralRouteHandler::handleReadPeripheral\s*\("
        )
        for field in (
            "framing",
            "bytesSent",
            "bytesReceived",
            "connected",
            "port",
            "rxAvailable",
        ):
            self.assertIn(field, ROUTE_CPP)
        self.assertIn("UART_API_MAX_BYTES", read)
        self.assertIn("printableAscii", read)

    def test_uart_web_terminal_uses_bounded_read_and_write_routes(self):
        for token in (
            "peripheral-io-modal",
            "peripheral-uart-payload",
            "peripheral-uart-text-output",
            "peripheral-uart-hex-output",
        ):
            self.assertIn(token, PERIPHERAL_IO_HTML)
        self.assertIn("apiPost('/api/peripherals/write'", PERIPHERAL_IO_JS)
        self.assertIn("'/api/peripherals/read'", PERIPHERAL_IO_JS)
        self.assertIn("info.bytes > 128", PERIPHERAL_IO_JS)
        self.assertIn("consumeData === true", PERIPHERAL_IO_JS)


class EncoderContractTests(unittest.TestCase):
    def test_encoder_configuration_round_trips(self):
        for token in (
            "ENCODER_DEFAULT_RESOLUTION",
            "initializeEncoderDefaults",
            "EncoderReading",
        ):
            self.assertIn(token, CONFIG_H)

        save = function_body(
            MANAGER_CPP, r"\bPeripheralManager::saveConfiguration\s*\("
        )
        load = function_body(
            MANAGER_CPP, r"\bPeripheralManager::loadConfiguration\s*\("
        )
        for field in ("resolution", "useInterrupt"):
            self.assertIn(f'params["{field}"]', save)
            self.assertIn(f'params["{field}"]', load)

        for handler in (
            "handleAddPeripheral",
            "handleUpdatePeripheral",
            "handleGetPeripheral",
            "handleReadPeripheral",
            "handleGetPeripheralStatus",
            "handleWritePeripheral",
        ):
            body = function_body(
                ROUTE_CPP, rf"\bPeripheralRouteHandler::{handler}\s*\("
            )
            self.assertIn("PeripheralType::ENCODER", body)

    def test_encoder_isr_has_no_fake_fallback_and_uses_both_phases(self):
        setup = function_body(MANAGER_CPP, r"\bPeripheralManager::setupHardware\s*\(")
        encoder_section = setup[setup.find("// ========== 编码器初始化") :]
        self.assertIn("ENCODER_RESOURCE_EXHAUSTED", encoder_section)
        self.assertNotIn("polling only", encoder_section)
        self.assertIsNotNone(
            re.search(r"if\s*\(\s*!slot\s*\).*?return false", encoder_section, re.S)
        )

        isr = function_body(
            MANAGER_CPP, r"\bPeripheralManager::handleEncoderInterrupt\s*\("
        )
        self.assertIn("slot->pinA", isr)
        self.assertIn("slot->pinB", isr)
        self.assertIn("phaseA == phaseB", isr)
        self.assertIn("edgePeriodMicros", isr)
        self.assertIn("slot->delta += step", isr)

    def test_encoder_read_reset_and_execution_are_real(self):
        read = function_body(
            MANAGER_CPP, r"\bPeripheralManager::getEncoderReading\s*\("
        )
        self.assertIn("1000000.0f", read)
        self.assertIn("frequencyHz * 60.0f", read)
        self.assertIn("slot->delta = 0", read)

        reset = function_body(
            MANAGER_CPP, r"\bPeripheralManager::resetEncoder\s*\("
        )
        for field in (
            "count",
            "delta",
            "direction",
            "lastEdgeMicros",
            "edgePeriodMicros",
        ):
            self.assertRegex(reset, rf"slot->{field}\s*=\s*0")

        exec_start = EXEC_CPP.index("bool PeriphExecExecutor::executeSensorReadAction")
        exec_end = EXEC_CPP.find("\nbool PeriphExecExecutor::", exec_start + 1)
        exec_body = EXEC_CPP[exec_start : exec_end if exec_end >= 0 else len(EXEC_CPP)]
        self.assertIn('strcmp(category, "pulse")', exec_body)
        self.assertIn('strcmp(category, "frequency")', exec_body)
        self.assertIn("getEncoderReading", exec_body)
        self.assertNotIn("pulse/frequency not yet implemented", exec_body)

    def test_encoder_web_defaults_and_execution_fields_match_firmware(self):
        self.assertIn('id="encoder-resolution" class="" value="20"', MODALS_HTML)
        self.assertIn('id="encoder-use-interrupt" checked disabled', MODALS_HTML)
        self.assertIn("data.resolution = document.getElementById('encoder-resolution')?.value || '20'", PERIPHERAL_SAVE_JS)
        self.assertIn("data.useInterrupt = '1'", PERIPHERAL_SAVE_JS)

        self.assertIn("else if (cat === 'pulse' || cat === 'frequency') allowedTypes = [43]", EXEC_SENSOR_JS)
        self.assertNotIn("pulseTypes = [46]", EXEC_SENSOR_JS)
        for field in ("count", "delta", "direction", "frequency", "rpm"):
            self.assertIn("['" + field + "'", EXEC_SENSOR_JS)
        self.assertIn("pulse: 'count'", EXEC_SENSOR_JS)
        self.assertIn("frequency: 'frequency'", EXEC_SENSOR_JS)


if __name__ == "__main__":
    unittest.main()
