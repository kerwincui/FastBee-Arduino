"""Compile real route parsers/serializers; only HTTP transport is substituted."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
ROUTE = (ROOT / "src/network/handlers/PeripheralRouteHandler.cpp").read_text(encoding="utf-8")
PERSISTENCE = (ROOT / "src/core/PeripheralManagerPersistence.cpp").read_text(encoding="utf-8")


def braced(source, opening):
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[opening:token.end()]
    raise AssertionError("Unterminated source block")


def definition(name):
    match = re.search(r"(?:bool|void)\s+" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", ROUTE)
    if not match:
        raise AssertionError("Missing production definition: " + name)
    opening = ROUTE.index("{", match.start())
    return ROUTE[match.start():opening] + braced(ROUTE, opening)


def body(name):
    start = ROUTE.index(name + "(")
    return braced(ROUTE, ROUTE.index("{", start))


class GenericPeripheralRouteContractTests(unittest.TestCase):
    def test_real_route_parser_serialization_and_persistence(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "C++17 compiler required for route parser behavior checks")
        json_headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        self.assertTrue(json_headers, "Project ArduinoJson dependency required")
        definitions = [definition(name) for name in (
            "isDigitalInputPeripheral", "isAnalogInputPeripheral", "isAnalogInputConfigType",
            "hasPeripheralRequestField", "readIoRequestInteger",
            "applyIoPeripheralRequestParameters", "addDisplayParams", "addGenericPeripheralFields")]
        p1 = body("applyP1PeripheralParams")
        sensor = p1.index("else if (config.type == PeripheralType::SENSOR_GENERIC)")
        definitions.append("bool applySensorRequest(WebHandlerContext* ctx, AsyncWebServerRequest* request, "
                           "PeripheralConfig& config, bool resetDefaults, String& errorMsg) " +
                           braced(p1, p1.index("{", sensor))[:-1] + "return true;\n}")
        start = PERSISTENCE.index("if (config.isGPIOPeripheral() && config.isExpanderChannel())")
        io_save = braced(PERSISTENCE, PERSISTENCE.index("{", start))
        gpio_start = PERSISTENCE.index("else if (config.isGPIOPeripheral() && !isAdcInputType(config.type))")
        gpio_save = braced(PERSISTENCE, PERSISTENCE.index("{", gpio_start))
        definitions.append("void saveIoPersistence(JsonObject params, const PeripheralConfig& config) { " +
                           io_save + gpio_save + " }")
        assignments = []
        for field in ("ioSource", "channel", "initialState", "invertLogic"):
            assignments.append(re.search(r"config\.params\.gpio\." + field + r"\s*=.*?;", PERSISTENCE).group())
        assignments.append(re.search(r"strlcpy\(config\.params\.gpio\.providerId,[\s\S]*?;", PERSISTENCE).group())
        definitions.append("void loadIoPersistence(JsonObject params, PeripheralConfig& config) { " +
                           "\n".join(assignments) + " }")
        with tempfile.TemporaryDirectory(prefix="fastbee-route-") as directory:
            generated = Path(directory) / "route_under_test.inc"
            generated.write_text("\n\n".join(definitions), encoding="utf-8")
            executable = Path(directory) / ("route.exe" if os.name == "nt" else "route")
            command = [compiler, "-std=c++17", "-pthread", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                       "-DARDUINOJSON_ENABLE_STD_STRING=1", "-Wall", "-Wextra",
                       f"-I{ROOT / 'test/unit/cpp/sensor_mocks'}", f"-I{ROOT / 'test/mocks'}",
                       f"-I{ROOT / 'include'}", f"-I{json_headers[0].parent}", f"-I{directory}",
                       str(ROOT / "test/unit/cpp/generic_peripheral_route_harness.cpp"),
                       str(ROOT / "src/core/SensorConfigSupport.cpp"),
                       str(ROOT / "src/peripherals/drivers/INA226Driver.cpp"),
                       str(ROOT / "src/peripherals/drivers/MPU6050Driver.cpp"),
                       "-o", str(executable)]
            built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=15)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("generic peripheral route behavior checks passed", ran.stdout)

    def test_all_mutations_use_the_checked_parser(self):
        self.assertIn("applyIoPeripheralRequestParameters", body("applyPeripheralRequestParameters"))
        for name in ("buildPreflightCandidate", "PeripheralRouteHandler::handleAddPeripheral",
                     "PeripheralRouteHandler::handleUpdatePeripheral"):
            self.assertIn("applyPeripheralRequestParameters", body(name))
        self.assertIn("buildPreflightCandidate", body("PeripheralRouteHandler::handlePreflightPeripheral"))

    def test_detail_list_and_compact_include_real_binding_fields(self):
        listing = body("PeripheralRouteHandler::handleGetPeripherals")
        self.assertIn("addGenericPeripheralFields(doc.as<JsonObject>(), config)", listing)
        compact = listing.index("if (!compact)")
        compact_body = braced(listing, listing.index("{", compact))
        self.assertNotIn("addGenericPeripheralFields", compact_body)
        self.assertIn("addGenericPeripheralFields(itemDoc.as<JsonObject>(), config)", listing)
        self.assertIn("addGenericPeripheralFields(data, *config)", body("PeripheralRouteHandler::handleGetPeripheral"))


if __name__ == "__main__":
    unittest.main()
