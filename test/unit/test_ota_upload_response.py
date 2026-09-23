"""OTA completion must preserve responses queued before multipart finishes."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_screen_panel_contract import braced_block

ROOT = pathlib.Path(__file__).resolve().parents[2]


class OtaUploadResponseTests(unittest.TestCase):
    def test_real_upload_completion_with_deferred_library_send(self):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        self.assertTrue(pathlib.Path(compiler).exists(), "C++ compiler required")
        candidates = list((ROOT / ".pio/libdeps").glob("*/ESPAsyncWebServer/src/WebRequest.cpp"))
        self.assertTrue(candidates, "Installed ESPAsyncWebServer source required")
        library = candidates[0].read_text(encoding="utf-8")
        route = (ROOT / "src/network/handlers/OTARouteHandler.cpp").read_text(encoding="utf-8")
        send_signature = "void AsyncWebServerRequest::send(AsyncWebServerResponse *response)"
        flush_signature = "void AsyncWebServerRequest::_send()"
        completion = braced_block(route, 'server->on("/api/ota/upload", HTTP_POST,')
        with tempfile.TemporaryDirectory(prefix="fastbee-ota-response-") as directory:
            temporary = pathlib.Path(directory)
            (temporary / "production_ota_completion.inc").write_text(completion, encoding="utf-8")
            (temporary / "production_library_send.inc").write_text(
                send_signature + " {" + braced_block(library, send_signature) + "}\n" +
                flush_signature + " {" + braced_block(library, flush_signature) + "}\n",
                encoding="utf-8",
            )
            executable = temporary / "ota-response.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 f"-I{temporary}", str(ROOT / "test/unit/cpp/ota_upload_response_harness.cpp"),
                 "-o", str(executable)],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=60, env=environment,
            )
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True,
                                 timeout=10, env=environment)
            self.assertEqual(0, run.returncode, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
