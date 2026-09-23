"""Compile the production OTA admission helper with OTA absent and present."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RemoteOtaAdmissionTests(unittest.TestCase):
    def test_ota_disabled_has_no_link_dependency_and_enabled_reports_busy(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++ compiler is required")
        source = (ROOT / "src/protocols/RemoteResourceManager.cpp").read_text(encoding="utf-8")
        helper = source[source.index("bool otaBusy() {"):source.index("String escaped(")]
        prefix = """
#include <cassert>
#if FASTBEE_ENABLE_OTA
struct OTAManager { bool busy = false; bool isOTAInProgress() const { return busy; } };
struct FastBeeFramework {
    static FastBeeFramework* current;
    OTAManager* ota = nullptr;
    static FastBeeFramework* getInstance() { return current; }
    OTAManager* getOTAManager() { return ota; }
};
FastBeeFramework* FastBeeFramework::current = nullptr;
#endif
"""
        checks = """
int main() {
    assert(!otaBusy());
#if FASTBEE_ENABLE_OTA
    FastBeeFramework framework;
    FastBeeFramework::current = &framework;
    assert(!otaBusy());
    OTAManager manager;
    framework.ota = &manager;
    assert(!otaBusy());
    manager.busy = true;
    assert(otaBusy());
    manager.busy = false;
    assert(!otaBusy());
#endif
}
"""
        with tempfile.TemporaryDirectory(prefix="fastbee-remote-ota-") as directory:
            path = Path(directory)
            cpp = path / "admission.cpp"
            cpp.write_text(prefix + helper + checks, encoding="utf-8")
            for enabled in (0, 1):
                executable = path / (f"admission-{enabled}" + (".exe" if os.name == "nt" else ""))
                result = subprocess.run(
                    [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                     f"-DFASTBEE_ENABLE_OTA={enabled}", str(cpp), "-o", str(executable)],
                    capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
