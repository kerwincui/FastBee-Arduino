"""Validate the public, disabled generic I2C configuration fragments."""

import json
import math
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "docs/generic-i2c-peripherals"


def read_json(name):
    return json.loads((EXAMPLES / name).read_text(encoding="utf-8"))


def enum_values(path, name):
    source = path.read_text(encoding="utf-8")
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    body = re.search(r"enum\s+class\s+" + name + r"[^\{]*\{([^}]+)", source).group(1)
    values, value = {}, -1
    for entry in body.split(","):
        parts = entry.strip().split("=")
        if not parts[0]:
            continue
        value = int(parts[1].strip(), 0) if len(parts) == 2 else value + 1
        values[parts[0].strip()] = value
    return values


class GenericI2cExamplesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.peripherals_doc = read_json("peripherals.json")
        cls.rules_doc = read_json("periph_exec.json")
        cls.peripherals = cls.peripherals_doc["peripherals"]
        cls.rules = cls.rules_doc["rules"]
        cls.by_id = {item["id"]: item for item in cls.peripherals}
        cls.types = enum_values(ROOT / "include/core/PeripheralTypes.h", "PeripheralType")
        cls.actions = enum_values(ROOT / "include/core/PeripheralExecution.h", "ExecActionType")
        cls.triggers = enum_values(ROOT / "include/core/PeripheralExecution.h", "ExecTriggerType")

    def test_fragments_are_public_disabled_and_have_unique_ids(self):
        self.assertEqual(set(self.peripherals_doc), {"peripherals"})
        self.assertEqual(set(self.rules_doc), {"rules"})
        self.assertEqual(len(self.by_id), len(self.peripherals))
        self.assertEqual(len({rule["id"] for rule in self.rules}), len(self.rules))
        for item in self.peripherals + self.rules:
            self.assertIs(item["enabled"], False)
            self.assertTrue(0 < len(item["id"].encode("utf-8")) <= 40)
            self.assertTrue(0 < len(item["name"].encode("utf-8")) <= 48)
        forbidden = {"password", "passwd", "token", "secret", "ssid", "devicenum",
                     "productid", "username", "auth", "users", "network", "protocol"}

        def check(value):
            if isinstance(value, dict):
                for key, child in value.items():
                    self.assertNotIn(key.lower(), forbidden)
                    check(child)
            elif isinstance(value, list):
                for child in value:
                    check(child)
            elif isinstance(value, str) and value.startswith("{"):
                check(json.loads(value))

        check(self.peripherals_doc)
        check(self.rules_doc)

    def test_bus_and_channel_references_are_complete_and_nonconflicting(self):
        addresses, channels = set(), set()
        for item in self.peripherals:
            params = item["params"]
            self.assertIn(item["type"], self.types.values())
            if item["type"] == self.types["I2C"]:
                self.assertEqual(len(item["pins"]), 2)
                self.assertEqual(len(set(item["pins"])), 2)
                self.assertIs(params["isMaster"], True)
                self.assertEqual(params["address"], 0)
                self.assertEqual(params["frequency"], 100000)
            if "busPeripheralId" in params:
                bus = self.by_id[params["busPeripheralId"]]
                self.assertEqual(bus["type"], self.types["I2C"])
                self.assertEqual(item["pins"], [])
                address_key = (bus["id"], params["address"])
                self.assertNotIn(address_key, addresses)
                addresses.add(address_key)
            if item["type"] == self.types["IO_EXPANDER"]:
                self.assertEqual(params["driver"], "PCF8574")
                self.assertTrue(0x20 <= params["address"] <= 0x27)
                self.assertTrue(20 <= params["sampleInterval"] <= 1000)
            if params.get("ioSource") == 1:
                parent = self.by_id[params["providerId"]]
                self.assertEqual(parent["type"], self.types["IO_EXPANDER"])
                self.assertIn(item["type"], [self.types["GPIO_DIGITAL_INPUT"],
                                             self.types["GPIO_DIGITAL_OUTPUT"]])
                self.assertEqual(item["pins"], [])
                self.assertTrue(0 <= params["channel"] < 8)
                key = (parent["id"], params["channel"])
                self.assertNotIn(key, channels)
                channels.add(key)
                self.assertEqual(params["inputMode"], 0)
                self.assertIs(params["passive"], False)
                self.assertIn(params["initialState"], [0, 1])
        self.assertEqual(len(channels), 2)

    def test_ina226_example_has_explicit_calibration_and_bounded_timing(self):
        item = self.by_id["power_meter"]
        self.assertEqual(item["type"], self.types["SENSOR_GENERIC"])
        params = item["params"]
        self.assertEqual(params["driver"], "INA226")
        self.assertTrue(0x40 <= params["address"] <= 0x4F)
        for key in ("shuntResistance", "maxCurrent"):
            self.assertTrue(math.isfinite(params[key]))
            self.assertGreater(params[key], 0)
        self.assertLessEqual(params["shuntResistance"] * params["maxCurrent"], 0.0819175)
        current_lsb = params["maxCurrent"] / 32767
        calibration = min(32767, math.floor(0.00512 / (current_lsb * params["shuntResistance"])))
        self.assertTrue(1 <= calibration <= 32767)
        self.assertIn(params["conversionTimeUs"], [140, 204, 332, 588, 1100, 2116, 4156, 8244])
        self.assertIn(params["averaging"], [1, 4, 16, 64, 128, 256, 512, 1024])
        self.assertLessEqual(2 * params["conversionTimeUs"] * params["averaging"], 500000)
        self.assertTrue(100 <= params["sampleInterval"] <= 86400000)

    def test_execution_actions_decode_and_reference_real_measurement_channels(self):
        header = (ROOT / "include/core/PeripheralExecution.h").read_text(encoding="utf-8")
        max_triggers = int(re.search(r"#define MAX_TRIGGERS_PER_RULE\s+(\d+)", header).group(1))
        max_actions = int(re.search(r"#define MAX_ACTIONS_PER_RULE\s+(\d+)", header).group(1))
        units = {"voltage": "V", "current": "A", "power": "W"}
        measured = set()
        for rule in self.rules:
            self.assertIs(rule["reportAfterExec"], False)
            self.assertTrue(0 < len(rule["triggers"]) <= max_triggers)
            self.assertTrue(0 < len(rule["actions"]) <= max_actions)
            for trigger in rule["triggers"]:
                if trigger["triggerType"] == self.triggers["EVENT_TRIGGER"]:
                    self.assertEqual(trigger["eventId"], "button_click")
                    self.assertEqual(self.by_id[trigger["triggerPeriphId"]]["type"],
                                     self.types["GPIO_DIGITAL_INPUT"])
                else:
                    self.assertEqual(trigger["triggerType"], self.triggers["TIMER_TRIGGER"])
                    self.assertEqual(trigger["intervalSec"], 1)
            for action in rule["actions"]:
                target = self.by_id[action["targetPeriphId"]]
                self.assertIsInstance(action["actionValue"], str)
                self.assertIs(action["useReceivedValue"], False)
                if action["actionType"] == self.actions["ACTION_SENSOR_READ"]:
                    value = json.loads(action["actionValue"])
                    self.assertEqual(value["periphId"], target["id"])
                    self.assertEqual(value["sensorCategory"], target["params"]["driver"])
                    self.assertEqual(target["id"], "power_meter")
                    self.assertEqual(value["unit"], units[value["channelField"]])
                    self.assertEqual(value["dataField"], "supply_" + value["channelField"])
                    self.assertNotIn("driverParams", value)
                    self.assertEqual(value["scaleFactor"], 1)
                    self.assertEqual(value["offset"], 0)
                    self.assertNotIn(value["channelField"], measured)
                    measured.add(value["channelField"])
                else:
                    self.assertEqual(action["actionType"], self.actions["ACTION_GPIO_TOGGLE"])
                    self.assertEqual(target["type"], self.types["GPIO_DIGITAL_OUTPUT"])
                    self.assertEqual(action["actionValue"], "")
        self.assertEqual(measured, set(units))

    def test_docs_link_examples(self):
        readme = (EXAMPLES / "README.md").read_text(encoding="utf-8")
        index = (ROOT / "docs/README.md").read_text(encoding="utf-8")
        ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
        expected = set(re.findall(r"^\[env:(esp32(?:c3|c6|s3)?-F\d+R\d+)\]$", ini, re.M))
        self.assertEqual(len(expected), 9)
        for name in ("peripherals.json", "periph_exec.json"):
            self.assertIn(f"]({name})", readme)
        self.assertIn("](generic-i2c-peripherals/README.md)", index)


if __name__ == "__main__":
    unittest.main()
