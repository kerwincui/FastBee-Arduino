"""Contracts for retry-safe trigger admission and peripheral type parsing."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANAGER_H = (ROOT / "include/core/PeriphExecManager.h").read_text(encoding="utf-8")
MANAGER_CPP = (ROOT / "src/core/PeriphExecManager.cpp").read_text(encoding="utf-8")
PERIPHERAL_TYPES = (ROOT / "include/core/PeripheralTypes.h").read_text(
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


class TriggerAdmissionContractTests(unittest.TestCase):
    def test_shared_helper_commits_progress_only_after_dispatch_admission(self):
        helper = function_body(
            MANAGER_CPP,
            r"\bPeriphExecManager::dispatchAndCommitTrigger\s*\(",
        )
        dispatch_at = helper.index("dispatchByRuleId(")
        rejection_return_at = helper.index("return false", dispatch_at)
        timestamp_at = helper.index("trigger.lastTriggerTime = millis()")
        count_at = helper.index("trigger.triggerCount++", timestamp_at)
        self.assertLess(dispatch_at, rejection_return_at)
        self.assertLess(rejection_return_at, timestamp_at)
        self.assertLess(timestamp_at, count_at)
        self.assertIn("if (!dispatchByRuleId", helper)
        self.assertIn("portMAX_DELAY", helper)
        self.assertIn("dispatchAndCommitTrigger", MANAGER_H)

    def test_event_mqtt_and_poll_matchers_do_not_preconsume_progress(self):
        functions = (
            "dispatchEventMatchedRules",
            "dispatchPeriphExecEvent",
            "dispatchButtonEventRules",
            "processMqttMessageMatch",
            "processPollDataMatch",
        )
        for name in functions:
            with self.subTest(name=name):
                body = function_body(
                    MANAGER_CPP, rf"\bPeriphExecManager::{name}\s*\("
                )
                self.assertIn("dispatchAndCommitTrigger", body)
                self.assertNotIn("lastTriggerTime =", body)
                self.assertNotIn("triggerCount++", body)
                self.assertIn("triggerIndex", body)

    def test_batched_data_sources_keep_one_candidate_per_rule(self):
        for name in ("processMqttMessageMatch", "processPollDataMatch"):
            with self.subTest(name=name):
                body = function_body(
                    MANAGER_CPP, rf"\bPeriphExecManager::{name}\s*\("
                )
                self.assertIn("bool alreadyMatched = false", body)
                self.assertIn("pending.ruleId == rule.id", body)
                self.assertIn("if (alreadyMatched) continue", body)


class PeripheralTypeIntegerContractTests(unittest.TestCase):
    def test_integer_parser_whitelists_every_non_reserved_enum_member(self):
        parser = function_body(PERIPHERAL_TYPES, r"\bperipheralTypeFromInt\s*\(")
        valid_members = (
            "UNCONFIGURED",
            "UART",
            "I2C",
            "SPI",
            "CAN",
            "USB",
            "GPIO_DIGITAL_INPUT",
            "GPIO_DIGITAL_OUTPUT",
            "GPIO_DIGITAL_INPUT_PULLUP",
            "GPIO_DIGITAL_INPUT_PULLDOWN",
            "GPIO_ANALOG_INPUT",
            "GPIO_ANALOG_OUTPUT",
            "GPIO_PWM_OUTPUT",
            "GPIO_INTERRUPT_RISING",
            "GPIO_INTERRUPT_FALLING",
            "GPIO_INTERRUPT_CHANGE",
            "GPIO_TOUCH",
            "ADC",
            "DAC",
            "JTAG",
            "SWD",
            "LCD",
            "SDIO",
            "SENSOR_GENERIC",
            "CAMERA",
            "ETHERNET",
            "PWM_SERVO",
            "STEPPER_MOTOR",
            "ENCODER",
            "ONE_WIRE",
            "NEO_PIXEL",
            "SEVEN_SEGMENT_TM1637",
            "RF_MODULE",
            "RADAR_SENSOR",
            "DS1302_RTC",
            "MODBUS_DEVICE",
            "LCD1602",
            "IR_RECEIVER",
            "RFID_MFRC522",
            "DEVICE_EVENT",
        )
        for member in valid_members:
            with self.subTest(member=member):
                self.assertIn(f"case static_cast<int>(PeripheralType::{member})", parser)

        self.assertNotRegex(parser, r"case[^\n]*PeripheralType::RESERVED_46")
        self.assertNotIn("value >=", parser)
        self.assertNotIn("value <=", parser)
        self.assertIn("default:", parser)
        self.assertIn("return PeripheralType::UNCONFIGURED", parser)

    def test_recognized_but_unimplemented_types_still_reach_fail_closed_gate(self):
        parser = function_body(PERIPHERAL_TYPES, r"\bperipheralTypeFromInt\s*\(")
        implemented = function_body(
            PERIPHERAL_TYPES, r"\bisPeripheralTypeImplemented\s*\("
        )
        for member in ("CAN", "USB", "JTAG", "SWD", "CAMERA", "ETHERNET"):
            with self.subTest(member=member):
                self.assertIn(f"PeripheralType::{member}", parser)
                self.assertNotIn(f"case PeripheralType::{member}", implemented)
        self.assertIn("default:", implemented)
        self.assertIn("return false", implemented)


if __name__ == "__main__":
    unittest.main()
