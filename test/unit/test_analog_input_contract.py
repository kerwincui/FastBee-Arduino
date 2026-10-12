"""Regression checks for the AI/ADC configuration contract.

These checks are intentionally source-level: the native test target does not
provide ArduinoJson/AsyncWebServer, while the regressions are about keeping
the Web form, route handler and persistent manager branches in agreement.
"""

import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
STATE = (ROOT / "web-src/js/state.js").read_text(encoding="utf-8")
PERIPHERALS_JS = (ROOT / "web-src/modules/runtime/peripherals.js").read_text(encoding="utf-8")
PERIPHERAL_SAVE_JS = (ROOT / "web-src/modules/runtime/peripheral-save.js").read_text(encoding="utf-8")
ROUTE_CPP = "\n".join((ROOT / path).read_text(encoding="utf-8") for path in (
    "src/network/handlers/PeripheralRouteHandler.cpp",
    "src/network/handlers/PeripheralReadRoutes.cpp",
    "src/network/handlers/PeripheralRouteReadSupport.h",
))
MANAGER_CPP = "\n".join(
    (ROOT / path).read_text(encoding="utf-8")
    for path in (
        "src/core/PeripheralManager.cpp",
        "src/core/PeripheralManagerAnalog.cpp",
        "src/core/PeripheralManagerDiagnostics.cpp",
        "src/core/PeripheralManagerIo.cpp",
        "src/core/PeripheralManagerPersistence.cpp",
        "src/core/PeripheralManagerValidation.cpp",
    )
)
MANAGER_H = (ROOT / "include/core/PeripheralManager.h").read_text(encoding="utf-8")
CONFIG_H = (ROOT / "include/core/PeripheralConfig.h").read_text(encoding="utf-8")
TYPES_H = (ROOT / "include/core/PeripheralTypes.h").read_text(encoding="utf-8")
SCHEDULER_CPP = (ROOT / "src/core/PeriphExecScheduler.cpp").read_text(encoding="utf-8")
PROTOCOL_CPP = (ROOT / "src/protocols/ProtocolManager.cpp").read_text(encoding="utf-8")
EXECUTOR_CPP = (ROOT / "src/core/PeriphExecExecutor.cpp").read_text(encoding="utf-8")
EXEC_ROUTE_CPP = (ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp").read_text(encoding="utf-8")


class AnalogInputContractTests(unittest.TestCase):
    def test_device_menu_is_not_gated_by_ota_feature(self):
        match = re.search(r"_featureMenuMap\s*:\s*\{(.*?)\n\s*\},", STATE, re.S)
        self.assertIsNotNone(match, "feature menu map must remain discoverable")
        self.assertNotRegex(match.group(1), r"\bota\s*:")
        self.assertIn("deviceControl: 'device-control'", match.group(1))

    def test_legacy_gpio_analog_input_uses_adc_form(self):
        self.assertIn("type >= 11 && type <= 21 && type !== 15", PERIPHERALS_JS)
        self.assertIn("type === 26 || type === 15", PERIPHERALS_JS)
        self.assertIn("typeNum >= 11 && typeNum <= 21 && typeNum !== 15", PERIPHERAL_SAVE_JS)
        self.assertIn("typeNum === 26 || typeNum === 15", PERIPHERAL_SAVE_JS)

    def test_adc_filter_fields_round_trip_through_api(self):
        self.assertRegex(ROUTE_CPP, r"isAnalogInput(?:Config|Peripheral)Type\(config(?:->|\.)type\)")
        self.assertIn('FastBee::setFixedJsonField(params, "sampleCount",', ROUTE_CPP)
        self.assertIn('FastBee::setFixedJsonField(params, "filterMode",', ROUTE_CPP)
        self.assertIn('getParamInt(request, "sampleCount"', ROUTE_CPP)
        self.assertIn('getParamInt(request, "filterMode"', ROUTE_CPP)
        self.assertIn("isAdcInputType(config.type)", MANAGER_CPP)
        self.assertIn('params["sampleCount"]', MANAGER_CPP)
        self.assertIn('params["filterMode"]', MANAGER_CPP)

    def test_ai_conversion_fields_are_exposed_and_submitted(self):
        required_ids = (
            "adc-signal-mode",
            "adc-conversion-enabled",
            "adc-calibrated-voltage",
            "adc-clamp-enabled",
            "adc-input-min",
            "adc-input-max",
            "adc-engineering-min",
            "adc-engineering-max",
            "adc-engineering-offset",
            "adc-vref",
            "adc-voltage-ratio",
            "adc-shunt-resistance",
            "adc-input-gain",
            "adc-input-offset",
            "adc-raw-min",
            "adc-raw-max",
        )
        modal = (ROOT / "web-src/pages/modals.html").read_text(encoding="utf-8")
        for element_id in required_ids:
            self.assertIn('id="' + element_id + '"', modal)
        for field in (
            "signalMode",
            "conversionEnabled",
            "calibratedVoltage",
            "clampEnabled",
            "inputMin",
            "inputMax",
            "engineeringMin",
            "engineeringMax",
            "engineeringOffset",
            "vRef",
            "voltageRatio",
            "shuntResistance",
            "inputGain",
            "inputOffset",
            "rawMin",
            "rawMax",
        ):
            self.assertIn(field, PERIPHERALS_JS)
            self.assertIn('FastBee::setFixedJsonField(params, "' + field + '",', ROUTE_CPP)

        defaults_body = re.search(
            r"_applyAnalogSignalDefaults\(modeValue\)\s*\{(.*?)\n\s*\},",
            PERIPHERALS_JS,
            re.S,
        )
        self.assertIsNotNone(defaults_body)
        self.assertIn("conversionEl.checked = mode !== 0", defaults_body.group(1))

    def test_ai_conversion_uses_guarded_linear_mapping(self):
        """Keep the documented 0-10V/4-20mA formula aligned with firmware."""
        self.assertIn("VOLTAGE_0_10V", CONFIG_H)
        self.assertIn("CURRENT_4_20MA", CONFIG_H)
        self.assertIn("convertAnalogRaw", MANAGER_CPP)
        self.assertIn("convertAnalogInputRaw(config, raw, quality)", MANAGER_CPP)
        self.assertIn("adc.clampEnabled", CONFIG_H)
        self.assertIn("const float boundaryTolerance = inputLsb * 0.5f", CONFIG_H)
        self.assertIn("classifyAnalogInputQuality", CONFIG_H)
        self.assertIn("measuredQuality == AnalogInputQuality::VALID", CONFIG_H)
        self.assertIn("if (inputValue < inputMin) inputValue = inputMin", CONFIG_H)
        self.assertIn("if (!adc.clampEnabled) return NAN", CONFIG_H)
        self.assertIn("engineeringOffset", CONFIG_H)
        self.assertIn("voltageRatio", CONFIG_H)
        self.assertIn("shuntResistance", CONFIG_H)

    def test_ai_quality_is_diagnostic_and_reportable(self):
        for quality in (
            "VALID",
            "BELOW_RANGE",
            "ABOVE_RANGE",
            "DISCONNECTED",
            "SHORT_CIRCUIT",
            "INVALID_CONFIG",
        ):
            self.assertIn(quality, CONFIG_H)
        for label in ("valid", "low", "high", "disconnected", "short"):
            self.assertIn('return "' + label + '"', CONFIG_H)

        self.assertIn("classifyAnalogInputQuality", CONFIG_H)
        self.assertIn("inputMin - 1.0f", CONFIG_H)
        self.assertIn("inputMax + 1.0f", CONFIG_H)
        self.assertIn("AnalogInputQuality* quality", MANAGER_H)
        self.assertIn('FastBee::setFixedJsonField(data, "quality", analogInputQualityName(quality))', ROUTE_CPP)
        self.assertIn('FastBee::setFixedJsonField(data, "qualityCode", static_cast<uint8_t>(quality))', ROUTE_CPP)
        self.assertIn('json += "_quality', PROTOCOL_CPP)
        self.assertIn('remark\\\":\\\"quality', PROTOCOL_CPP)

    def test_di_options_round_trip_and_simple_defaults(self):
        modal = (ROOT / "web-src/pages/modals.html").read_text(encoding="utf-8")
        for element_id in (
            "gpio-input-mode",
            "gpio-invert-logic",
            "gpio-debounce-ms",
            "gpio-report-mode",
        ):
            self.assertIn('id="' + element_id + '"', modal)
        for field in ("inputMode", "invertLogic", "debounceMs", "reportMode"):
            self.assertIn(field, PERIPHERALS_JS)
            self.assertIn('FastBee::setFixedJsonField(params, "' + field + '",', ROUTE_CPP)
        self.assertRegex(CONFIG_H, r"enum class DigitalInputMode\s*:\s*uint8_t")
        self.assertRegex(CONFIG_H, r"\bAUTO\s*=\s*0")
        self.assertRegex(CONFIG_H, r"\bPULL_UP\s*=\s*2")
        self.assertRegex(CONFIG_H, r"enum class DigitalReportMode\s*:\s*uint8_t")
        self.assertRegex(CONFIG_H, r"\bON_CHANGE\s*=\s*0")
        self.assertIn("DigitalInputMode::AUTO", MANAGER_CPP)
        self.assertIn("DigitalReportMode", ROUTE_CPP + MANAGER_CPP)
        self.assertIn("debounceMs", MANAGER_CPP)

    def test_s3_f16r8_ai_bundle_enables_engineering_conversion(self):
        bundle = json.loads(
            (ROOT / "docs/device-test-configs/com15-esp32s3-f16r8/config/peripherals.json").read_text(encoding="utf-8")
        )
        peripherals = {item["id"]: item for item in bundle["peripherals"]}
        expected_ranges = {
            "ai_soil_temp": (-40, 80),
            "ai_soil_humidity": (0, 100),
        }

        for peripheral_id, engineering_range in expected_ranges.items():
            params = peripherals[peripheral_id]["params"]
            self.assertEqual(params["signalMode"], 3)
            self.assertTrue(params["conversionEnabled"])
            self.assertFalse(params["clampEnabled"])
            self.assertEqual((params["inputMin"], params["inputMax"]), (4, 20))
            self.assertEqual(
                (params["engineeringMin"], params["engineeringMax"]),
                engineering_range,
            )
            self.assertEqual(params["shuntResistance"], 150)
            self.assertTrue(params["calibratedVoltage"])
            self.assertEqual(params["inputGain"], 1)
            self.assertEqual(params["decimalPlaces"], 1)

        for peripheral_id in ("di1", "di2"):
            params = peripherals[peripheral_id]["params"]
            self.assertEqual(params["inputMode"], 2)
            self.assertFalse(params.get("invertLogic", False))
            self.assertEqual(params["debounceMs"], 50)
            self.assertEqual(params.get("reportMode", 0), 0)

        self.assertIn('config.params.gpio.invertLogic = params["invertLogic"] | false;', MANAGER_CPP)
        self.assertIn(
            'config.params.gpio.reportMode = params["reportMode"] | static_cast<uint8_t>(DigitalReportMode::ON_CHANGE);',
            MANAGER_CPP,
        )

        exec_bundle = json.loads(
            (ROOT / "docs/device-test-configs/com15-esp32s3-f16r8/config/periph_exec.json").read_text(encoding="utf-8")
        )
        collect_rule = next(rule for rule in exec_bundle["rules"] if rule["id"] == "s3_collect_inputs")
        ai_actions = [
            json.loads(action["actionValue"])
            for action in collect_rule["actions"]
            if action["targetPeriphId"] in expected_ranges
        ]
        self.assertEqual(len(ai_actions), 2)
        for action in ai_actions:
            self.assertEqual(action["decimalPlaces"], 1)
            self.assertEqual(action["unit"], "C" if action["periphId"] == "ai_soil_temp" else "%")
            self.assertEqual(action["sensorCategory"], "analog")
            self.assertNotIn("engineeringMin", action)
            self.assertNotIn("engineeringMax", action)
            self.assertNotIn("mapToEngineering", action)

    def test_s3_f16r8_sensor_report_ids_are_unique_and_channels_stay_correct(self):
        exec_bundle = json.loads(
            (ROOT / "docs/device-test-configs/com15-esp32s3-f16r8/config/periph_exec.json").read_text(encoding="utf-8")
        )
        collect_rule = next(rule for rule in exec_bundle["rules"] if rule["id"] == "s3_collect_inputs")
        self.assertNotIn("s3_optional_sht30", {rule["id"] for rule in exec_bundle["rules"]})
        self.assertEqual({a["targetPeriphId"] for a in collect_rule["actions"]},
                         {"ai_soil_temp", "ai_soil_humidity"})
        sensor_actions = [
            json.loads(action["actionValue"])
            for action in collect_rule["actions"]
            if action["actionType"] == 19
        ]

        report_ids = [action["dataField"] for action in sensor_actions]
        self.assertEqual(len(report_ids), len(set(report_ids)))
        self.assertEqual(
            set(report_ids),
            {"soil_temperature", "soil_humidity"},
        )

        self.assertIn('doc["channelField"]', EXECUTOR_CPP)
        self.assertIn('sDoc["channelField"]', EXEC_ROUTE_CPP)
        self.assertIn('String cacheKey = sensorPeriphId + "_" + cacheField', EXEC_ROUTE_CPP)

    def test_di_report_modes_have_distinct_runtime_paths(self):
        self.assertIn("shouldReportDigitalInputChange", CONFIG_H)
        self.assertIn("shouldReportDigitalInputPeriodically", CONFIG_H)
        self.assertIn("handleStableDigitalInputChange", SCHEDULER_CPP)
        self.assertIn("currentLevel != btnState.currentState", SCHEDULER_CPP)
        self.assertIn("now - btnState.lastChangeTime < debounceMs", SCHEDULER_CPP)
        self.assertIn("shouldReportDigitalInputChange(config.params.gpio.reportMode)", SCHEDULER_CPP)
        self.assertIn("!shouldReportDigitalInputPeriodically(reportMode)", PROTOCOL_CPP)
        self.assertIn("hasDataSourceRule(config.id)", SCHEDULER_CPP)

    def test_json_interrupt_inputs_auto_attach_with_configured_debounce(self):
        helper = re.search(
            r"inline bool isGpioInterruptInputType\(PeripheralType type\)\s*\{(.*?)\n\}",
            TYPES_H,
            re.S,
        )
        self.assertIsNotNone(helper)
        for type_name in (
            "GPIO_INTERRUPT_RISING",
            "GPIO_INTERRUPT_FALLING",
            "GPIO_INTERRUPT_CHANGE",
        ):
            self.assertIn(type_name, helper.group(1))

        self.assertRegex(
            MANAGER_CPP,
            r"(?s)isGpioInterruptInputType\(config\.type\).*?attachInterrupt\(config\.id, nullptr\)",
        )
        self.assertRegex(
            MANAGER_CPP,
            r"(?s)isGpioInterruptInputType\(config\.type\)\s*\?\s*effectiveDigitalInputDebounceMs\(config\.params\.gpio\.debounceMs\)",
        )
        self.assertIn("sizeof(GpioIsrEvent)", MANAGER_CPP)
        self.assertIn("gpio_get_level", MANAGER_CPP)
        self.assertIn("popDigitalInputInterruptEvent", MANAGER_H + MANAGER_CPP + SCHEDULER_CPP)
        self.assertIn("digitalInputLogicalLevel", MANAGER_CPP)
        self.assertIn("matchesDigitalInterruptEdge", MANAGER_CPP + SCHEDULER_CPP)
        self.assertRegex(
            MANAGER_CPP,
            r"(?s)GPIO_INTERRUPT_RISING.*?invertLogic\s*\?\s*FALLING\s*:\s*RISING",
        )
        self.assertRegex(
            MANAGER_CPP,
            r"(?s)GPIO_INTERRUPT_FALLING.*?invertLogic\s*\?\s*RISING\s*:\s*FALLING",
        )

    def test_s3_f16r8_modbus_templates_match_executor_indices(self):
        protocol = json.loads(
            (ROOT / "docs/device-test-configs/com15-esp32s3-f16r8/config/protocol.json").read_text(encoding="utf-8")
        )
        exec_bundle = json.loads(
            (ROOT / "docs/device-test-configs/com15-esp32s3-f16r8/config/periph_exec.json").read_text(encoding="utf-8")
        )
        master = protocol["modbusRtu"]["master"]
        rules = {rule["id"]: rule for rule in exec_bundle["rules"]}

        self.assertEqual(len(master["tasks"]), 5)
        # Keep the failing leak device isolated from the healthy batch so its
        # rule-level retry delay cannot stall the other four sensors.
        poll_rules = {"s3_modbus_water_leak": [0], "s3_modbus_environment": [1, 2, 3, 4]}
        for rule_id, task_indices in poll_rules.items():
            rule = rules[rule_id]
            action = rule["actions"][0]
            self.assertEqual(action["actionType"], 18)
            self.assertEqual(action["targetPeriphId"], f"modbus-task:{task_indices[0]}")
            self.assertEqual(json.loads(action["actionValue"]), {"poll": task_indices})
            self.assertEqual(rule["triggers"][0]["triggerType"], 5)

        self.assertEqual(master["tasks"][0]["functionCode"], 4)
        self.assertEqual(master["tasks"][4]["functionCode"], 4)
        self.assertEqual([m["regOffset"] for m in master["tasks"][4]["mappings"]], [0, 1])
        self.assertEqual(len(master["devices"]), 1)
        self.assertEqual(master["devices"][0]["slaveAddress"], 6)

        relay_index = next(
            index
            for index, device in enumerate(master["devices"])
            if device["deviceType"] == "relay"
        )
        for rule_id, command_value, action_name in (
            ("s3_modbus_relay", "1", "on"),
            ("s3_modbus_relay_off", "0", "off"),
        ):
            rule = rules[rule_id]
            trigger = rule["triggers"][0]
            action = rule["actions"][0]
            self.assertEqual(trigger["triggerType"], 0)
            self.assertEqual(trigger["triggerPeriphId"], f"modbus:{relay_index}")
            self.assertEqual(trigger["compareValue"], command_value)
            self.assertEqual(action["actionType"], 18)
            self.assertEqual(action["targetPeriphId"], f"modbus:{relay_index}")
            self.assertEqual(
                json.loads(action["actionValue"]),
                {"ctrl": [{"d": relay_index, "c": 0, "a": action_name}]},
            )


if __name__ == "__main__":
    unittest.main()
