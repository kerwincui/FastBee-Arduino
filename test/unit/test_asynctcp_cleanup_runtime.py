"""Fault injection for actual patched AsyncTCP accept/abort cleanup methods."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition
from test_libdeps_patch_contract import PATCH_MODULE

ROOT = Path(__file__).resolve().parents[2]


class AsyncTcpCleanupRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("g++") or shutil.which("clang++")
        if not cls.compiler:
            raise unittest.SkipTest("A C++17 compiler is required")
        candidates = sorted((ROOT / ".pio/libdeps").glob("*/AsyncTCP/src/AsyncTCP.cpp"))
        if not candidates:
            raise unittest.SkipTest("Install AsyncTCP before running cleanup tests")
        installed = candidates[0].read_text(encoding="utf-8")
        cls.source, _ = PATCH_MODULE._patch_async_tcp_source_text(installed)
        again, changed = PATCH_MODULE._patch_async_tcp_source_text(cls.source)
        if changed or again != cls.source:
            raise AssertionError("AsyncTCP patch must be idempotent")

    def run_method_harness(self, mode, mutation=None):
        source = self.source
        if mutation == "abort":
            method = definition(source, "int8_t AsyncClient::abort()")
            source = source.replace(method, method.replace("  _fastbee_release_accepted();\n", "", 1), 1)
        elif mutation == "accept":
            method = definition(source, "int8_t AsyncTCP_detail::tcp_accept(")
            source = source.replace(method, method.replace("      _reset_tcp_callbacks(pcb, c);\n", "", 1), 1)
        signatures = (
            "static void _reset_tcp_callbacks(",
            "void AsyncTCP_detail::tcp_error(",
            "static err_t _tcp_close_api(",
            "static esp_err_t _tcp_close(",
            "static err_t _tcp_abort_api(",
            "static esp_err_t _tcp_abort(",
            "void AsyncClient::_fastbee_release_accepted()",
            "AsyncClient::~AsyncClient()",
            "void AsyncClient::close()",
            "int8_t AsyncClient::abort()",
            "int8_t AsyncClient::_close()",
            "void AsyncClient::_error(",
            "int8_t AsyncTCP_detail::tcp_accept(",
        )
        with tempfile.TemporaryDirectory(prefix="fastbee-tcp-cleanup-") as directory:
            temporary = Path(directory)
            (temporary / "tcp_cleanup_methods.inc").write_text(
                "\n\n".join(definition(source, signature) for signature in signatures),
                encoding="utf-8")
            binary = temporary / ("cleanup.exe" if os.name == "nt" else "cleanup")
            result = subprocess.run(
                [self.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", f"-I{temporary}",
                 str(ROOT / "test/unit/cpp/asynctcp_cleanup_harness.cpp"),
                 "-o", str(binary)], capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return subprocess.run([str(binary), mode], capture_output=True,
                                  text=True, timeout=15)

    def test_abort_releases_slot_before_retained_client_destruction(self):
        result = self.run_method_harness("abort")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_accept_event_oom_cannot_queue_a_deleted_client(self):
        result = self.run_method_harness("accept")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_original_abort_and_oom_order_are_detected(self):
        for mode, expected in (("abort", "abort retained admission slot"),
                               ("accept", "queued event references deleted client")):
            with self.subTest(mode=mode):
                result = self.run_method_harness(mode, mode)
                self.assertEqual(result.returncode, 86, result.stdout + result.stderr)
                self.assertIn(expected, result.stderr)


if __name__ == "__main__":
    unittest.main()
