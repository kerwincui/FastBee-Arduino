"""Complete list route versus frozen original; catalog graph is checked separately."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_peripheral_route_fixed_json import definition, local_includes

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'test/unit/cpp'
MANIFEST = CPP / 'peripheral_resident_shared_manifest.json'


def manifest():
    if hashlib.sha256(MANIFEST.read_bytes()).hexdigest() != '40cd1972629c22b566e2083d41d4fb0dd3fb08dc18700587335d476be7699124':
        raise AssertionError('Frozen sharing manifest changed')
    return json.loads(MANIFEST.read_text(encoding='utf-8'))


def replace_once(body, before, after):
    if body.count(before) != 1:
        raise AssertionError('Shared-list source anchor drifted: ' + before[:100])
    return body.replace(before, after, 1)


def shared_listing(original):
    frozen = manifest()
    result = original
    for object_name in ('doc', 'itemDoc'):
        block = frozen['identity'].replace('doc[', object_name + '[')
        block = '\n'.join('        ' + line for line in block.splitlines())
        result = replace_once(result, block,
            '        addPeripheralListIdentity(' + object_name + ', config);')
    for key, indent, object_name in (('paramBlock', 8, 'doc'), ('secondParams', 12, 'itemDoc')):
        block = '\n'.join(' ' * indent + line for line in frozen[key].splitlines())
        result = replace_once(result, block,
            ' ' * indent + 'addPeripheralListParams(' + object_name + ', config);')
    return result


def shared_catalog(original):
    result = original
    for name, target in (('addRoles', 'roles'), ('addOperations', 'operations')):
        result = replace_once(result,
            '    auto ' + name + ' = [&' + target + '](std::initializer_list<const char*> values) {\n'
            '        for (const char* value : values) ' + target + '.add(value);\n    };',
            '    auto ' + name + ' = [&' + target + '](std::initializer_list<const char*> values) {\n'
            '        appendCatalogCopiedValues(' + target + ', values);\n    };')
    return result


def frozen_graph():
    data = manifest()
    paths = (CPP / 'peripheral_list_shared_oracle.inc', CPP / 'peripheral_catalog_shared_oracle.inc')
    for path, expected in zip(paths, (data['listingSha256'], data['catalogOracleSha256'])):
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise AssertionError('Frozen shared-route oracle changed: ' + str(path))
    return tuple(path.read_text(encoding='utf-8') for path in paths)


def checked_catalog(source):
    _, oracle = frozen_graph()
    names = ('addSensorDriverDescriptors', 'addIoExpanderDriverDescriptors',
             'addScreenDisplayDescriptor', 'addPeripheralTypeDescriptorDetails',
             'PeripheralRouteHandler::handleGetPeripheralTypes')
    # Descriptor ownership stays frozen; the response implementation has its
    # own complete wire, lifetime, OOM and bounded-memory regression.
    for name in names[:-1]:
        expected = definition(oracle, name)
        if name == 'addPeripheralTypeDescriptorDetails':
            expected = shared_catalog(expected)
        if definition(source, name) != expected:
            raise AssertionError('Catalog changed outside reviewed sharing: ' + name)
    helper = 'appendCatalogCopiedValues'
    if definition(source, helper) != definition(manifest()['addedHelpers']['copied'], helper):
        raise AssertionError('Catalog copied-string writer drifted')
    return 'namespace CatalogOracle {\n' + '\n\n'.join(
        definition(oracle, name) for name in names[:-1]) + '\n}\n'


class PeripheralResidentSharedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        if not Path(compiler).is_file():
            raise RuntimeError('C++17 compiler required')
        library = Path(os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE',
                                      ROOT / '.pio/libdeps/native/ArduinoJson/src'))
        if not (library / 'ArduinoJson.h').is_file():
            raise RuntimeError('Real ArduinoJson headers required')
        source_path = ROOT / 'src/network/handlers/PeripheralRouteHandler.cpp'
        source = source_path.read_text(encoding='utf-8')
        original, _ = frozen_graph()
        original = original.rstrip('\n')
        current = definition(source, 'PeripheralRouteHandler::handleGetPeripherals')
        expected = shared_listing(original)
        if current != expected:
            raise AssertionError('Complete list route differs outside reviewed sharing')
        data = manifest()
        for key, name in (('identity', 'addPeripheralListIdentity'), ('params', 'addPeripheralListParams')):
            if definition(source, name) != definition(data['addedHelpers'][key], name):
                raise AssertionError('List helper drifted: ' + name)
        helper_names = ('sameText', 'isAnalogInputPeripheral', 'isDigitalInputPeripheral',
            'addDigitalInputParams', 'addAnalogInputParams', 'addServoParams', 'addUartParams',
            'addEncoderParams', 'addDisplayParams', 'addGenericPeripheralFields',
            'addP1PeripheralParams', 'addPeripheralDiagnosticCapability',
            'addPeripheralListIdentity', 'addPeripheralListParams')
        helpers = '\n\n'.join(definition(source, name) for name in helper_names)
        helpers += '\ntemplate <typename TJson>\n' + definition(source, 'addPassiveBuzzerFlag')
        read_path = ROOT / 'src/network/handlers/PeripheralRouteReadSupport.h'
        read = read_path.read_text(encoding='utf-8')
        helpers += '\n' + '\n'.join(definition(read, name) for name in ('addUartRuntime', 'addEncoderReading'))
        sensor_path = ROOT / 'src/core/SensorConfigSupport.cpp'
        pca_path = ROOT / 'src/core/Pca9685ConfigSupport.cpp'
        serializers = definition(sensor_path.read_text(encoding='utf-8'), 'serializeSensorConfig')
        serializers += '\n' + definition(pca_path.read_text(encoding='utf-8'), 'serializePca9685Config')
        methods = ''
        for name, body in (('ListOracle', original), ('ListCurrent', current), ('ListDraft', expected)):
            marker = 'size_t freeHeap = ESP.getFreeHeap();'
            if body.count(marker) != 1:
                raise AssertionError('List byte-count observation seam drifted')
            body = body.replace(marker, 'sharedEffect("items-size:" + std::to_string(totalItemsSize));\n    ' + marker)
            methods += 'struct ' + name + ' { ListContext* ctx; void handleGetPeripherals(AsyncWebServerRequest*); };\n'
            methods += body.replace('PeripheralRouteHandler::', name + '::', 1) + '\n'
        harness_path = CPP / 'peripheral_list_shared_harness.cpp.in'
        program = harness_path.read_text(encoding='utf-8')
        for key, value in (('@CONFIG_SERIALIZERS@', serializers), ('@LIST_HELPERS@', helpers), ('@LIST_METHODS@', methods)):
            if program.count(key) != 1:
                raise AssertionError('Harness marker drifted: ' + key)
            program = program.replace(key, value)
        seeds = [CPP / 'peripheral_shared_probe.h', ROOT / 'include/core/PeripheralConfig.h',
                 ROOT / 'include/core/SensorDriverMetadata.h', ROOT / 'include/core/RtcBootSeedConfig.h',
                 ROOT / 'src/network/FixedJsonField.cpp', ROOT / 'src/utils/SharedJsonSerializer.cpp']
        cls.inputs = sorted(local_includes(seeds, [CPP, ROOT / 'include', ROOT / 'test/mocks', library]) | {
            Path(__file__), MANIFEST, harness_path, source_path, sensor_path, pca_path, read_path,
            CPP / 'peripheral_list_shared_oracle.inc', CPP / 'peripheral_catalog_shared_oracle.inc'})
        cls.inputs += sorted(library.rglob('*.h')) + sorted(library.rglob('*.hpp'))
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temporary = tempfile.TemporaryDirectory(prefix='fastbee-peripheral-shared-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.addClassCleanup(cls.verify_inputs)
        temp = Path(cls.temporary.name)
        cpp = temp / 'list.cpp'
        cpp.write_text(program, encoding='utf-8')
        cls.binary = temp / 'list.exe'
        cls.environment = dict(os.environ)
        cls.environment['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.environment.get('PATH', '')
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DUNIT_TEST=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0', '-I' + str(CPP), '-I' + str(ROOT / 'include'),
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(library), str(cpp),
            str(ROOT / 'src/network/FixedJsonField.cpp'), str(ROOT / 'src/utils/SharedJsonSerializer.cpp'),
            '-o', str(cls.binary)]
        built = subprocess.run(command, env=cls.environment, capture_output=True, text=True,
                               encoding='utf-8', errors='replace', timeout=180)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def verify_inputs(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('List differential inputs changed')

    def check_mode(self, mode):
        result = subprocess.run([str(self.binary), mode], env=self.environment, capture_output=True,
            text=True, encoding='utf-8', errors='replace', timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_complete_list_routes(self):
        self.check_mode('values')

    def test_each_list_json_allocation(self):
        self.check_mode('oom')


if __name__ == '__main__':
    unittest.main()
