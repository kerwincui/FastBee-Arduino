import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test.unit.test_screen_system_actions import braced_statement


ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT / "docs" / "device-test-configs" / "com8-com9-esp32s3-touch" / "config"


class WaveshareTouchLcd4BundleTests(unittest.TestCase):
    def setUp(self):
        self.docs = {
            path.name: json.loads(path.read_text(encoding="utf-8"))
            for path in BUNDLE.glob("*.json")
        }

    def test_preserved_names_are_valid_utf8_without_changing_bindings(self):
        labels = []
        def inspect(value):
            if isinstance(value, dict):
                for key, child in value.items():
                    if key in ('name', 'label', 'sensorLabel') and child:
                        labels.append(child)
                    elif key == 'actionValue' and isinstance(child, str) and child.startswith('{'):
                        inspect(json.loads(child))
                    else:
                        inspect(child)
            elif isinstance(value, list):
                for child in value:
                    inspect(child)
        for name in ('peripherals.json', 'periph_exec.json', 'protocol.json'):
            inspect(self.docs[name])
        self.assertGreaterEqual(len(labels), 16)
        for label in labels:
            self.assertNotIn('\ufffd', label)
            self.assertEqual(label, label.encode('utf-8').decode('utf-8'))
        collect = next(rule for rule in self.docs['periph_exec.json']['rules'] if rule['id'] == 'collect_sht30')
        actions = [json.loads(action['actionValue']) for action in collect['actions']]
        self.assertEqual(['air_temperature', 'air_humidity'], [action['dataField'] for action in actions])
        self.assertEqual(['temperature', 'humidity'], [action['channelField'] for action in actions])
        self.assertEqual(['Air temperature', 'Air humidity'], [action['sensorLabel'] for action in actions])

    def test_bundle_has_required_documents_and_no_live_credentials(self):
        for name in ("device.json", "network.json", "peripherals.json",
                     "periph_exec.json", "protocol.json"):
            self.assertIn(name, self.docs)
        device, network = self.docs["device.json"], self.docs["network.json"]
        self.assertEqual("", device["deviceId"])
        self.assertEqual(0, device["productNumber"])
        self.assertEqual("", device["userId"])
        self.assertEqual("", network["staSSID"])
        self.assertEqual("", network["staPassword"])
        self.assertEqual([], network.get("networks", []))
        mqtt = self.docs["protocol.json"]["mqtt"]
        self.assertFalse(mqtt["enabled"])
        for key in ("clientId", "username", "password", "mqttSecret", "authCode"):
            self.assertEqual("", mqtt[key])
        self.assertNotIn("auth.json", self.docs)
        self.assertNotIn("users.json", self.docs)
        weather = self.docs["protocol.json"]["weatherSources"]
        self.assertEqual(1, len(weather))
        self.assertEqual("home_weather", weather[0]["id"])
        self.assertTrue(weather[0]["enabled"])  # Preserve intent; the public API key remains blank.
        self.assertEqual("", weather[0]["apiKey"])

    def test_confirmed_bus_pins_and_sht30_schema(self):
        periphs = {item["id"]: item for item in self.docs["peripherals.json"]["peripherals"]}
        self.assertEqual([43, 44], periphs["rs485"]["pins"])
        self.assertEqual(1, periphs["rs485"]["type"])
        self.assertEqual([15, 7], periphs["panel_i2c"]["pins"])
        self.assertEqual(2, periphs["panel_i2c"]["type"])
        self.assertTrue(periphs["rs485"]["enabled"])
        self.assertTrue(periphs["panel_i2c"]["enabled"])
        sensor = periphs["sht30"]
        self.assertEqual("SHT30", sensor["params"]["driver"])
        self.assertEqual(68, sensor["params"]["address"])
        self.assertEqual("panel_i2c", sensor["params"]["busPeripheralId"])
        self.assertTrue(sensor["enabled"])
        self.assertNotIn("resolution", sensor["params"])
        self.assertNotIn("filter", sensor["params"])

    def test_confirmed_modbus_registers_and_editable_defaults(self):
        rtu = self.docs["protocol.json"]["modbusRtu"]
        self.assertTrue(rtu["enabled"])
        self.assertEqual(255, rtu["dePin"])
        self.assertEqual(4, rtu["slaveAddress"])  # Master default does not override per-task addresses.
        tasks = rtu["master"]["tasks"][1:]
        self.assertEqual([5, 7, 4, 10], [task["startAddress"] for task in tasks])
        self.assertTrue(all((task["slaveAddress"], task["functionCode"], task["quantity"]) ==
                            (8, 3, 1) for task in tasks))
        self.assertEqual(["motor_speed", "motor_pulse", "motor_count", "motor_baud_code"],
                         [task["mappings"][0]["sensorId"] for task in tasks])
        self.assertEqual(8, rtu["master"]["devices"][0]["slaveAddress"])
        rules = self.docs["periph_exec.json"]["rules"]
        enabled = {rule["id"]: rule["enabled"] for rule in rules}
        self.assertTrue(enabled["collect_sht30"])
        self.assertFalse(enabled["collect_pm"])
        self.assertFalse(enabled["collect_motor"])

    def test_sht30_cache_history_and_panel_use_matching_channels(self):
        rule = next(rule for rule in self.docs["periph_exec.json"]["rules"] if rule["id"] == "collect_sht30")
        self.assertTrue(rule["recordHistory"])
        self.assertFalse(rule["reportAfterExec"])
        actions = [json.loads(item["actionValue"]) for item in rule["actions"]]
        self.assertEqual({"temperature", "humidity"}, {a["channelField"] for a in actions})
        sensor_refs = {a["periphId"] + "_" + a["channelField"] for a in actions}
        history_refs = sensor_refs | {"modbus.pm25", "modbus.pm10"}
        self.assertEqual(history_refs, set(self.docs["point_history.json"]["sourceAllowlist"]))
        panel = self.docs["screen_panel.json"]
        self.assertEqual(1, panel["storageVersion"])
        cards = [card for page in panel["config"]["pages"] for card in page["cards"]]
        self.assertEqual(sensor_refs,
                         {card["sourceRef"] for card in cards
                          if card.get("sourceRef") and card["kind"] != "weather"})
        for card in cards:
            if card.get("visualType") == "line":
                self.assertEqual("chart", card["kind"])
                self.assertEqual("recent10", card["historyMode"])

    def test_rgb_touch_and_sht30_share_bus_without_mcu_reset_pins(self):
        display = next(item for item in self.docs["peripherals.json"]["peripherals"]
                       if item["type"] == 56)
        params = display["params"]
        self.assertEqual("rgb", params["backend"])
        self.assertEqual("st7701", params["driver"])
        self.assertEqual("ch32v003", params["controlBackend"])
        self.assertIs(params["backlightActiveHigh"], False)  # User-confirmed inverse wiring on this board.
        self.assertEqual(36, params["controlAddress"])
        self.assertEqual(32, params["controlEnableMask"])
        self.assertEqual(64, params["controlBuzzerMask"])
        self.assertEqual(0, params["controlInactiveMask"])
        self.assertEqual(4, params["controlTouchIntMask"])
        self.assertEqual(0, params["controlInactiveMask"] &
                         (params["controlEnableMask"] | params["controlResetMask"] |
                          params["controlBuzzerMask"]))
        self.assertEqual(0, params["controlTouchIntMask"] &
                         (params["controlEnableMask"] | params["controlResetMask"] |
                          params["controlInactiveMask"] | params["controlBacklightMask"] |
                          params["controlBuzzerMask"]))
        self.assertEqual(100000, params["i2cFrequency"])
        pins = params["pins"]
        self.assertEqual((15, 7), (pins["controlSda"], pins["controlScl"]))
        self.assertEqual((15, 7), (pins["touchSda"], pins["touchScl"]))
        self.assertEqual((-1, -1), (pins["touchInt"], pins["touchRst"]))
        self.assertEqual(16, len(set(pins["rgbDataPins"])))
        self.assertTrue(set(pins["rgbDataPins"]).isdisjoint({15, 7, 43, 44, 19, 20}))

    def test_unconnected_modbus_motor_template_is_not_scheduled(self):
        rules = self.docs["periph_exec.json"]["rules"]
        self.assertFalse(next(rule for rule in rules if rule["id"] == "collect_pm")["enabled"])
        poll_rule = next(rule for rule in rules if rule["id"] == "collect_motor")
        self.assertFalse(poll_rule["enabled"])
        self.assertTrue(poll_rule["recordHistory"])
        trigger = poll_rule["triggers"][0]
        self.assertEqual(5, trigger["triggerType"])
        self.assertEqual(10, trigger["intervalSec"])
        payloads = []
        for action in poll_rule["actions"]:
            self.assertEqual(18, action["actionType"])
            payloads.append(json.loads(action["actionValue"]))
        self.assertEqual([{"poll": [index], "ctrl": []} for index in range(1, 5)], payloads)
        tasks = self.docs["protocol.json"]["modbusRtu"]["master"]["tasks"]
        self.assertTrue(all(not task["enabled"] for task in tasks))
        self.assertEqual([5, 7, 4, 10], [task["startAddress"] for task in tasks[1:]])

    def test_canvas_cards_are_in_bounds_nonoverlapping_and_navigable(self):
        pages = self.docs["screen_panel.json"]["config"]["pages"]
        page_ids = {page["id"] for page in pages}
        for page in pages:
            prior = []
            for card in page["cards"]:
                rect = card["rect"]
                x, y, w, h = (rect[key] for key in ("x", "y", "w", "h"))
                self.assertTrue(x >= 0 and y >= 0 and w > 0 and h > 0)
                self.assertLessEqual(x + w, page["logicalWidth"])
                self.assertLessEqual(y + h, page["logicalHeight"])
                for px, py, pw, ph in prior:
                    self.assertFalse(x < px + pw and px < x + w and
                                     y < py + ph and py < y + h)
                prior.append((x, y, w, h))
                if card["kind"] == "page-link":
                    self.assertIn(card["pageId"], page_ids)
                    self.assertGreaterEqual(min(w, h), 44)

    def test_layout_has_no_unconfigured_weather_or_control_placeholders(self):
        config = self.docs["screen_panel.json"]["config"]
        pages = {page["id"]: page for page in config["pages"]}
        self.assertEqual(4, len(pages))
        self.assertIn("environment", pages)
        cards = [card for page in pages.values() for card in page["cards"]]
        self.assertLessEqual(len(cards), 24)
        self.assertEqual(len(cards), len({card["id"] for card in cards}))
        weather_cards = [card for card in cards if card["kind"] == "weather"]
        self.assertEqual(1, len(weather_cards))
        self.assertTrue(all(card.get("sourceRef") == "weather.home_weather" for card in weather_cards))
        controls = [card for card in pages["environment"]["cards"] if card["kind"] == "control"]
        self.assertEqual({"action"}, {card["visualType"] for card in controls})
        self.assertTrue(all(card.get("controlRef") for card in controls))
        collector_button = next(card for card in controls if card.get("controlRef") == "rule.collect_sht30")
        self.assertEqual("rule.collect_sht30", collector_button["controlRef"])
        self.assertRegex(collector_button["bindingFingerprint"], r"^[0-9a-f]{8}$")
        allowed_rules = [rule for rule in self.docs["periph_exec.json"]["rules"] if rule["allowPanelControl"]]
        self.assertEqual(["collect_sht30"], [rule["id"] for rule in allowed_rules])
        self.assertTrue(all(action["actionType"] == 19 and action["targetPeriphId"] == "sht30"
                            for action in allowed_rules[0]["actions"]))
        system_actions = [card["systemActionId"] for card in pages["system"]["cards"]
                          if card["kind"] == "system"]
        self.assertEqual(["brightness", "sleep-time", "touch-beep", "buzzer", "restart", "sleep"], system_actions)
        brightness = [card["actionValue"] for card in pages["system"]["cards"]
                      if card.get("systemActionId") == "brightness"]
        self.assertEqual([70], brightness)
        self.assertEqual("slider", pages["system"]["cards"][0]["visualType"])
        self.assertEqual("select", pages["system"]["cards"][1]["visualType"])
        self.assertTrue(config["settings"]["allowSystemRestart"])
        restart = next(card for card in pages["system"]["cards"] if card.get("systemActionId") == "restart")
        self.assertTrue(restart["requireConfirmation"])
        self.assertNotIn("confirm", restart)
        home = pages[config["settings"]["homePageId"]]
        self.assertFalse(any(card["kind"] == "page-link" for card in home["cards"]))
        self.assertTrue(any(card["kind"] == "weather" for card in home["cards"]))

    def test_preserved_motor_panel_does_not_enable_scheduled_motion(self):
        pages = self.docs["screen_panel.json"]["config"]["pages"]
        self.assertTrue(any(page["id"] == "motor" for page in pages))
        self.assertFalse(any(page["id"] == "particles" for page in pages))
        cards = [card for page in pages for card in page["cards"]]
        self.assertFalse(any(card.get("sourceRef", "").startswith("modbus.pm") for card in cards))
        master = self.docs["protocol.json"]["modbusRtu"]["master"]
        device = master["devices"][0]
        self.assertEqual((8, 1, [0, 1, 2, 5, 7]),
                         (device["slaveAddress"], device["controlProtocol"], device["motorRegs"]))
        self.assertEqual((0, 0), (device["motorMinPosition"], device["motorMaxPosition"]))
        self.assertEqual((3, 6, 5, 7), tuple(device["motorProtocol"][key] for key in
                         ("readFunction", "writeFunction", "speedReadRegister", "pulseReadRegister")))
        self.assertTrue(all(not task["enabled"] for task in master["tasks"]))
        reads = master["tasks"][1:]
        self.assertEqual([5, 7, 4, 10], [task["startAddress"] for task in reads])
        self.assertTrue(all((task["slaveAddress"], task["functionCode"], task["quantity"]) == (8, 3, 1)
                            for task in reads))
        rules = {rule["id"]: rule for rule in self.docs["periph_exec.json"]["rules"]}
        collector = rules["collect_motor"]
        self.assertFalse(collector["enabled"])
        self.assertEqual([{"poll": [index], "ctrl": []} for index in (1, 2, 3, 4)],
                         [json.loads(action["actionValue"]) for action in collector["actions"]])
        for name, value in (("motor_forward", "0:1"), ("motor_reverse", "1:1"), ("motor_stop", "2:1")):
            rule = rules[name]
            self.assertFalse(rule["enabled"])
            self.assertFalse(rule["allowPanelControl"])
            self.assertEqual(0, rule["triggers"][0]["triggerType"])
            self.assertEqual(name + "_request", rule["triggers"][0]["triggerPeriphId"])
            self.assertEqual("1", rule["triggers"][0]["compareValue"])
            action = rule["actions"][0]
            self.assertEqual(("modbus:0", 17, value, False),
                             (action["targetPeriphId"], action["actionType"], action["actionValue"], action["useReceivedValue"]))
        writes = [action for rule in rules.values() for action in rule["actions"] if action["actionType"] == 17]
        self.assertEqual({"0:1", "1:1", "2:1"}, {action["actionValue"] for action in writes})

    def test_peripheral_names_fit_production_limits(self):
        for peripheral in self.docs["peripherals.json"]["peripherals"]:
            self.assertLessEqual(len(peripheral["name"].encode("utf-8")), 48,
                                 peripheral["id"])

    def test_saved_collector_binding_matches_production_fingerprint(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("Requires g++")
        source = (ROOT / "src/core/ScreenPanelManager.cpp").read_text(encoding="utf-8")
        production = braced_statement(source, "String ScreenPanelManager::ruleFingerprint(")
        rule = next(rule for rule in self.docs["periph_exec.json"]["rules"] if rule["id"] == "collect_sht30")
        card = next(card for page in self.docs["screen_panel.json"]["config"]["pages"]
                    for card in page["cards"] if card.get("controlRef") == "rule.collect_sht30")
        actions = ",\n".join("{%d, %s, %s, %s, %d}" % (
            action["actionType"], json.dumps(action["targetPeriphId"]),
            json.dumps(action["actionValue"]), str(action["useReceivedValue"]).lower(),
            action["syncDelayMs"]) for action in rule["actions"])
        harness = r'''
#include <Arduino.h>
#include <cassert>
struct ExecAction { int actionType; String targetPeriphId, actionValue; bool useReceivedValue; int syncDelayMs; };
struct PeriphExecRule { String id; bool allowPanelControl; std::vector<ExecAction> actions; };
struct ScreenPanelManager { static String ruleFingerprint(const PeriphExecRule&); };
PRODUCTION
int main() {
  PeriphExecRule rule{RULE_ID, true, {ACTIONS}};
  assert(ScreenPanelManager::ruleFingerprint(rule) == EXPECTED);
  rule.actions[0].actionValue += " ";
  assert(ScreenPanelManager::ruleFingerprint(rule) != EXPECTED);
}
'''.replace("PRODUCTION", production).replace("RULE_ID", json.dumps(rule["id"])) \
   .replace("ACTIONS", actions).replace("EXPECTED", json.dumps(card["bindingFingerprint"]))
        with tempfile.TemporaryDirectory(prefix="panel-bundle-binding-") as directory:
            cpp = Path(directory) / "binding.cpp"
            executable = Path(directory) / ("binding.exe" if os.name == "nt" else "binding")
            cpp.write_text(harness, encoding="utf-8")
            built = subprocess.run([compiler, "-std=c++17", "-I" + str(ROOT / "test/mocks"),
                                    str(cpp), "-o", str(executable)], capture_output=True,
                                   text=True, timeout=60)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(0, ran.returncode, ran.stdout + ran.stderr)

    def test_home_screen_stays_visible_without_sensor_readings(self):
        config = self.docs["screen_panel.json"]["config"]
        self.assertEqual(52, config["settings"]["brightnessPercent"])
        self.assertEqual(0, config["settings"]["idleSleepSeconds"])
        home = next(page for page in config["pages"]
                    if page["id"] == config["settings"]["homePageId"])
        self.assertTrue(any(card["kind"] == "clock" for card in home["cards"]))
        refs = {card.get("sourceRef", "") for card in home["cards"]}
        self.assertTrue(any(card["kind"] == "weather" for card in home["cards"]))
        self.assertFalse(any(card["kind"] == "control" and
                             not card.get("sourceRef", card.get("controlRef")) for card in home["cards"]))
        self.assertTrue({"sht30_temperature", "sht30_humidity"}.issubset(refs))


if __name__ == "__main__":
    unittest.main()
