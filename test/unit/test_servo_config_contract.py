"""Source contracts for the dedicated PWM servo configuration path."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONFIG_H = (ROOT / "include/core/PeripheralConfig.h").read_text(encoding="utf-8")
MANAGER_H = (ROOT / "include/core/PeripheralManager.h").read_text(encoding="utf-8")
MANAGER_CPP = "\n".join(
    (ROOT / path).read_text(encoding="utf-8")
    for path in (
        "src/core/PeripheralManager.cpp",
        "src/core/PeripheralManagerServoControl.cpp",
        "src/core/PeripheralManagerPersistence.cpp",
        "src/core/PeripheralManagerValidation.cpp",
    )
)
ROUTE_CPP = "\n".join((ROOT / path).read_text(encoding="utf-8") for path in (
    "src/network/handlers/PeripheralRouteHandler.cpp",
    "src/network/handlers/PeripheralReadRoutes.cpp",
))


SERVO_FIELDS = (
    "frequency",
    "minPulse",
    "maxPulse",
    "minAngle",
    "maxAngle",
    "initialAngle",
    "inverted",
    "releaseAfterMove",
)


def function_body(source: str, signature: str) -> str:
    """Return a C++ function body using brace matching."""
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


def named_struct_bodies(source: str, member_name: str):
    """Return bodies for anonymous structs assigned to a named union member."""
    bodies = []
    for match in re.finditer(rf"\}}\s*{re.escape(member_name)}\s*;", source):
        depth = 1
        for index in range(match.start() - 1, -1, -1):
            if source[index] == "}":
                depth += 1
            elif source[index] == "{":
                depth -= 1
                if depth == 0:
                    prefix = source[max(0, index - 16) : index]
                    if re.search(r"struct\s*$", prefix):
                        bodies.append(source[index + 1 : match.start()])
                    break
    return bodies


class ServoConfigContractTests(unittest.TestCase):
    def test_servo_has_dedicated_config_and_runtime_state(self):
        servo_blocks = named_struct_bodies(CONFIG_H, "servo")
        config_blocks = [body for body in servo_blocks if "frequency" in body]
        runtime_blocks = [body for body in servo_blocks if "currentAngle" in body]

        self.assertEqual(
            len(config_blocks),
            1,
            "PeripheralConfig must have one dedicated params.servo member",
        )
        self.assertEqual(
            len(runtime_blocks),
            1,
            "PeripheralRuntimeState must expose a dedicated state.servo member",
        )
        for field in SERVO_FIELDS:
            self.assertRegex(config_blocks[0], rf"\b{field}\b")
        self.assertIn("currentAngle", runtime_blocks[0])
        self.assertIn("currentPulseUs", runtime_blocks[0])

        pwm_blocks = named_struct_bodies(CONFIG_H, "pwm")
        self.assertEqual(len(pwm_blocks), 1, "generic PWM configuration must remain present")
        self.assertNotIn(
            "initialAngle",
            pwm_blocks[0],
            "servo settings must not be overlaid on generic PWM settings",
        )

    def test_servo_defaults_and_validation_are_centralized(self):
        defaults = function_body(CONFIG_H, r"\binitializeServoDefaults\s*\(")
        expected_defaults = {
            "frequency": ("SERVO_DEFAULT_FREQUENCY_HZ", "50"),
            "minPulse": ("SERVO_DEFAULT_MIN_PULSE_US", "500"),
            "maxPulse": ("SERVO_DEFAULT_MAX_PULSE_US", "2500"),
            "minAngle": ("SERVO_DEFAULT_MIN_ANGLE", "0"),
            "maxAngle": ("SERVO_DEFAULT_MAX_ANGLE", "180"),
            "initialAngle": ("SERVO_DEFAULT_INITIAL_ANGLE", "90"),
            "inverted": "false",
            "releaseAfterMove": "false",
        }
        for field, value in expected_defaults.items():
            assignment = value[0] if isinstance(value, tuple) else value
            self.assertRegex(
                defaults,
                rf"params\.servo\.{field}\s*=\s*{assignment}\b",
                f"safe default assignment for servo {field} is missing",
            )
            if isinstance(value, tuple):
                self.assertRegex(
                    CONFIG_H,
                    rf"constexpr\s+[^;=]+\b{value[0]}\s*=\s*{value[1]}\s*;",
                    f"safe default for servo {field} must remain {value[1]}",
                )

        validator_match = re.search(
            r"\b(?:isValidServoConfig|isServoConfigValid|validateServoConfig)\s*\(",
            CONFIG_H,
        )
        self.assertIsNotNone(
            validator_match, "servo range validation must live with its data model"
        )
        validator_name = re.search(
            r"(?:isValidServoConfig|isServoConfigValid|validateServoConfig)",
            validator_match.group(0),
        ).group(0)
        validator = function_body(CONFIG_H, rf"\b{validator_name}\s*\(")
        for field in ("frequency", "minPulse", "maxPulse", "minAngle", "maxAngle"):
            self.assertIn(field, validator)
        self.assertRegex(validator, r"minPulse\s*>=\s*(?:servo\.)?maxPulse")
        self.assertRegex(validator, r"minAngle\s*>=\s*(?:servo\.)?maxAngle")
        self.assertIn("initialAngle", validator)

        route_parser = function_body(ROUTE_CPP, r"\bapplyServoParams\s*\(")
        for field in ("frequency", "minPulse", "maxPulse", "minAngle", "maxAngle"):
            self.assertIn(field, route_parser)
        self.assertIn("INVALID_SERVO_CONFIG", route_parser)

    def test_angle_is_mapped_to_pulse_then_ledc_duty(self):
        pulse_mapper = function_body(CONFIG_H, r"\bservoAngleToPulseUs\s*\(")
        for token in (
            "minAngle",
            "maxAngle",
            "minPulse",
            "maxPulse",
            "inverted",
        ):
            self.assertIn(token, pulse_mapper)
        self.assertRegex(pulse_mapper, r"angle\s*<\s*minAngle")
        self.assertRegex(pulse_mapper, r"angle\s*>\s*maxAngle")
        self.assertIn("pulseSpan", pulse_mapper)
        self.assertIn("angleSpan", pulse_mapper)

        duty_match = re.search(
            r"\b(servoPulse(?:Us)?ToDuty)\s*\(", CONFIG_H
        )
        self.assertIsNotNone(duty_match, "servo pulse-to-duty helper is required")
        duty_mapper = function_body(CONFIG_H, rf"\b{duty_match.group(1)}\s*\(")
        self.assertIn("1000000", duty_mapper)
        self.assertRegex(duty_mapper, r"\bpulse(?:Us)?\b")
        self.assertRegex(duty_mapper, r"\bfrequency\b")
        self.assertRegex(duty_mapper, r"\bresolution\b")

        writer = function_body(
            MANAGER_CPP, r"\bPeripheralManager::submitOrdinaryServo\s*\("
        )
        self.assertIn("servoAngleToPulseUs", writer)
        self.assertIn(duty_match.group(1), writer)
        self.assertIn("ledcWrite", writer)
        self.assertIn("currentAngle", writer)
        self.assertIn("currentPulseUs", writer)
        self.assertIn("releaseAfterMove", writer)
        self.assertRegex(
            MANAGER_H,
            r"bool\s+writeServoAngle\s*\(\s*const\s+String&\s*\w+\s*,",
        )

    def test_servo_parameters_round_trip_through_persistence(self):
        save = function_body(
            MANAGER_CPP, r"\bPeripheralManager::saveConfiguration\s*\("
        )
        load = function_body(
            MANAGER_CPP, r"\bPeripheralManager::loadConfiguration\s*\("
        )
        self.assertIn("serializePersistedPeripheral(obj, config)", save)
        self.assertIn("parsePersistedPeripheral(obj, config, parseError)", load)
        serializer = function_body(MANAGER_CPP, r"\bserializePersistedPeripheral\s*\(")
        parser = function_body(MANAGER_CPP, r"\bparsePersistedPeripheral\s*\(")
        self.assertIn("PeripheralType::PWM_SERVO", serializer)
        self.assertIn("PeripheralType::PWM_SERVO", parser)
        self.assertIn("initializeServoDefaults", parser)
        for field in SERVO_FIELDS:
            self.assertIn(f'FastBee::setFixedJsonField(params, "{field}",', serializer)
            self.assertIn(f'params["{field}"]', parser)
            self.assertIn(f"config.params.servo.{field}", serializer)
            self.assertIn(f"config.params.servo.{field}", parser)

    def test_add_and_update_routes_round_trip_all_servo_fields(self):
        serializer = function_body(ROUTE_CPP, r"\baddServoParams\s*\(")
        parser = function_body(ROUTE_CPP, r"\bapplyServoParams\s*\(")
        for field in SERVO_FIELDS:
            self.assertIn(f'FastBee::setFixedJsonField(params, "{field}",', serializer)
            self.assertIn(f'"{field}"', parser)
            self.assertIn(f"config.params.servo.{field}", serializer)

        add = function_body(
            ROUTE_CPP, r"\bPeripheralRouteHandler::handleAddPeripheral\s*\("
        )
        update = function_body(
            ROUTE_CPP, r"\bPeripheralRouteHandler::handleUpdatePeripheral\s*\("
        )
        detail = function_body(
            ROUTE_CPP, r"\bPeripheralRouteHandler::handleGetPeripheral\s*\("
        )
        self.assertRegex(add, r"applyServoParams\s*\([^;]*\btrue\b")
        self.assertRegex(update, r"applyServoParams\s*\([^;]*\bfalse\b")
        self.assertIn("addServoParams", detail)
        self.assertIn("addPeripheralAndSave", add)
        self.assertIn("updatePeripheralAndSave", update)

    def test_read_write_and_status_routes_expose_servo_angle(self):
        setup = function_body(
            ROUTE_CPP, r"\bPeripheralRouteHandler::setupRoutes\s*\("
        )
        for endpoint in (
            "/api/peripherals/status",
            "/api/peripherals/read",
            "/api/peripherals/write",
        ):
            self.assertIn(endpoint, setup)

        status = function_body(
            ROUTE_CPP, r"\bPeripheralRouteHandler::handleGetPeripheralStatus\s*\("
        )
        read = function_body(
            ROUTE_CPP, r"\bPeripheralRouteHandler::handleReadPeripheral\s*\("
        )
        write = function_body(
            ROUTE_CPP, r"\bPeripheralRouteHandler::handleWritePeripheral\s*\("
        )
        for body in (status, read):
            self.assertIn("PeripheralType::PWM_SERVO", body)
        self.assertIn('FastBee::setFixedJsonField(data, "angle",', status)
        self.assertIn('FastBee::setFixedJsonField(data, "pulseUs",', status)
        self.assertIn('FastBee::setFixedJsonField(data, "angle", runtimeState.state.servo.currentAngle)', read)
        self.assertIn('FastBee::setFixedJsonField(data, "pulseUs", runtimeState.state.servo.currentPulseUs)', read)
        self.assertIn('FastBee::setFixedJsonField(data, "value", runtimeState.state.servo.currentAngle)', read)
        self.assertIn('FastBee::setFixedJsonField(data, "unit", "deg")', read)
        self.assertIn("PeripheralType::PWM_SERVO", write)
        self.assertIn("handleHttpServoOutput", write)
        boundary = (ROOT / "src/network/HttpOutputControl.cpp").read_text(encoding="utf-8")
        servo = function_body(boundary, r"\bhandleHttpServoOutput\s*\(")
        self.assertIn('hasParam("angle", true)', servo)
        self.assertIn("submitOrdinaryServo", servo)

    def test_new_peripherals_and_servos_are_disabled_by_default(self):
        add = function_body(
            ROUTE_CPP, r"\bPeripheralRouteHandler::handleAddPeripheral\s*\("
        )
        self.assertRegex(
            add,
            r'getParamBool\s*\(\s*request\s*,\s*"enabled"\s*,\s*false\s*\)',
        )
        servo_branch = re.search(
            r"config\.type\s*==\s*PeripheralType::PWM_SERVO(?P<body>.*?)"
            r"(?:else\s+if|\n\s*if\s*\(config\.id\.isEmpty)",
            add,
            re.S,
        )
        self.assertIsNotNone(servo_branch)
        self.assertRegex(servo_branch.group("body"), r"config\.enabled\s*=\s*false")


if __name__ == "__main__":
    unittest.main()
