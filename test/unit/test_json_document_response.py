import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class JsonDocumentResponseTests(unittest.TestCase):
    def test_real_json_document_move_lifetime_and_fragmented_reads(self):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        candidates = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        self.assertTrue(candidates, "ArduinoJson dependency required")
        with tempfile.TemporaryDirectory(prefix="fastbee-json-response-") as directory:
            temporary = pathlib.Path(directory)
            (temporary / "ESPAsyncWebServer.h").write_text('''#pragma once
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
''', encoding="utf-8")
            executable = temporary / "json-response.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                f"-I{temporary}", f"-I{ROOT / 'include'}", f"-I{candidates[0].parent}",
                str(ROOT / "test/unit/cpp/json_document_response_harness.cpp"), "-o", str(executable)],
                capture_output=True, text=True, timeout=60, env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20, env=environment)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
