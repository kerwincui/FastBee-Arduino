"""Execute the temporary screen worker diagnostic policy with a host compiler."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ScreenWorkerIsolationTests(unittest.TestCase):
    def test_diagnostic_worker_code_requires_exact_compile_time_opt_in(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        source = (ROOT / "src/screen/ScreenRuntime.cpp").read_text(encoding="utf-8")
        # Preprocess the actual worker branches without target header contents;
        # this checks opt-in elimination, not hardware type/link compatibility.
        source = "\n".join(line for line in source.splitlines()
                           if not line.lstrip().startswith("#include"))
        results = {}
        for flag in (None, 0, 1, 2):
            command = [compiler, "-E", "-P", "-x", "c++", "-",
                       "-DFASTBEE_ENABLE_SCREEN_UI=1", "-DARDUINO_ARCH_ESP32=1"]
            if flag is not None:
                command.append(f"-DFASTBEE_SCREEN_WORKER_ISOLATION={flag}")
            result = subprocess.run(command, input=source, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            results[flag] = result.stdout
        self.assertEqual(results[None], results[0])
        self.assertEqual(results[None], results[2])
        self.assertNotIn("[SCRISO]", results[None])
        self.assertNotIn("ScreenWorkerIsolation", results[None])
        self.assertNotIn("ScreenIsolationTitle", results[None])
        self.assertIn("[SCRISO]", results[1])
        self.assertIn("ScreenWorkerIsolation isolation", results[1])

    def test_time_gates_failures_and_snapshot_title_restoration(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        env = dict(os.environ)
        env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="fastbee-screen-isolation-") as temporary:
            executable = Path(temporary) / ("isolation.exe" if os.name == "nt" else "isolation")
            build = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'include'}",
                str(ROOT / "test/unit/cpp/screen_worker_isolation_harness.cpp"),
                "-o", str(executable)
            ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, env=env)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=15, env=env)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("time gates, wrap, render failure and title restoration passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
