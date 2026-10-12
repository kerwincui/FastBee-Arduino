"""Actual finite response, Request, transport clock, destructor and lease regression."""
from contextlib import ExitStack
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

from test.unit.test_libdeps_patch_contract import PATCH_MODULE
from test.unit.test_sensor_publication import definition
from test.unit.zero_progress_support import (
    ROOT, RUNTIME_ENVIRONMENT, copy_web_members, method, progress_methods,
    select_runtime_inputs, verify_old_fixture, verify_runtime_inputs_unchanged,
    write_progress_headers,
)


class WebFiniteProgressTests(unittest.TestCase):
    def run_harness(self, mutation=None, *, basic=False, legacy_basic=False, expect_bounded=True):
        inputs = select_runtime_inputs(ROOT, RUNTIME_ENVIRONMENT)
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        environment = dict(os.environ)
        environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="fastbee-finite-progress-") as directory, ExitStack() as patch_context:
            temp = Path(directory)
            copy = temp / "ESPAsyncWebServer"
            if legacy_basic:
                assert basic
                # Install the existing fixes on the pinned SDK fixture,
                # leaving Basic on its original opt-out progress hook.
                for name in ("_patch_basic_finite_progress_impl", "_patch_basic_finite_progress_response"):
                    patch_context.enter_context(mock.patch.object(PATCH_MODULE, name, side_effect=lambda source: (source, False)))
            copy_web_members(verify_old_fixture() if legacy_basic else inputs.web, copy)
            # Installed input may already be upgraded; the pinned fixture below
            # separately requires the first old-to-new installation to change it.
            PATCH_MODULE.apply_to_library(copy, log=lambda _: None)
            snapshot = {p.relative_to(copy): p.read_bytes() for p in copy.rglob("*") if p.is_file()}
            self.assertFalse(PATCH_MODULE.apply_to_library(copy, log=lambda _: None))
            self.assertEqual(snapshot, {p.relative_to(copy): p.read_bytes() for p in copy.rglob("*") if p.is_file()})
            source = (copy / "src/WebResponses.cpp").read_text(encoding="utf-8")
            request = (copy / "src/WebRequest.cpp").read_text(encoding="utf-8")
            tcp = inputs.tcp_source.read_text(encoding="utf-8")
            impl = (copy / "src/WebResponseImpl.h").read_text(encoding="utf-8")
            handler = (ROOT / "src/network/WebHandlerContext.cpp").read_text(encoding="utf-8")
            hook_signature = "bool AsyncAbstractResponse::_fastbeeCheckProgress(AsyncWebServerRequest *request)"
            # Keep mutations on Abstract when Basic has its own progress hook.
            hook_start = source.index(hook_signature)
            unchanged_prefix, source = source[:hook_start], source[hook_start:]
            if mutation == "disabled":
                actual = definition(source, hook_signature)
                source = source.replace(actual, hook_signature + " { (void)request; return true; }", 1)
            elif mutation == "empty_send":
                anchor = "  if (static_cast<uint32_t>(now - _fastbeeLastProgressMs) >= 10000U) {"
                source = source.replace(anchor, "  _fastbeeLastProgressMs = std::max(_fastbeeLastProgressMs, request->client()->lastSuccessfulSend);\n" + anchor, 1)
            elif mutation == "ack_length":
                anchor = "  if (!_fastbeeCheckProgress(request)) return 0;\n"
                source = source.replace(anchor, "  if (len) _fastbeeLastProgressMs = millis();\n" + anchor, 1)
            elif mutation == "constant_space":
                source = source.replace("drained > _fastbeeDrainedHighWater", "tcpSpace > 0", 1)
            elif mutation == "writable_poll_only":
                request = request.replace(PATCH_MODULE._FINITE_REQUEST_CHECK, "")
            source = unchanged_prefix + source
            write_progress_headers(temp, copy)
            response_signatures = (
                "size_t AsyncAbstractResponse::write_send_buffs(AsyncWebServerRequest *request, size_t len, uint32_t time)",
                "size_t AsyncBasicResponse::write_send_buffs(AsyncWebServerRequest *request, size_t len, uint32_t time)",
            )
            (temp / "production_response_batch.inc").write_text(
                progress_methods(source) + (method(source, "void AsyncBasicResponse::_respond(AsyncWebServerRequest *request)") if basic else "") +
                "".join(method(source, signature) for signature in response_signatures), encoding="utf-8")
            request_signatures = (
                "void AsyncWebServerRequest::_onAck(size_t len, uint32_t time)",
                "void AsyncWebServerRequest::_onPoll()",
                "void AsyncWebServerRequest::_onTimeout(uint32_t time)",
                "void AsyncWebServerRequest::_onDisconnect()",
                "void AsyncWebServerRequest::onDisconnect(ArDisconnectHandler fn)",
            )
            peer = "".join(method(request, signature) for signature in request_signatures)
            peer += definition(tcp, "static err_t _tcp_close_api(") + "\n"
            (temp / "production_peer_drain.inc").write_text(peer, encoding="utf-8")
            transport = "\n".join(definition(tcp, signature) for signature in (
                "bool AsyncClient::send()", "size_t AsyncClient::space() const",
                "int8_t AsyncClient::_poll(tcp_pcb *pcb)", "int8_t AsyncClient::_close()",
                "void AsyncClient::_fastbee_release_accepted()"))
            if basic:
                transport = definition(tcp, "size_t AsyncClient::add(") + "\n" + transport
            if mutation == "release_slot":
                transport = transport.replace("  _fastbee_release_accepted();\n", "", 1)
            (temp / "production_transport_clock.inc").write_text(
                transport.replace("AsyncClient::", "TransportClock::"), encoding="utf-8")
            (temp / "production_owned_request.inc").write_text(
                definition(request, "AsyncWebServerRequest::~AsyncWebServerRequest()"), encoding="utf-8")
            (temp / "production_file_destructor.inc").write_text(
                definition(impl, "~AsyncFileResponse()"), encoding="utf-8")
            (temp / "production_lease_types.inc").write_text("\n".join(
                definition(handler, signature) + ";" for signature in (
                    "struct StaticInFlightLease", "struct StaticInFlightSlot")), encoding="utf-8")
            (temp / "production_lease_release.inc").write_text("\n".join(
                definition(handler, signature) for signature in (
                    "static void releaseStaticInFlightLease(", "static void registerStaticInFlightRelease(")), encoding="utf-8")
            executable = temp / "finite.exe"
            harness = "web_basic_progress_harness.cpp" if basic else "web_finite_progress_harness.cpp"
            flags = [f"-DEXPECT_BOUNDED={int(expect_bounded)}", "-DCONFIG_LWIP_TCP_MSS=1436",
                "-DFASTBEE_TEST_BASIC_RESPOND"] if basic else []
            build = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DTCP_SND_BUF=5744",
                *flags, f"-I{temp}", str(ROOT / "test/unit/cpp" / harness), "-o", str(executable),
            ], capture_output=True, text=True, encoding="utf-8", errors="backslashreplace", timeout=60, env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, encoding="utf-8", errors="backslashreplace", timeout=15, env=environment)
            verify_runtime_inputs_unchanged(inputs)
            return result

    def test_fixed_pre_finite_fixture_upgrades_and_is_idempotent(self):
        fixture = verify_old_fixture()
        with tempfile.TemporaryDirectory(prefix="fastbee-fixed-web-upgrade-") as directory:
            copy = Path(directory) / "library"
            copy_web_members(fixture, copy)
            self.assertTrue(PATCH_MODULE.apply_to_library(copy, log=lambda _: None))
            snapshot = {p.relative_to(copy): p.read_bytes() for p in copy.rglob("*") if p.is_file()}
            self.assertFalse(PATCH_MODULE.apply_to_library(copy, log=lambda _: None))
            self.assertEqual(snapshot, {p.relative_to(copy): p.read_bytes() for p in copy.rglob("*") if p.is_file()})

    def test_actual_finite_progress_paths(self):
        result = self.run_harness()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_original_and_false_progress_mutations_are_detected(self):
        for mutation in ("disabled", "empty_send", "ack_length", "constant_space", "writable_poll_only", "release_slot"):
            with self.subTest(mutation=mutation):
                result = self.run_harness(mutation)
                self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertIn("Assertion", result.stderr)
                self.assertIn("failed", result.stderr)

    def test_partial_progress_upgrades_are_rejected(self):
        fixture = verify_old_fixture()
        base = (fixture / "src/ESPAsyncWebServer.h").read_text(encoding="utf-8")
        base, _ = PATCH_MODULE._patch_finite_progress_base(base)
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_finite_progress_base(base.replace("    (void)request;\n", "", 1))
        impl = (fixture / "src/WebResponseImpl.h").read_text(encoding="utf-8")
        impl, _ = PATCH_MODULE._patch_finite_progress_impl(impl)
        for missing in ("  bool _fastbeeProgressInitialized{false};\n", PATCH_MODULE._FINITE_PROGRESS_DECLARATION):
            damaged = impl.replace(missing, "", 1)
            # Missing a complete declaration alone is an ordinary old-to-new
            # upgrade; partial state of a joint fields/declaration upgrade is not.
            with self.assertRaises(PATCH_MODULE.PatchError):
                PATCH_MODULE._patch_finite_progress_impl(damaged)
        request = (fixture / "src/WebRequest.cpp").read_text(encoding="utf-8")
        request, _ = PATCH_MODULE._patch_finite_progress_request(request)
        incomplete = PATCH_MODULE._FINITE_REQUEST_CHECK.replace("    _client->close();\n", "")
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_finite_progress_request(request.replace(PATCH_MODULE._FINITE_REQUEST_CHECK, incomplete, 1))
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_finite_progress_request(request.replace(PATCH_MODULE._FINITE_REQUEST_CHECK, "", 1))

    def test_four_file_partial_install_and_write_failure_preserve_originals(self):
        with tempfile.TemporaryDirectory(prefix="fastbee-finite-install-") as directory:
            copy = Path(directory) / "library"
            copy_web_members(verify_old_fixture(), copy)
            members = tuple(copy / "src" / name for name in (
                "ESPAsyncWebServer.h", "WebRequest.cpp", "WebResponseImpl.h", "WebResponses.cpp"))
            original = {path: path.read_bytes() for path in members}
            # Preflight every source before any write, including a bad final source.
            members[-1].write_text("unknown response source", encoding="utf-8")
            broken = {path: path.read_bytes() for path in members}
            with self.assertRaises(PATCH_MODULE.PatchError):
                PATCH_MODULE.apply_to_library(copy, log=lambda _: None)
            self.assertEqual(broken, {path: path.read_bytes() for path in members})
            for path, data in original.items():
                path.write_bytes(data)
            actual_write = PATCH_MODULE._write
            def fail_third(path, text):
                if path == members[2]:
                    path.write_text("partial write", encoding="utf-8")
                    raise OSError("injected third-file failure")
                actual_write(path, text)
            with mock.patch.object(PATCH_MODULE, "_write", side_effect=fail_third):
                with self.assertRaises(PATCH_MODULE.PatchError):
                    PATCH_MODULE.apply_to_library(copy, log=lambda _: None)
            self.assertEqual(original, {path: path.read_bytes() for path in members})
            self.assertTrue(PATCH_MODULE.apply_to_library(copy, log=lambda _: None))
            complete = {path: path.read_bytes() for path in members}
            for old_member in members:
                old_member.write_bytes(original[old_member])
                partial = {path: path.read_bytes() for path in members}
                with self.assertRaises(PATCH_MODULE.PatchError):
                    PATCH_MODULE.apply_to_library(copy, log=lambda _: None)
                self.assertEqual(partial, {path: path.read_bytes() for path in members})
                old_member.write_bytes(complete[old_member])


if __name__ == "__main__":
    unittest.main()
