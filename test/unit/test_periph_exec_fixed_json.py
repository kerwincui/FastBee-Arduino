"""Real route serializers versus frozen originals, including allocation faults.

The preflight fixture extracts the unchanged response prefix/lambda and suffix;
validation decisions are supplied explicitly, not claimed as route admission tests.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ["FASTBEE_TEST_ROOT"]) if os.environ.get("FASTBEE_TEST_ROOT") else next(
    parent for parent in HERE.parents if (parent / "platformio.ini").is_file())
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))
SIGNATURES = ("void writeRuleCandidate(",
              "void PeriphExecRouteHandler::handlePreflightRuleJson(",
              "String PeriphExecRouteHandler::serializeRuleFull(")


def definition(source, signature):
    if source.count(signature) != 1:
        raise AssertionError("Expected one definition: " + signature)
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise AssertionError("Unterminated " + signature)


def scalar_draft(source, expected):
    pattern = re.compile(r'^(\s*)(target|item|data|issue|limits|tObj|aObj)\["([^"\n]+)"\] = ([^\n]+);$', re.M)
    transformed, count = pattern.subn(lambda m:
        f'{m[1]}FastBee::setFixedJsonField({m[2]}, "{m[3]}", {m[4]});', source)
    if count != expected:
        raise AssertionError(f"Reviewed scalar count changed: {count} != {expected}")
    return transformed


def shared_route_draft(source):
    # A quoted semicolon is part of the RHS, not its statement terminator.
    source = re.sub(r'\b(ruleDoc|response|result|doc)\["([^"\n]+)"\][ \t]*=\s*'
        r'((?:"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[^;"\'])+);',
        lambda match: 'FastBee::setFixedJsonField(' + match[1] + ', "' + match[2] +
                      '", ' + match[3] + ');', source)
    for target, variable in (("item", "trigger"), ("tObj", "t")):
        pattern = (r'        FastBee::setFixedJsonField\(' + target + r', "triggerType", ' +
            variable + r'\.triggerType\);[\s\S]*?        FastBee::setFixedJsonField\(' +
            target + r', "pollInterPollDelay", ' + variable + r'\.pollInterPollDelay\);')
        source = re.sub(pattern, "        writeRuleTriggerFields(" + target + ", " + variable + ");", source)
    for target, variable in (("item", "action"), ("aObj", "a")):
        pattern = (r'        FastBee::setFixedJsonField\(' + target + r', "targetPeriphId", ' +
            variable + r'\.targetPeriphId\);[\s\S]*?        FastBee::setFixedJsonField\(' +
            target + r', "execMode", ' + variable + r'\.execMode\);')
        source = re.sub(pattern, "        writeRuleActionFields(" + target + ", " + variable + ");", source)
    return source


def definitions_for(functions, suffix):
    candidate, preflight, full = functions
    candidate = candidate.replace("void writeRuleCandidate(", "void writeRuleCandidate" + suffix + "(", 1)
    full = full.replace("String PeriphExecRouteHandler::serializeRuleFull(",
                        "String serializeRuleFull" + suffix + "(", 1)
    # Preserve every changed response assignment and the actual addIssue lambda.
    # Auth/parsing/validation branches are outside this serialization fixture.
    prefix_start = preflight.index("    JsonDocument response;")
    prefix_end = preflight.index('    if (operation == "add" &&', prefix_start)
    tail_start = preflight.index("    const bool allowed = errorCount == 0;")
    prefix = preflight[prefix_start:prefix_end]
    tail = preflight[tail_start:preflight.rfind("}")]
    if prefix.count("auto addIssue =") != 1 or tail.count("HandlerUtils::sendJsonStream(request, response);") != 1:
        raise AssertionError("Preflight response extraction changed")
    tail = tail.replace("writeRuleCandidate(", "writeRuleCandidate" + suffix + "(")
    fixture = ("void preflightResponse" + suffix + "(FakeRequest* request, const String& operation, "
               "const PeriphExecRule& current, const PeriphExecRule& candidate, "
               "const std::vector<Issue>& suppliedIssues, uint8_t suppliedRisk) {\n" + prefix +
               "    for (const auto& supplied : suppliedIssues)\n"
               "        addIssue(supplied.code.c_str(), supplied.severity.c_str(), "
               "supplied.path, supplied.message, supplied.risk);\n"
               "    if (suppliedRisk > risk) risk = suppliedRisk;\n" + tail + "}\n")
    return "\n\n".join((candidate, full, fixture))


def local_includes(seeds, roots):
    pending, seen = list(seeds), set()
    while pending:
        path = pending.pop().resolve()
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        for name in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', path.read_text(encoding="utf-8"), re.M):
            for base in (path.parent, *roots):
                child = base / name
                if child.is_file():
                    pending.append(child)
                    break
    return seen


class PeriphExecFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered ArduinoJson headers required")
        source_path = ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp"
        oracle_path = HERE / "cpp/periph_exec_fixed_json_oracle.inc"
        harness_path = HERE / "cpp/periph_exec_fixed_json_harness.cpp.in"
        source = source_path.read_text(encoding="utf-8")
        oracle = oracle_path.read_text(encoding="utf-8")
        old = [definition(oracle, signature) for signature in SIGNATURES]
        draft = [shared_route_draft(scalar_draft(function, count)) for function, count in zip(old, (26, 13, 22))]
        current = [definition(source, signature) for signature in SIGNATURES]
        for actual, baseline, proposed in zip(current, old, draft):
            if actual not in (baseline, proposed):
                raise AssertionError("Production body drifted beyond the reviewed scalar/shared writers")
        program = harness_path.read_text(encoding="utf-8")
        if program.count("@PRODUCTION_SERIALIZERS@") != 1:
            raise AssertionError("Expected one production marker")
        shared = "\n\n".join(definition(source, signature) for signature in (
            "__attribute__((noinline)) void writeRuleTriggerFields(",
            "__attribute__((noinline)) void writeRuleActionFields("))
        program = program.replace("@PRODUCTION_SERIALIZERS@", shared + "\n\n" + "\n\n".join(
            definitions_for(functions, suffix) for functions, suffix in
            ((current, "Current"), (old, "Oracle"), (draft, "Draft"))))
        mock = SUPPORT / "test/mocks/Arduino.h"
        helper = ROOT / "src/network/FixedJsonField.cpp"
        roots = (ROOT / "include", SUPPORT / "include", SUPPORT / "test/mocks", library)
        seeds = (ROOT / "include/core/PeripheralExecution.h", ROOT / "include/core/PeripheralConfig.h",
                 ROOT / "include/core/ResourceProfile.h", ROOT / "include/network/FixedJsonField.h", mock, helper)
        cls.inputs = sorted(local_includes(seeds, roots) | {Path(__file__), source_path, oracle_path, harness_path})
        cls.inputs += sorted(library.rglob("*.h")) + sorted(library.rglob("*.hpp"))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-periphexec-json-")
        cls.addClassCleanup(cls.temporary.cleanup)
        temp = Path(cls.temporary.name)
        arduino = mock.read_text(encoding="utf-8")
        marker = "void clear() { _value.clear(); }"
        if arduino.count(marker) != 1:
            raise AssertionError("Arduino String writer fixture changed")
        arduino = arduino.replace(marker, marker +
            '\n    bool concat(const char* p) { _value += p ? p : ""; return true; }', 1)
        (temp / "Arduino.h").write_text(arduino, encoding="utf-8")
        cpp = temp / "periphexec-json.cpp"
        cpp.write_text(program, encoding="utf-8")
        cls.binary = temp / ("periphexec-json.exe" if os.name == "nt" else "periphexec-json")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUNIT_TEST",
                   "-DCONFIG_IDF_TARGET_ESP32S3=1", "-DFASTBEE_TEST_NEEDS_STRLCPY=1",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_STD_STRING=1", "-I" + str(temp),
                   *["-I" + str(path) for path in roots], str(cpp), str(helper), "-o", str(cls.binary)]
        built = subprocess.run(command, text=True, capture_output=True, encoding="utf-8",
                               errors="replace", timeout=120, env=cls.environment)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.hashes:
            raise AssertionError("PeriphExec JSON verification inputs changed during the run")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], text=True, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode, result.stdout)


for _mode in ("candidate-values", "candidate-oom", "full-values", "full-oom", "preflight-values", "preflight-oom"):
    setattr(PeriphExecFixedJsonTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
