"""Contracts for peripheral lifecycle, capability, and API ownership boundaries."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
TYPES_H = (ROOT / "include/core/PeripheralTypes.h").read_text(encoding="utf-8")
MANAGER_CPP = (ROOT / "src/core/PeripheralManager.cpp").read_text(encoding="utf-8")
IO_CPP = (ROOT / "src/core/PeripheralManagerIo.cpp").read_text(encoding="utf-8")
PERSISTENCE_CPP = (
    ROOT / "src/core/PeripheralManagerPersistence.cpp"
).read_text(encoding="utf-8")
ROUTE_CPP = (
    ROOT / "src/network/handlers/PeripheralRouteHandler.cpp"
).read_text(encoding="utf-8")
ROUTE_CPP += "\n" + (ROOT / "src/network/handlers/PeripheralCatalogRoutes.cpp").read_text(encoding="utf-8")
PROTOCOL_CPP = (ROOT / "src/protocols/ProtocolManager.cpp").read_text(
    encoding="utf-8"
)
EXECUTOR_CPP = (ROOT / "src/core/PeriphExecExecutor.cpp").read_text(
    encoding="utf-8"
)


def function_body(source: str, signature: str) -> str:
    match = re.search(signature + r"[^;{]*\{", source, re.S)
    if match is None:
        raise AssertionError(f"missing C++ function matching {signature!r}")
    opening = source.find("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated C++ function matching {signature!r}")


class PeripheralLifecycleCapabilityContractTests(unittest.TestCase):
    def test_io_expander_references_round_trip_and_invalid_parents_propagate(self):
        save = function_body(
            PERSISTENCE_CPP, r"\bPeripheralManager::saveConfiguration\s*\("
        )
        load = function_body(
            PERSISTENCE_CPP, r"\bPeripheralManager::loadConfiguration\s*\("
        )
        self.assertIn("serializePersistedPeripheral(obj, config)", save)
        self.assertIn("parsePersistedPeripheral(obj, config, parseError)", load)
        serializer = function_body(PERSISTENCE_CPP, r"\bserializePersistedPeripheral\s*\(")
        parser = function_body(PERSISTENCE_CPP, r"\bparsePersistedPeripheral\s*\(")
        for field in (
            "ioSource",
            "providerId",
            "channel",
            "driver",
            "busPeripheralId",
            "address",
            "sampleInterval",
        ):
            self.assertIn(f'FastBee::setFixedJsonField(params, "{field}",', serializer)
            self.assertIn(f'params["{field}"]', parser)

        dependencies = function_body(
            MANAGER_CPP, r"\bPeripheralManager::validateIoDependencies\s*\("
        )
        resolve_bus = function_body(
            MANAGER_CPP, r"\bPeripheralManager::resolveI2cConfig\s*\("
        )
        self.assertIn("runtimeStates.find(parent->first)", dependencies)
        self.assertIn("parentConfigInvalid", dependencies)
        self.assertIn("runtimeStates.find(bus->first)", resolve_bus)
        self.assertIn("busConfigInvalid", resolve_bus)
        self.assertIn('startsWith("CONFIG_INVALID:")', dependencies)
        self.assertIn('startsWith("CONFIG_INVALID:")', resolve_bus)
        self.assertIn("validateIoDependencies(item.second, item.first", load)

    def test_fixed_peripheral_ids_use_bounded_string_comparisons(self):
        helper = function_body(MANAGER_CPP, r"\bfixedIdEquals\s*\(")
        managed_helper = function_body(
            MANAGER_CPP, r"\bmanagedI2cReferenceEquals\s*\("
        )
        self.assertIn("id.length() < N", helper)
        self.assertIn("std::strncmp(storedId, id.c_str(), N)", helper)
        self.assertIn("id.length() < capacity", managed_helper)
        self.assertIn(
            "std::strncmp(storedId, id.c_str(), capacity)", managed_helper
        )
        self.assertGreaterEqual(MANAGER_CPP.count("fixedIdEquals("), 9)
        self.assertGreaterEqual(MANAGER_CPP.count("managedI2cReferenceEquals("), 3)
        self.assertNotRegex(
            MANAGER_CPP,
            r"params\.(?:gpio\.providerId|(?:ioExpander|sensor)\.busPeripheralId)\s*(?:==|!=)\s*"
            r"(?:id|config\.id|providerId|item\.first)",
        )

    def test_expander_action_validation_stays_in_execution_path(self):
        report = function_body(EXECUTOR_CPP, r"\bbuildActionReportValue\s*\(")
        execute = function_body(
            EXECUTOR_CPP, r"\bPeriphExecExecutor::executePeripheralAction\s*\("
        )
        self.assertNotIn("getPeripheralSnapshot", report)
        self.assertNotIn("return false", report)
        validation = execute.index("digitalTarget.isExpanderChannel()")
        # The earlier switch dispatches native DO through the ordinary arbiter.
        # This contract binds the legacy expander validation to its own switch.
        dispatch = execute.index("switch (actType)", validation)
        self.assertLess(validation, dispatch)
        for token in (
            "getPeripheralSnapshot",
            "PeripheralType::GPIO_DIGITAL_OUTPUT",
            "ExecActionType::ACTION_BLINK",
            "interval < 200L",
            "interval > 65535L",
            "GPIOState::STATE_UNDEFINED",
        ):
            self.assertIn(token, execute[:dispatch])

    def test_gpio_state_parameter_is_not_shadowed_by_runtime_lookup(self):
        write = function_body(
            MANAGER_CPP, r"\bPeripheralManager::writeGpioStateLocked\s*\("
        )
        self.assertNotRegex(write, r"auto\s+state\s*=\s*runtimeStates\.find")
        self.assertIn("auto runtime = runtimeStates.find(config.id)", write)
        self.assertIn("state == GPIOState::STATE_HIGH", write)
        self.assertIn("ok ? state : GPIOState::STATE_UNDEFINED", write)

    def test_setup_reaches_real_initializers_instead_of_teardown(self):
        setup = function_body(
            MANAGER_CPP, r"\bPeripheralManager::setupHardware\s*\("
        )
        for token in (
            "configureAndMount",
            "DS1302Driver::getInstance().begin",
            "LCD1602Driver::getInstance().begin",
            "RFIDDriver::getInstance().begin",
            "config.type == PeripheralType::SENSOR_GENERIC",
        ):
            self.assertIn(token, setup)
        for teardown_token in (
            ".unmount(config.id",
            "DS1302Driver::getInstance().end",
            "LCD1602Driver::getInstance().end",
            "RFIDDriver::getInstance().stop",
            "SensorDriverCache::instance().release",
        ):
            self.assertNotIn(teardown_token, setup)

    def test_teardown_is_symmetric_for_special_peripherals(self):
        teardown = function_body(
            MANAGER_CPP, r"\bPeripheralManager::teardownHardware\s*\("
        )
        for token in (
            "SDCardManager::getInstance().unmount",
            "DS1302Driver::getInstance().end",
            "LCD1602Driver::getInstance().end",
            "RFIDDriver::getInstance().stop",
            "SensorDriverCache::instance().release",
        ):
            self.assertIn(token, teardown)
        self.assertIn("releaseConfiguration", (ROOT / "include/peripherals/SDCardManager.h").read_text(encoding="utf-8"))
        self.assertNotIn("SPI.end()", teardown[teardown.index("PeripheralType::SDIO") : teardown.index("PeripheralType::DS1302_RTC")])

    def test_spi_transactions_assert_and_release_chip_select(self):
        write = function_body(IO_CPP, r"\bPeripheralManager::writeData\s*\(")
        read = function_body(IO_CPP, r"\bPeripheralManager::readData\s*\(")
        write_spi = write[
            write.index("case PeripheralType::SPI") :
            write.index("case PeripheralType::RF_MODULE")
        ]
        read_spi = read[
            read.index("case PeripheralType::SPI") :
            read.index("case PeripheralType::RADAR_SENSOR")
        ]
        for block in (write_spi, read_spi):
            self.assertIn("pinCount < 4", block)
            self.assertIn("isInputOnlyPin", block)
            self.assertLess(block.index("digitalWrite(cs, LOW)"), block.index("SPI.transfer"))
            self.assertLess(block.index("SPI.transfer"), block.index("digitalWrite(cs, HIGH)"))
            self.assertLess(block.index("digitalWrite(cs, HIGH)"), block.index("SPI.endTransaction"))

    def test_touch_fails_closed_until_real_semantics_exist(self):
        implemented = function_body(TYPES_H, r"\bisPeripheralTypeImplemented\s*\(")
        setup = function_body(MANAGER_CPP, r"\bPeripheralManager::setupHardware\s*\(")
        setup_gpio = function_body(MANAGER_CPP, r"\bPeripheralManager::setupGPIOPin\s*\(")
        read = function_body(IO_CPP, r"\bPeripheralManager::readData\s*\(")
        digital_read = read[: read.index("case PeripheralType::ADC")]
        self.assertNotIn("case PeripheralType::GPIO_TOUCH", implemented)
        self.assertIn("config.type == PeripheralType::GPIO_TOUCH", setup)
        self.assertIn("case PeripheralType::GPIO_TOUCH", setup_gpio)
        self.assertNotIn("PeripheralType::GPIO_TOUCH", digital_read)

    def test_compiled_capabilities_and_ir_parser_are_truthful(self):
        self.assertIn('#include "core/FeatureFlags.h"', TYPES_H)
        self.assertIn('strcasecmp(typeStr, "IR_RECEIVER")', TYPES_H)
        self.assertIn('strcasecmp(typeStr, "IR")', TYPES_H)
        for macro in (
            "FASTBEE_ENABLE_LCD",
            "FASTBEE_ENABLE_NEOPIXEL",
            "FASTBEE_ENABLE_SEVEN_SEGMENT",
            "FASTBEE_ENABLE_DS1302",
            "FASTBEE_ENABLE_LCD1602",
            "FASTBEE_ENABLE_RFID",
            "FASTBEE_ENABLE_MODBUS",
            "FASTBEE_ENABLE_IR_REMOTE",
        ):
            self.assertIn(f"return {macro} != 0", TYPES_H)

    def test_modbus_targets_are_protocol_owned_not_ordinary_crud(self):
        self.assertIn("isPeripheralTypeUserConfigurable", TYPES_H)
        descriptor = function_body(
            ROUTE_CPP, r"\baddPeripheralTypeDescriptorDetails\s*\("
        )
        for token in (
            'FastBee::setFixedJsonField(typeObj, "implemented", false)',
            'FastBee::setFixedJsonField(typeObj, "runtimeManaged", true)',
            'FastBee::setFixedJsonField(typeObj, "owner", "protocol.modbus")',
            'FastBee::setFixedJsonField(typeObj, "configurable", isPeripheralTypeUserConfigurable(type))',
        ):
            self.assertIn(token, descriptor)

        for handler in (
            "handleAddPeripheral",
            "handleUpdatePeripheral",
            "handleDeletePeripheral",
            "handleEnablePeripheral",
            "handleDisablePeripheral",
            "handleValidatePins",
        ):
            body = function_body(
                ROUTE_CPP, rf"\bPeripheralRouteHandler::{handler}\s*\("
            )
            self.assertTrue(
                "userConfigurablePeripheralTypeError" in body
                or "isPeripheralTypeUserConfigurable" in body,
                handler,
            )

        for wrapper in (
            "addPeripheralAndSave",
            "updatePeripheralAndSave",
            "removePeripheralAndSave",
            "enablePeripheralAndSave",
            "disablePeripheralAndSave",
        ):
            body = function_body(
                MANAGER_CPP, rf"\bPeripheralManager::{wrapper}\s*\("
            )
            self.assertIn("EXTERNALLY_MANAGED_PERIPHERAL_TYPE", body, wrapper)

        self.assertIn("pm.addPeripheral(pCfg)", PROTOCOL_CPP)


if __name__ == "__main__":
    unittest.main()
