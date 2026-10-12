import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class WorkerStackProfileTests(unittest.TestCase):
    def test_production_stack_constants_for_memory_profiles(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = pathlib.Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        source = (ROOT / "include/core/PeriphExecWorkerPool.h").read_text(encoding="utf-8")
        start = source.index("#if defined(BOARD_HAS_PSRAM) && FASTBEE_USE_PSRAM")
        end = source.index("#endif", start) + len("#endif")
        production = source[start:end]
        environment = dict(os.environ)
        environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        cases = [
            ("c3_c6", 4096, False, 0, 8192),
            ("classic_s3_no_psram", 6144, False, 0, 8192),
            ("board_psram_disabled", 6144, True, 0, 8192),
            ("psram_flag_without_board", 6144, False, 1, 8192),
            ("s3_psram", 6144, True, 1, 12288),
            ("custom_larger_stack", 16384, True, 1, 16384),
            ("custom_no_psram_stack", 12288, False, 0, 12288),
        ]
        with tempfile.TemporaryDirectory(prefix="fastbee-worker-stack-") as directory:
            temporary = pathlib.Path(directory)
            cpp = temporary / "profile.cpp"
            cpp.write_text("#include <cstdint>\n" + production +
                           "\nstatic_assert(WORKER_STACK == EXPECTED_STACK, \"stack profile mismatch\");\n",
                           encoding="utf-8")
            for name, simple, board_psram, use_psram, expected in cases:
                with self.subTest(profile=name):
                    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                               f"-DSIMPLE_TASK_STACK={simple}", f"-DFASTBEE_USE_PSRAM={use_psram}",
                               f"-DEXPECTED_STACK={expected}", "-fsyntax-only", str(cpp)]
                    if board_psram:
                        command.append("-DBOARD_HAS_PSRAM")
                    result = subprocess.run(command, capture_output=True, text=True,
                                            timeout=30, env=environment)
                    self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
