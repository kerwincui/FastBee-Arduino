"""Actual peripheral serializers/response sections versus frozen old source.

Draft only until the coordinator integrates and runs it. No network or devices.
Auth, admission, mutations and hardware reads are outside these response tests.
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
ROOT = Path(os.environ['FASTBEE_TEST_ROOT']) if os.environ.get('FASTBEE_TEST_ROOT') else next(
    p for p in HERE.parents if (p / 'platformio.ini').is_file())
SUPPORT = Path(os.environ.get('FASTBEE_TEST_SUPPORT_ROOT', ROOT))
CPP = HERE if (HERE / 'peripheral_route_fixed_json_harness.cpp.in').is_file() else HERE / 'cpp'
TARGETS = r'params|object|pins|target|data|row|item|encoder|status|channel|payload'
SCALARS = re.compile(r'\b(' + TARGETS + r')\["([^"\n]+)"\]\s*=\s*(?!=)([^;]*);')
COUNTS = {
    'addDigitalInputParams': 6, 'addAnalogInputParams': 22, 'addServoParams': 8,
    'addUartParams': 5, 'addEncoderParams': 2, 'addGenericPeripheralFields': 13,
    'addP1PeripheralParams': 9, 'addDisplayParams': 49, 'addPeripheralDiagnosticCapability': 3,
    'buildMcpGroupPreview': 7, 'addResourceRows': 7, 'addPreflightIssues': 9,
    'addCandidateClaims': 9, 'addPreflightResponse': 7,
    'PeripheralRouteHandler::handleGetPeripheralResources': 16,
    'PeripheralRouteHandler::handleGetPeripherals': 5,
    'PeripheralRouteHandler::handleGetPeripheral': 41,
    'PeripheralRouteHandler::handleGetPeripheralStatus': 49,
    'PeripheralRouteHandler::handleWritePeripheral': 9,
}
HELPERS = (
    'isAnalogInputPeripheral', 'isDigitalInputPeripheral', 'addDigitalInputParams',
    'addAnalogInputParams', 'addServoParams', 'addUartParams', 'addEncoderParams',
    'addDisplayParams', 'addGenericPeripheralFields', 'addP1PeripheralParams',
    'addPeripheralDiagnosticCapability', 'buildMcpGroupPreview',
    'preflightOperationName', 'pinRoleName', 'claimStateName', 'shareModeName',
    'sameResourceIdentity', 'countUniqueClaims', 'countProfilePins', 'resourceCapacity',
    'resourceLabel', 'addResourceRows', 'requiredAcknowledgements',
    'addPreflightIssues', 'addCandidateClaims', 'resolvedPinRole',
    'addResolvedPins', 'addPreflightResponse')


def definition(source, name):
    pattern = re.compile(r'(?:inline\s+)?(?:bool|void|size_t|String|uint32_t|const char\*)\s+' +
                         re.escape(name) + r'\s*\([^;{]*\)\s*\{')
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise AssertionError('Expected exactly one definition: ' + name)
    start = matches[0].start()
    opening = matches[0].end() - 1
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    for token in tokens.finditer(source, opening):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise AssertionError('Unterminated ' + name)


def scalar_draft(source, expected):
    transformed, count = SCALARS.subn(lambda m:
        f'FastBee::setFixedJsonField({m[1]}, "{m[2]}", {m[3]});', source)
    if count != expected:
        raise AssertionError(f'Reviewed scalar count changed: {count} != {expected}')
    return transformed


def ownership_reference(source):
    """Correct only four known array lifetimes in a separate old-body reference.

    The frozen oracle file remains original and its actual helper also builds
    as Legacy for the borrowing/NUL-length/dependency-reuse negative control.
    """
    fields = {
        'providerId': 'config.params.gpio.providerId',
        'driver': 'config.params.ioExpander.driver',
        'busPeripheralId': 'config.params.ioExpander.busPeripheralId',
        'resetPeripheralId': 'config.params.ioExpander.resetPeripheralId',
    }
    for key, value in fields.items():
        original = f'params["{key}"] = {value};'
        if source.count(original) != 1:
            raise AssertionError('Expected one original ownership site: ' + key)
        source = source.replace(original, f'params["{key}"] = JsonString({value}, false);')
    return source


def copied_text_reference(source, fields):
    """Explicit ownership-only corrections; original bodies stay independent."""
    for key, value in fields.items():
        original = f'params["{key}"] = {value};'
        if source.count(original) != 1:
            raise AssertionError('Expected one original array site: ' + key)
        source = source.replace(original, f'params["{key}"] = JsonString({value}, false);')
    return source


def between(source, start, end):
    if source.count(start) != 1:
        raise AssertionError('Expected one response boundary: ' + start)
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def reviewed_addition(source, old, new):
    """Adapt explicit later features without recapturing the old oracle."""
    if source.count(old) != 1:
        raise AssertionError('Reviewed feature insertion anchor drifted: ' + old)
    return source.replace(old, new, 1)


def rtc_seed_reference(source):
    anchor = '        params["trickleCharge"] = config.params.ds1302.trickleCharge;'
    return reviewed_addition(source, anchor, anchor + '\n'
        '        FastBee::serializeRtcBootSeedConfig(params, config.params.ds1302.bootSeed,\n'
        '                                           config.params.ds1302.utcOffsetMinutes);')


def sensor_scan_reference(source):
    anchor = '        params["mux"] = sensor.mux;'
    return reviewed_addition(source, anchor, anchor + '\n        params["scanMask"] = sensor.scanMask;')


def sensor_status_reference(source):
    source = reviewed_addition(source,
        'metadata->getChannel(i, config.params.sensor.mode)',
        'metadata->getChannel(i, config.params.sensor.mode, config.params.sensor.scanMask)')
    anchor = '                channel["valid"] = !stale;'
    return reviewed_addition(source, anchor, anchor + '\n'
        '                if (sample.channelMetadata) {\n'
        '                    channel["timestamp"] = sample.channelTimestamps[i];\n'
        '                    channel["quality"] = !stale ? "good" : "scan_invalid";\n'
        '                    channel["error"] = sensorChannelErrorName(sample.channelErrors[i]);\n'
        '                }')


def case_match_draft(source):
    """Review only the twenty literal bridge sites; retain the frozen oracle."""
    old = 'ctx->getParamValue(request, "action", "").equalsIgnoreCase("release")'
    new = 'FastBee::stringEqualsIgnoreCaseLiteral(ctx->getParamValue(request, "action", ""), "release")'
    if source.count(old) != 1:
        raise AssertionError('Reviewed nested release matcher anchor drifted')
    source = source.replace(old, new, 1)
    literals = (
        'mount', 'recover', 'unmount', 'write', 'write-file', 'delete', 'delete-file',
        'setDateTime', 'set-datetime', 'setRunning', 'set-running', 'print', 'clear',
        'backlight', 'readBlock', 'read-block', 'writeBlock', 'write-block', 'reset')
    if len(literals) != 19:
        raise AssertionError('Reviewed action matcher count changed')
    for literal in literals:
        old = 'action.equalsIgnoreCase("' + literal + '")'
        new = 'FastBee::stringEqualsIgnoreCaseLiteral(action, "' + literal + '")'
        if source.count(old) != 1:
            raise AssertionError('Reviewed action matcher anchor drifted: ' + literal)
        source = source.replace(old, new, 1)
    return source


def sections(functions, suffix):
    result = ['namespace ' + suffix + ' {']
    result += [functions[name] for name in HELPERS]
    detail = functions['PeripheralRouteHandler::handleGetPeripheral']
    detail = detail[detail.index('    JsonDocument doc;'):detail.rfind('}')]
    result += ['void detailResponse(FakeRequest* request, const PeripheralConfig* config, '
               'const String& id, PeripheralManager& pm) {\n' + detail + '}']
    status = functions['PeripheralRouteHandler::handleGetPeripheralStatus']
    status = status[status.index('    JsonDocument doc;'):status.rfind('}')]
    result += ['void statusResponse(FakeRequest* request, const PeripheralConfig& config, '
               'const String& id, bool hasRuntimeState, bool hasEncoderReading, '
               'const PeripheralRuntimeState& runtimeState, const EncoderReading& encoderReading, '
               'PeripheralManager& pm) {\n' + status + '}']
    resource = functions['PeripheralRouteHandler::handleGetPeripheralResources']
    header = between(resource, '    JsonDocument doc;', '    {\n        MutexGuard resultLock')
    # Keep the real header including null placeholders; inject only the already
    # decided observed branch, leaving planner/lock admission out of this test.
    observed = between(resource, '                data["configurationStatus"] = "observed";',
                       '            }\n        }') if 'data["configurationStatus"] = "observed";' in resource else between(
                           resource, '                FastBee::setFixedJsonField(data, "configurationStatus", "observed");',
                           '            }\n        }')
    result += ['void resourceResponse(FakeRequest* request, const FastBee::BoardProfile* profile, '
               'const FastBee::BoardProfile* active, const FastBee::PreflightResult& preflightResult, '
               'bool observed) {\n' + header + '\nif (observed) {\n' + observed +
               '\n}\nHandlerUtils::sendJsonStream(request, doc);\n}']
    listing = functions['PeripheralRouteHandler::handleGetPeripherals']
    encoder = between(listing, '                    JsonObject encoder = itemDoc["reading"].to<JsonObject>();',
                      '                }\n            } else')
    result += ['void encoderResponse(JsonDocument& itemDoc, const PeripheralRuntimeState& runtimeState) {\n' + encoder + '}']
    write = functions['PeripheralRouteHandler::handleWritePeripheral']
    pca = between(write, '        response["success"] = success;', '        return;\n    }')
    epoch = re.search(r'(?:data\["configEpoch"\] = writeConfigVersion;|'
                      r'FastBee::setFixedJsonField\(data, "configEpoch", writeConfigVersion\);)', write)
    if epoch is None:
        raise AssertionError('PCA epoch response field missing')
    result += ['void pcaResponse(FakeRequest* request, const String& id, bool success, '
               'const String& error, uint64_t writeConfigVersion, bool withEpoch, PeripheralManager& pm) {\n'
               'JsonDocument response; JsonObject data = response["data"].to<JsonObject>();\n'
               'if (withEpoch) { ' + epoch.group() + ' }\n' + pca + '}']
    rfid = between(write, '            JsonDocument response;\n            response["success"] = true;',
                   '            return;')
    result += ['void rfidResponse(FakeRequest* request, const String& id, int blockValue, '
               'uint8_t (&blockData)[16], RFIDDriver& reader) {\n' + rfid + '}']
    result += ['}']
    return '\n\n'.join(result)


def local_includes(seeds, roots):
    pending, seen = list(seeds), set()
    while pending:
        path = pending.pop().resolve()
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        for name in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', path.read_text(encoding='utf-8'), re.M):
            for base in (path.parent, *roots):
                if (base / name).is_file():
                    pending.append(base / name)
                    break
    return seen


class PeripheralRouteFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        if not Path(compiler).is_file():
            raise RuntimeError('C++17 compiler required')
        library = Path(os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE',
                                      SUPPORT / '.pio/libdeps/native/ArduinoJson/src'))
        if not (library / 'ArduinoJson.h').is_file():
            raise RuntimeError('Registered ArduinoJson headers required')
        source_path = ROOT / 'src/network/handlers/PeripheralRouteHandler.cpp'
        oracle_path = CPP / 'peripheral_route_fixed_json_oracle.inc'
        harness_path = CPP / 'peripheral_route_fixed_json_harness.cpp.in'
        ownership_path = CPP / 'peripheral_mcp_ownership_regression.inc'
        deferred_path = CPP / 'peripheral_deferred_string_regression.inc'
        support_oracle_path = CPP / 'peripheral_deferred_string_oracle.inc'
        source, oracle = source_path.read_text(encoding='utf-8'), oracle_path.read_text(encoding='utf-8')
        if hashlib.sha256(oracle_path.read_bytes()).hexdigest() != 'b1e2e7c99a38cc43d24c31be33a30ae06cfda56a03b8ee88f0f93c419aaca9a3':
            raise AssertionError('Original peripheral route oracle changed')
        if hashlib.sha256(support_oracle_path.read_bytes()).hexdigest() != '886900b1c5d176efa7d26b794668097ea180d97546cde35785d7c7e292d02792':
            raise AssertionError('Original peripheral support oracle changed')
        names = tuple(dict.fromkeys((*HELPERS, *COUNTS)))
        frozen = {name: definition(oracle, name) for name in names}
        old = dict(frozen)
        old['addP1PeripheralParams'] = rtc_seed_reference(frozen['addP1PeripheralParams'])
        old['PeripheralRouteHandler::handleGetPeripheralStatus'] = sensor_status_reference(
            frozen['PeripheralRouteHandler::handleGetPeripheralStatus'])
        old['addGenericPeripheralFields'] = ownership_reference(frozen['addGenericPeripheralFields'])
        old['addDisplayParams'] = copied_text_reference(frozen['addDisplayParams'], {
            key: 'display.' + key for key in
            ('backend', 'driver', 'touchBackend', 'rgbInitProfile', 'controlBackend')})
        draft = {name: scalar_draft(body, COUNTS.get(name, 0)) for name, body in old.items()}
        write_name = 'PeripheralRouteHandler::handleWritePeripheral'
        draft[write_name] = case_match_draft(draft[write_name])
        from test_peripheral_resident_shared import shared_listing
        draft['PeripheralRouteHandler::handleGetPeripherals'] = shared_listing(
            draft['PeripheralRouteHandler::handleGetPeripherals'])
        resource_name = 'PeripheralRouteHandler::handleGetPeripheralResources'
        resource_anchor = '                const std::vector<PeripheralConfig> existing = manager.getAllPeripherals();'
        if draft[resource_name].count(resource_anchor) != 1:
            raise AssertionError('Resource workspace admission anchor drifted')
        # Keep the frozen wire oracle; only the independently exercised request
        # workspace admission changes before the existing descriptor builders.
        draft[resource_name] = draft[resource_name].replace(resource_anchor,
            '                std::unique_ptr<FastBee::PreflightResult> workspace;\n'
            '                if (!allocatePreflightWorkspace(ctx, request, workspace)) return;\n'
            '                FastBee::PreflightResult& preflightResult = *workspace;\n' + resource_anchor)
        current = {name: definition(source, name) for name in names}
        for name in names:
            if current[name] != draft[name]:
                raise AssertionError('Production body must contain the reviewed scalar rewrite exactly: ' + name)
        if sum(COUNTS.values()) != 276:
            raise AssertionError('Selected field accounting changed')
        support_paths = [ROOT / p for p in ('src/core/SensorConfigSupport.cpp',
            'src/core/Pca9685ConfigSupport.cpp', 'src/core/PeripheralResourcePlanner.cpp',
            'src/network/handlers/PeripheralRouteReadSupport.h')]
        sensor, pca, planner, read = [p.read_text(encoding='utf-8') for p in support_paths]
        original_support = support_oracle_path.read_text(encoding='utf-8')
        support_reference = {
            'serializeSensorConfig': sensor_scan_reference(copied_text_reference(
                definition(original_support, 'serializeSensorConfig'),
                {'driver': 'sensor.driver', 'busPeripheralId': 'sensor.busPeripheralId'})),
            'serializePca9685Config': copied_text_reference(
                definition(original_support, 'serializePca9685Config'),
                {'busPeripheralId': 'p.busPeripheralId', 'oePeripheralId': 'p.oePeripheralId'}),
        }
        for actual, name in ((sensor, 'serializeSensorConfig'), (pca, 'serializePca9685Config')):
            if definition(actual, name) != support_reference[name]:
                raise AssertionError('Support body must equal frozen body plus explicit ownership fixes: ' + name)
        support = ['namespace FastBee {', definition(sensor, 'serializeSensorConfig'),
                   definition(pca, 'serializePca9685Config')]
        support += [definition(planner, name) for name in
                    ('preflightIssueCodeName', 'preflightSeverityName', 'resourceKindName')]
        support += ['}', definition(source, 'sameText'), definition(source, 'bytesToHex'),
                    'template <typename TJson>\n' + definition(source, 'addPassiveBuzzerFlag')]
        support += [definition(read, name) for name in ('addUartRuntime', 'addEncoderReading', 'isAnalogInputConfigType')]
        for namespace, source_bodies in (
                ('LegacySupport', [definition(original_support, name) for name in support_reference]),
                ('ReferenceSupport', list(support_reference.values()))):
            support += ['namespace ' + namespace + ' { using namespace ::FastBee;']
            support += source_bodies
            support += ['}']
        differential_reference = {name: body.replace(
            'FastBee::serializeSensorConfig', 'ReferenceSupport::serializeSensorConfig').replace(
            'FastBee::serializePca9685Config', 'ReferenceSupport::serializePca9685Config')
            for name, body in old.items()}
        program = harness_path.read_text(encoding='utf-8')
        production = '\n\n'.join(sections(fs, name) for fs, name in
                                ((current, 'Current'), (differential_reference, 'Oracle'), (draft, 'Draft')))
        production += ('\nnamespace Legacy {\n'
                       'using Oracle::isDigitalInputPeripheral;\n' +
                       frozen['addDisplayParams'] + '\n' +
                       frozen['addGenericPeripheralFields'] + '\n}\n')
        for marker, value in (('@SUPPORT_METHODS@', '\n\n'.join(support)),
                              ('@PRODUCTION_SERIALIZERS@', production),
                              ('@OWNERSHIP_REGRESSIONS@', ownership_path.read_text(encoding='utf-8')),
                              ('@DEFERRED_REGRESSIONS@', deferred_path.read_text(encoding='utf-8'))):
            if program.count(marker) != 1:
                raise AssertionError('Expected one marker ' + marker)
            program = program.replace(marker, value)
        mock = SUPPORT / 'test/mocks/Arduino.h'
        helper = ROOT / 'src/network/FixedJsonField.cpp'
        shared_serializer = ROOT / 'src/utils/SharedJsonSerializer.cpp'
        roots = (ROOT / 'include', SUPPORT / 'include', SUPPORT / 'test/mocks', library)
        seeds = (ROOT / 'include/core/PeripheralConfig.h', ROOT / 'include/core/PeripheralResourcePlanner.h',
                 ROOT / 'include/core/RtcBootSeedConfig.h',
                 ROOT / 'include/core/SensorDriverMetadata.h', ROOT / 'include/core/interfaces/ISensorDriver.h',
                 ROOT / 'include/network/FixedJsonField.h',
                 ROOT / 'include/network/JsonDocumentResponse.h', mock, helper, shared_serializer)
        cls.inputs = sorted(local_includes(seeds, roots) | {
            Path(__file__), source_path, oracle_path, harness_path, ownership_path,
            deferred_path, support_oracle_path, *support_paths})
        cls.inputs += sorted(library.rglob('*.h')) + sorted(library.rglob('*.hpp'))
        cls.hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix='fastbee-peripheral-route-json-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.addClassCleanup(cls.verify_inputs)
        temp = Path(cls.temporary.name)
        arduino = mock.read_text(encoding='utf-8')
        marker = 'void clear() { _value.clear(); }'
        if arduino.count(marker) != 1:
            raise AssertionError('Arduino String fixture changed')
        arduino = arduino.replace(marker, marker +
            '\n    bool concat(const char* p) { _value += p ? p : ""; return true; }', 1)
        (temp / 'Arduino.h').write_text(arduino, encoding='utf-8')
        (temp / 'prefix.h').write_text('#include <Arduino.h>\n#undef malloc\n#undef free\n', encoding='utf-8')
        (temp / 'ESPAsyncWebServer.h').write_text('''#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
class AsyncAbstractResponse {
public:
 virtual ~AsyncAbstractResponse()=default;
 virtual bool _sourceValid() const {return false;}
 virtual size_t _fillBuffer(uint8_t*,size_t)=0;
 int _code=0; std::string _contentType;
 size_t _contentLength=0,_sentLength=0; bool _sendContentLength=false;
};
''', encoding='utf-8')
        cpp = temp / 'peripheral-route-json.cpp'
        cpp.write_text(program, encoding='utf-8')
        cls.environment = dict(os.environ)
        cls.environment['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.environment.get('PATH', '')
        cls.binaries = []
        for enabled in (0, 1):
            binary = temp / ('peripheral-route-json-' + str(enabled) + '.exe')
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DUNIT_TEST=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1',
                '-DARDUINOJSON_ENABLE_STD_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
                '-DFIXTURE_HARDWARE_FEATURES=' + str(enabled),
                '-DFASTBEE_ENABLE_DS1302=' + str(enabled), '-DFASTBEE_ENABLE_LCD1602=' + str(enabled),
                '-DFASTBEE_ENABLE_RFID=' + str(enabled),
                '-I' + str(temp), '-I' + str(ROOT / 'include'), '-I' + str(SUPPORT / 'test/mocks'),
                '-I' + str(library), '-include', str(temp / 'prefix.h'),
                str(cpp), str(helper), str(shared_serializer), '-o', str(binary)]
            built = subprocess.run(command, env=cls.environment, capture_output=True, text=True,
                                   encoding='utf-8', errors='replace', timeout=180)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.binaries.append(binary)

    @classmethod
    def verify_inputs(cls):
        if cls.hashes != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}:
            raise AssertionError('Serializer inputs changed during the test')

    def run_mode(self, mode):
        for binary in self.binaries:
            result = subprocess.run([str(binary), mode], env=self.environment, capture_output=True,
                                    text=True, encoding='utf-8', errors='replace', timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_real_config_helpers_and_detail_response(self):
        self.run_mode('config')

    def test_real_resource_preflight_and_group_response(self):
        self.run_mode('resources')

    def test_real_runtime_status_and_command_response_sections(self):
        self.run_mode('status')

    def test_null_reused_and_input_string_ownership(self):
        self.run_mode('ownership')

    def test_mcp_local_snapshot_strings_are_copied(self):
        self.run_mode('mcp-ownership')

    def test_actual_deferred_response_outlives_display_sensor_and_pca_snapshots(self):
        self.run_mode('deferred-ownership')


if __name__ == '__main__':
    unittest.main()
