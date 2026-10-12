import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_libdeps_patch_contract import PATCH_MODULE
from test.unit.zero_progress_support import (
    RUNTIME_ENVIRONMENT, braced_block, copy_web_members, progress_methods,
    select_runtime_inputs, verify_old_fixture, verify_runtime_inputs_unchanged,
    write_progress_headers,
)

ROOT = pathlib.Path(__file__).resolve().parents[2]


class WebResponseBatchTests(unittest.TestCase):
    def test_full_installer_on_installed_312_is_idempotent(self):
        inputs = select_runtime_inputs(ROOT, RUNTIME_ENVIRONMENT)
        with tempfile.TemporaryDirectory(prefix="fastbee-web-install-") as directory:
            target = pathlib.Path(directory) / "WebResponses.cpp"
            shutil.copyfile(inputs.web / "src/WebResponses.cpp", target)
            PATCH_MODULE._patch_web_responses(target)
            first = target.read_text(encoding="utf-8")
            self.assertFalse(PATCH_MODULE._patch_web_responses(target))
            self.assertEqual(first, target.read_text(encoding="utf-8"))
            self.assertIn("bound a low-DRAM response batch", first)
            self.assertEqual(2, first.count("FastBee patch: retry only previously failed TCP output"))
        verify_runtime_inputs_unchanged(inputs)

    def test_installed_header_upgrade_preserves_dynamic_buffers_and_is_idempotent(self):
        inputs = select_runtime_inputs(ROOT, RUNTIME_ENVIRONMENT)
        with tempfile.TemporaryDirectory(prefix="fastbee-web-header-") as directory:
            target = pathlib.Path(directory) / "WebResponseImpl.h"
            source = (inputs.web / "src/WebResponseImpl.h").read_text(encoding="utf-8")
            # Explicitly exercise the previous deployed header's upgrade.
            source = source.replace("  size_t _fastbeePendingOutputBytes{0};\n", "")
            target.write_text(source, encoding="utf-8")
            self.assertTrue(PATCH_MODULE._patch_web_response_impl(target))
            first = target.read_text(encoding="utf-8")
            self.assertEqual(2, first.count("size_t _fastbeePendingOutputBytes{0};"))
            self.assertFalse(PATCH_MODULE._patch_web_response_impl(target))
            self.assertEqual(first, target.read_text(encoding="utf-8"))
        verify_runtime_inputs_unchanged(inputs)

    def test_real_response_code_keeps_complete_bytes_under_bounded_batches(self):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        source = (ROOT / "patches/libdeps/ESPAsyncWebServer@3.11.1/src/WebResponses.cpp").read_text(encoding="utf-8")
        source, changed = PATCH_MODULE._patch_bounded_response_batch(source)
        self.assertTrue(changed)
        second, changed = PATCH_MODULE._patch_bounded_response_batch(source)
        self.assertFalse(changed)
        self.assertEqual(source, second)
        source, changed = PATCH_MODULE._patch_bounded_basic_response(source)
        self.assertTrue(changed)
        source, changed = PATCH_MODULE._patch_outstanding_response_budget(source)
        self.assertTrue(changed)
        second, changed = PATCH_MODULE._patch_outstanding_response_budget(source)
        self.assertFalse(changed)
        self.assertEqual(source, second)
        source, changed = PATCH_MODULE._patch_basic_queued_accounting(source)
        self.assertTrue(changed)
        source, changed = PATCH_MODULE._patch_chunked_terminal_drain(source)
        self.assertTrue(changed)
        second, changed = PATCH_MODULE._patch_chunked_terminal_drain(source)
        self.assertFalse(changed)
        self.assertEqual(source, second)
        source, changed = PATCH_MODULE._patch_low_dram_staging_capacity(source)
        self.assertTrue(changed)
        second, changed = PATCH_MODULE._patch_low_dram_staging_capacity(source)
        self.assertFalse(changed)
        self.assertEqual(source, second)
        signature = "size_t AsyncAbstractResponse::write_send_buffs(AsyncWebServerRequest *request, size_t len, uint32_t time)"
        source, changed = PATCH_MODULE._patch_pending_output_retry(source)
        self.assertTrue(changed)
        second, changed = PATCH_MODULE._patch_pending_output_retry(source)
        self.assertFalse(changed)
        self.assertEqual(source, second)
        source, changed = PATCH_MODULE._patch_peer_drained_completion(source)
        self.assertTrue(changed)
        second, changed = PATCH_MODULE._patch_peer_drained_completion(source)
        self.assertFalse(changed)
        self.assertEqual(source, second)
        source, changed = PATCH_MODULE._patch_finite_progress_response(source)
        self.assertTrue(changed)
        second, changed = PATCH_MODULE._patch_finite_progress_response(source)
        self.assertFalse(changed)
        self.assertEqual(source, second)
        source, changed = PATCH_MODULE._patch_basic_finite_progress_response(source)
        self.assertTrue(changed)
        second, changed = PATCH_MODULE._patch_basic_finite_progress_response(source)
        self.assertFalse(changed)
        self.assertEqual(source, second)
        basic_signature = "size_t AsyncBasicResponse::write_send_buffs(AsyncWebServerRequest *request, size_t len, uint32_t time)"
        with tempfile.TemporaryDirectory(prefix="fastbee-web-batch-") as directory:
            temporary = pathlib.Path(directory)
            library = temporary / "ESPAsyncWebServer"
            copy_web_members(verify_old_fixture(), library)
            self.assertTrue(PATCH_MODULE.apply_to_library(library, log=lambda _: None))
            write_progress_headers(temporary, library)
            (temporary / "production_response_batch.inc").write_text(
                progress_methods(source) + signature + " {" + braced_block(source, signature) + "}\n" +
                basic_signature + " {" + braced_block(source, basic_signature) + "}", encoding="utf-8")
            executable = temporary / "batch.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror",f"-I{temporary}",
                                    str(ROOT / "test/unit/cpp/web_response_batch_harness.cpp"),"-o",str(executable)],
                                   capture_output=True,text=True,timeout=60,env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            result = subprocess.run([str(executable)],capture_output=True,text=True,timeout=10,env=environment)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
