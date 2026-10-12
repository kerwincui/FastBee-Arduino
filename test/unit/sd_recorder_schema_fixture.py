"""Read the real firmware SD recorder catalog, without a device or hand-coded schema."""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def sd_recorder_schema_json():
    compiler = shutil.which('g++') or shutil.which('clang++')
    headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
    if not compiler or not headers:
        raise RuntimeError('C++17 compiler and installed ArduinoJson required')
    with tempfile.TemporaryDirectory(prefix='fastbee-sd-recorder-schema-') as directory:
        executable = Path(directory) / 'schema.exe'
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-declarations', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
            '-DARDUINOJSON_ENABLE_STD_STRING=1', '-I' + str(ROOT / 'test/mocks'),
            '-I' + str(ROOT / 'include'), '-I' + str(headers[0].parent),
            str(ROOT / 'test/unit/cpp/sd_recorder_schema_harness.cpp'),
            str(ROOT / 'src/network/FixedJsonField.cpp'), '-o', str(executable)],
            capture_output=True, text=True, encoding='utf-8', timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        result = subprocess.run([str(executable)], capture_output=True,
                                text=True, encoding='utf-8', timeout=10)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        json.loads(result.stdout)
        return result.stdout
