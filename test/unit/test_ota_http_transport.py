"""Exercise production URL OTA code with observable HTTP/TLS clients."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OtaHttpTransportTests(unittest.TestCase):
    def test_production_download_uses_one_client_and_preserves_failure_gates(self):
        source = (ROOT / "src/network/OTAManager.cpp").read_text(encoding="utf-8")
        start = source.index("bool OTAManager::startOTA(")
        end = source.index("bool OTAManager::startOTAAsync(", start)
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        self.assertTrue(Path(compiler).exists(), "C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="fastbee-ota-http-") as directory:
            temporary = Path(directory)
            (temporary / "production_ota_download.inc").write_text(source[start:end], encoding="utf-8")
            executable = temporary / "ota-http.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                 "-DUSE_NATIVE_STDLIB=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                 "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
                 f"-I{temporary}", f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                 f"-I{ROOT / '.pio/libdeps/native/ArduinoJson/src'}",
                 str(ROOT / "test/unit/cpp/ota_http_transport_harness.cpp"), "-o", str(executable)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15, env=environment)
            self.assertEqual(0, run.returncode, run.stdout + run.stderr)

    def test_installed_http_client_rejects_cross_scheme_redirects(self):
        # Production uses strict redirects on the supplied client. Verify the
        # installed dependency cannot silently downgrade HTTPS to plain HTTP.
        framework = Path.home() / ".platformio/packages/framework-arduinoespressif32"
        source = (framework / "libraries/HTTPClient/src/HTTPClient.cpp").read_text(encoding="utf-8")
        body = source[source.index("bool HTTPClient::setURL("):]
        self.assertIn('if (!url.startsWith(_protocol +', body)
        guard = body[:body.index("// check if the port is specified")]
        self.assertIn("return false;", guard)


if __name__ == "__main__":
    unittest.main()
