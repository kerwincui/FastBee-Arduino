"""Run the production manager scheduling/status paths with a controlled clock.

No deployment capture is test input; networking is an explicit mock boundary.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_snapshot_cache import definition

ROOT = Path(__file__).resolve().parents[2]

class RemoteWeatherSchedulerTests(unittest.TestCase):
    def test_production_manager_admission_recovery_status_cache_and_generation(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists(): compiler = str(fallback)
        if not compiler: raise RuntimeError("C++17 compiler required")
        source = (ROOT / "src/protocols/RemoteResourceManager.cpp").read_text(encoding="utf-8")
        names = ["bool weatherNetworkConnected()", "class Guard", "void RemoteResourceManager::applyConfig(", "bool RemoteResourceManager::getValue(",
                 "bool RemoteResourceManager::requestRead(", "bool RemoteResourceManager::getWeatherTestStatus(",
                 "void RemoteResourceManager::tick()", "void RemoteResourceManager::weatherTask(",
                 "void RemoteResourceManager::collectWeather()"]
        definitions = [definition(source, name) + (";" if name == "class Guard" else "") for name in names]
        with tempfile.TemporaryDirectory(prefix="fastbee-weather-manager-") as folder:
            temp = Path(folder)
            (temp / "manager.inc").write_text("\n".join(definitions), encoding="utf-8")
            binary = temp / ("weather.exe" if os.name == "nt" else "weather")
            headers = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
            env = dict(os.environ)
            env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
            result = subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror",
                "-Wno-unused-but-set-variable", "-DUNIT_TEST", "-DFASTBEE_ENABLE_PERIPH_EXEC=0",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                *[f"-I{p}" for p in (temp, ROOT/"test/mocks", headers.parent, ROOT/"include")],
                str(ROOT/"test/unit/cpp/weather_manager_harness.cpp"),"-o",str(binary)],
                capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=60,env=env)
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15,env=env)
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)

    def test_tls_response_and_error_contract(self):
        source=(ROOT/"src/protocols/RemoteResourceManager.cpp").read_text(encoding="utf-8")
        fetch=definition(source,"bool fetchWeather(")
        for token in ("setCACertBundle", "time(nullptr)>=1577836800", "HTTPC_DISABLE_FOLLOW_REDIRECTS",
                      "heap_caps_get_largest_free_block", "MAX_BODY", "providerFailure", "language=zh-Hans&unit=c"):
            self.assertIn(token,fetch)
        self.assertNotIn("setInsecure",fetch)
        self.assertNotIn('source["url"]',fetch)
        self.assertIn("15000U",definition(source,"class BoundedResponse"))

    def test_production_fetch_tls_bounds_and_provider_errors(self):
        compiler=shutil.which("g++") or shutil.which("clang++")
        fallback=Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists(): compiler=str(fallback)
        if not compiler: raise RuntimeError("C++17 compiler required")
        source=(ROOT/"src/protocols/RemoteResourceManager.cpp").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="fastbee-weather-fetch-") as folder:
            temp=Path(folder)
            # Extend only the Arduino String shim with the normal concat API.
            arduino=(ROOT/"test/mocks/Arduino.h").read_text(encoding="utf-8")
            arduino=arduino.replace("    void clear() { _value.clear(); }",
                "    bool concat(const char* text, size_t len) { _value.append(text,len); return true; }\n"
                "    void clear() { _value.clear(); }")
            (temp/"Arduino.h").write_text(arduino,encoding="utf-8")
            (temp/"fetch.inc").write_text(definition(source,"String escaped(")+"\n"+
                definition(source,"class BoundedResponse")+";\n"+definition(source,"bool fetchWeather("),encoding="utf-8")
            headers=next((ROOT/".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
            binary=temp/("fetch.exe" if os.name=="nt" else "fetch")
            env=dict(os.environ); env["PATH"]=str(Path(compiler).parent)+os.pathsep+env.get("PATH","")
            result=subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0","-DARDUINOJSON_ENABLE_STD_STRING=1",
                *[f"-I{p}" for p in (temp,headers.parent,ROOT/"include")],
                str(ROOT/"test/unit/cpp/weather_fetch_harness.cpp"),"-o",str(binary)],
                capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=60,env=env)
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15,env=env)
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)

if __name__ == "__main__": unittest.main()
