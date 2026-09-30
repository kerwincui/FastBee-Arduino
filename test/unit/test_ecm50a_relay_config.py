"""Validate the field configuration, without claiming a hardware response."""
import json
import os
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
CONFIG = Path(os.environ.get(
    "FASTBEE_ECM50A_CONFIG_DIR", ROOT / "docs/ecm50a-s3-f16r8"))


def load(name):
    return json.loads((CONFIG / name).read_text(encoding="utf-8"))


class Ecm50aRelayConfigTests(unittest.TestCase):
    def test_onboard_nc_lamp_initializes_high_without_changing_relay_two(self):
        outputs = {p["id"]: p for p in load("peripherals.json")["peripherals"]}
        for ident, pin, initial in (("do1", 15, 1), ("do2", 16, 0)):
            with self.subTest(peripheral=ident):
                config = outputs[ident]
                self.assertTrue(config["enabled"])
                self.assertEqual(config["type"], 12)
                self.assertEqual(config["pins"], [pin])
                self.assertEqual(config["params"]["initialState"], initial)
                self.assertFalse(config["params"].get("invertLogic", False))

    def test_photoelectric_edge_toggles_onboard_relay_not_modbus(self):
        rules = {r["id"]: r for r in load("periph_exec.json")["rules"]}
        rule = rules["s3_photoelectric_relay_toggle"]
        self.assertTrue(rule["enabled"])
        self.assertEqual(len(rule["triggers"]), 1)
        trigger = rule["triggers"][0]
        self.assertEqual((trigger["triggerType"], trigger["triggerPeriphId"],
                          trigger["eventId"], trigger["compareValue"]),
                         (4, "di1", "ds:di1", "0"))
        self.assertEqual(len(rule["actions"]), 1)
        action = rule["actions"][0]
        self.assertEqual((action["targetPeriphId"], action["actionType"]), ("do1", 30))

    def test_boot_ready_turns_only_nc_channel_one_load_off(self):
        rules = {r["id"]: r for r in load("periph_exec.json")["rules"]}
        rule = rules["s3_modbus_relay_boot_off"]
        self.assertTrue(rule["enabled"])
        self.assertEqual(len(rule["triggers"]), 1)
        self.assertEqual(rule["triggers"][0]["triggerType"], 4)
        self.assertEqual(rule["triggers"][0]["eventId"], "system_ready")
        self.assertEqual(rule["triggers"][0]["compareValue"], "")
        self.assertEqual(len(rule["actions"]), 1)
        action = rule["actions"][0]
        self.assertEqual(action["actionType"], 18)
        self.assertEqual(action["targetPeriphId"], "modbus:0")
        self.assertEqual(json.loads(action["actionValue"]),
                         {"ctrl": [{"d": 0, "c": 0, "a": "off"}]})
        self.assertFalse(action["useReceivedValue"])
        framework = (ROOT / "src/core/FastBeeFramework.cpp").read_text(encoding="utf-8")
        self.assertIn("triggerEvent(EventType::EVENT_SYSTEM_READY", framework)

    def test_air_temperature_humidity_use_tenths_and_signed_temperature(self):
        task = next(t for t in load("protocol.json")["modbusRtu"]["master"]["tasks"]
                    if t["slaveAddress"] == 5)
        self.assertEqual((task["functionCode"], task["startAddress"], task["quantity"]), (4, 0, 2))
        mappings = task["mappings"]
        self.assertEqual([m["dataType"] for m in mappings], [1, 0])
        self.assertEqual([m["scaleFactor"] for m in mappings], [0.1, 0.1])

    def test_field_relay_is_four_channel_nc_register_control(self):
        protocol = load("protocol.json")["modbusRtu"]
        self.assertTrue(protocol["enabled"])
        self.assertEqual(protocol["mode"], "master")
        self.assertEqual(protocol["peripheralId"], "rs485")
        devices = protocol["master"]["devices"]
        self.assertEqual(len(devices), 1)
        relay = devices[0]
        expected = {
            "sensorId": "modbus_relay", "deviceType": "relay",
            "slaveAddress": 6, "channelCount": 4, "coilBase": 0,
            "ncMode": True, "controlProtocol": 1, "enabled": True,
        }
        for key, value in expected.items():
            with self.subTest(field=key):
                self.assertEqual(relay[key], value)
        uart = next(p for p in load("peripherals.json")["peripherals"]
                    if p["id"] == protocol["peripheralId"])
        self.assertTrue(uart["enabled"])
        self.assertEqual(uart["params"]["baudRate"], 9600)
        self.assertEqual(uart["pins"], [18, 17])

    def test_on_off_rules_address_device_index_and_first_channel(self):
        rules = {r["id"]: r for r in load("periph_exec.json")["rules"]}
        for rule_id, command in (("s3_modbus_relay", "on"),
                                 ("s3_modbus_relay_off", "off")):
            with self.subTest(rule=rule_id):
                rule = rules[rule_id]
                self.assertTrue(rule["enabled"])
                self.assertEqual(len(rule["actions"]), 1)
                action = rule["actions"][0]
                self.assertEqual(action["targetPeriphId"], "modbus:0")
                self.assertEqual(action["actionType"], 18)
                self.assertEqual(json.loads(action["actionValue"]),
                                 {"ctrl": [{"d": 0, "c": 0, "a": command}]})

    def test_display_names_are_chinese_and_protocol_ids_stay_stable(self):
        protocol = load("protocol.json")["modbusRtu"]["master"]
        objects = (protocol["devices"] + protocol["tasks"] +
                   load("peripherals.json")["peripherals"] +
                   load("periph_exec.json")["rules"])
        for item in objects:
            with self.subTest(name=item["name"]):
                self.assertRegex(item["name"], r"[\u4e00-\u9fff]")
                self.assertNotIn("\ufffd", item["name"])
        self.assertEqual(protocol["devices"][0]["sensorId"], "modbus_relay")
        rules = {r["id"] for r in load("periph_exec.json")["rules"]}
        self.assertIn("s3_modbus_relay", rules)
        self.assertIn("s3_modbus_relay_off", rules)


if __name__ == "__main__":
    unittest.main()
