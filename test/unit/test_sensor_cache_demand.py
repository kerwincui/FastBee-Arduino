"""Exercise the actual demand-allocated sensor cache and cooperative release."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SensorCacheDemandTests(unittest.TestCase):
    def test_actual_cache_cold_oom_release_eviction_and_cooperative_replacement(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        with tempfile.TemporaryDirectory(prefix="fastbee-sensor-cache-demand-") as directory:
            executable = Path(directory) / ("demand.exe" if os.name == "nt" else "demand")
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'test/unit/cpp/sensor_cache_mocks'}",
                f"-I{ROOT / 'test/unit/cpp/sensor_mocks'}",
                f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                str(ROOT / "test/unit/cpp/sensor_cache_demand_harness.cpp"),
                str(ROOT / "src/core/SensorDriverCache.cpp"), "-o", str(executable)]
            build = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=90, env=environment)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=15, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("sensor cache demand lifecycle checks passed", result.stdout)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
