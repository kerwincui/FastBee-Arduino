"""Regression checks for the dependency patch that protects low-memory Web responses."""

from __future__ import annotations

import json
import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
PLATFORMIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")
PATCH_SCRIPT = (ROOT / "scripts/apply-libdeps-patches.py").read_text(encoding="utf-8")
VALIDATOR_SCRIPT = (ROOT / "scripts/validate-libdeps-patches.js").read_text(encoding="utf-8")

PATCH_SPEC = importlib.util.spec_from_file_location(
    "fastbee_apply_libdeps_patches", ROOT / "scripts/apply-libdeps-patches.py"
)
assert PATCH_SPEC and PATCH_SPEC.loader
PATCH_MODULE = importlib.util.module_from_spec(PATCH_SPEC)
sys.modules[PATCH_SPEC.name] = PATCH_MODULE
PATCH_SPEC.loader.exec_module(PATCH_MODULE)


class LibdepsPatchContractTests(unittest.TestCase):
    def test_patch_hook_is_registered_before_build(self):
        self.assertIn("pre:scripts/apply-libdeps-patches.py", PLATFORMIO)

    def test_libdeps_path_resolution_preserves_default_and_custom_layouts(self):
        with tempfile.TemporaryDirectory(prefix="fastbee-libdeps-path-") as temporary:
            project = pathlib.Path(temporary) / "project"
            absolute = pathlib.Path(temporary) / "shared-libdeps"
            self.assertEqual(
                (project / ".pio" / "libdeps").resolve(),
                PATCH_MODULE._resolve_libdeps_dir(project, None),
            )
            self.assertEqual(
                (project / "cache" / "libdeps").resolve(),
                PATCH_MODULE._resolve_libdeps_dir(project, "cache/libdeps"),
            )
            self.assertEqual(
                absolute.resolve(),
                PATCH_MODULE._resolve_libdeps_dir(project, absolute),
            )

    def test_scons_context_uses_actual_project_libdeps_dir_and_config_fallback(self):
        class FakeConfig:
            def __init__(self, libdeps):
                self.libdeps = libdeps
                self.calls = []

            def get(self, section, option):
                self.calls.append((section, option))
                return self.libdeps

        class FakeEnv:
            def __init__(self, project, expanded, configured):
                self.values = {
                    "$PROJECT_DIR": str(project),
                    "$PIOENV": "esp32s3-touch-test",
                    "$PROJECT_LIBDEPS_DIR": expanded,
                }
                self.config = FakeConfig(configured)

            def subst(self, value):
                return self.values[value]

            def GetProjectConfig(self):
                return self.config

        with tempfile.TemporaryDirectory(prefix="fastbee-libdeps-scons-") as temporary:
            project = pathlib.Path(temporary) / "project"
            absolute = pathlib.Path(temporary) / "actual-libdeps"
            cases = (
                (str(absolute), "ignored/from-config", absolute.resolve()),
                ("relative/libdeps", "ignored/from-config", (project / "relative/libdeps").resolve()),
                ("$PROJECT_LIBDEPS_DIR", "configured/libdeps", (project / "configured/libdeps").resolve()),
                ("$PROJECT_LIBDEPS_DIR", None, (project / ".pio/libdeps").resolve()),
            )
            for expanded, configured, expected in cases:
                with self.subTest(expanded=expanded, configured=configured):
                    fake_env = FakeEnv(project, expanded, configured)
                    with mock.patch.object(PATCH_MODULE, "Import", create=True), mock.patch.object(
                        PATCH_MODULE, "env", fake_env, create=True
                    ):
                        context = PATCH_MODULE._platformio_context()
                    self.assertEqual(
                        (project.resolve(), "esp32s3-touch-test", expected), context
                    )
                    self.assertEqual(
                        [("platformio", "libdeps_dir")], fake_env.config.calls
                    )

    def test_patch_is_version_checked_and_idempotent(self):
        self.assertIn('version != "3.12.0"', PATCH_SCRIPT)
        self.assertIn('version != "3.5.0"', PATCH_SCRIPT)
        self.assertIn("FastBee patch: dynamic send buffer", PATCH_SCRIPT)
        self.assertIn("kFallbackSizes", PATCH_SCRIPT)
        self.assertIn("zero-read retry", PATCH_SCRIPT)
        self.assertIn("chunkCap", PATCH_SCRIPT)
        self.assertIn("responseBufferCap", PATCH_SCRIPT)
        self.assertIn("hasPendingSendBuffer", PATCH_SCRIPT)
        self.assertIn("writable TCP space is authoritative liveness evidence", PATCH_SCRIPT)
        self.assertIn("return RESPONSE_TRY_AGAIN;", PATCH_SCRIPT)
        self.assertIn("zero progress must not consume the finite send credit", PATCH_SCRIPT)
        self.assertIn("account bytes accepted by AsyncTCP before a partial", PATCH_SCRIPT)
        self.assertIn("ERR_CONTENT_LENGTH_MISMATCH under memory pressure", PATCH_SCRIPT)
        self.assertIn("bool _ackTimeoutRecoveryAttempted = false;", PATCH_SCRIPT)
        self.assertIn("_response->_ack(this, 0, 0) > 0", PATCH_SCRIPT)
        self.assertIn("FastBee patch: reject a connection before allocating AsyncClient", PATCH_SCRIPT)
        self.assertIn("CONFIG_ASYNC_TCP_MAX_CONNECTIONS", PATCH_SCRIPT)
        self.assertIn(
            "heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
            PATCH_SCRIPT,
        )
        self.assertIn("__atomic_fetch_add", PATCH_SCRIPT)
        self.assertIn("__atomic_fetch_sub", PATCH_SCRIPT)
        self.assertIn("FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE", PATCH_SCRIPT)
        self.assertIn("_fastbee_alloc_error_event(client)", PATCH_SCRIPT)
        self.assertIn("_fastbee_release_error_event(evpkt)", PATCH_SCRIPT)
        self.assertIn("already present", PATCH_SCRIPT)

    def test_validator_requires_all_accepted_client_release_paths(self):
        for marker in (
            "void _fastbee_release_accepted();",
            "void AsyncClient::_fastbee_release_accepted()",
            "__atomic_exchange_n(",
            "client->_fastbee_release_accepted();",
            "c->_fastbee_release_accepted();",
        ):
            self.assertIn(marker, VALIDATOR_SCRIPT)

    def test_async_tcp_gate_installs_and_is_idempotent(self):
        header = "\n".join((
            PATCH_MODULE._ASYNCTCP_HEADER_PROTECTED,
            PATCH_MODULE._ASYNCTCP_HEADER_CONNECT_PORT,
        ))
        source = "\n".join((
            PATCH_MODULE._ASYNCTCP_EVENT_PACKET_CONSTRUCTOR,
            PATCH_MODULE._ASYNCTCP_QUEUE_GLOBALS,
            PATCH_MODULE._ASYNCTCP_FREE_EVENT,
            PATCH_MODULE._ASYNCTCP_CLIENT_SECTION,
            PATCH_MODULE._ASYNCTCP_CONSTRUCTOR_TAIL,
            PATCH_MODULE._ASYNCTCP_DESTRUCTOR,
            PATCH_MODULE._ASYNCTCP_CLOSE,
            PATCH_MODULE._ASYNCTCP_ABORT,
            PATCH_MODULE._ASYNCTCP_TCP_ERROR,
            PATCH_MODULE._ASYNCTCP_ACCEPT_HEAD,
            PATCH_MODULE._ASYNCTCP_ACCEPT_CLIENT,
            PATCH_MODULE._ASYNCTCP_ACCEPT_EVENT_FAILURE,
        ))

        patched_header, header_changed = PATCH_MODULE._patch_async_tcp_header_text(header)
        patched_source, source_changed = PATCH_MODULE._patch_async_tcp_source_text(source)
        self.assertTrue(header_changed)
        self.assertTrue(source_changed)
        self.assertIn("_fastbee_accepted_count", patched_header)
        self.assertIn("_fastbee_server_accepted", patched_header)
        self.assertIn("void _fastbee_release_accepted();", patched_header)
        self.assertIn("CONFIG_ASYNC_TCP_MAX_CONNECTIONS", patched_source)
        self.assertIn(
            "heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
            patched_source,
        )
        self.assertIn(
            "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
            patched_source,
        )
        failure_start = patched_source.index("c->_pcb = nullptr;")
        failure_block = patched_source[failure_start : failure_start + 500]
        self.assertIn("c->_fastbee_release_accepted();", failure_block)
        self.assertIn("delete c;", failure_block)
        self.assertLess(patched_source.index("_reset_tcp_callbacks(pcb, c);"), failure_start)
        abort_start = patched_source.index("int8_t AsyncClient::abort()")
        abort_block = patched_source[abort_start : abort_start + 400]
        self.assertLess(abort_block.index("_fastbee_release_accepted();"),
                        abort_block.index("_tcp_abort(&_pcb, this)"))
        self.assertIn("__atomic_exchange_n(", patched_source)
        self.assertIn("FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE", patched_source)
        self.assertIn("_fastbee_alloc_error_event(client)", patched_source)
        self.assertIn("_fastbee_release_error_event(evpkt)", patched_source)
        close_start = patched_source.index("int8_t AsyncClient::_close()")
        close_block = patched_source[close_start : close_start + 400]
        self.assertIn("_fastbee_release_accepted();", close_block)
        error_start = patched_source.index(
            "// LwIP has already freed the PCB. Release the admission slot"
        )
        error_block = patched_source[error_start : error_start + 600]
        self.assertLess(
            error_block.index("client->_fastbee_release_accepted();"),
            error_block.index("new (std::nothrow) lwip_tcp_event_packet_t"),
        )
        destructor_start = patched_source.index("AsyncClient::~AsyncClient()")
        destructor_block = patched_source[destructor_start : destructor_start + 250]
        self.assertIn("_fastbee_release_accepted();", destructor_block)

        header_again, header_changed_again = PATCH_MODULE._patch_async_tcp_header_text(patched_header)
        source_again, source_changed_again = PATCH_MODULE._patch_async_tcp_source_text(patched_source)
        self.assertFalse(header_changed_again)
        self.assertFalse(source_changed_again)
        self.assertEqual(patched_header, header_again)
        self.assertEqual(patched_source, source_again)

        # Upgrade the immediately preceding r11 patch as well as a clean
        # upstream source. The old rollback is a suffix of the new block,
        # so this also catches accidentally re-inserting cleanup each build.
        r11 = patched_source.replace(PATCH_MODULE._ASYNCTCP_ABORT_PATCHED,
                                     PATCH_MODULE._ASYNCTCP_ABORT, 1).replace(
            PATCH_MODULE._ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED,
            PATCH_MODULE._ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED_R11, 1)
        upgraded, upgraded_changed = PATCH_MODULE._patch_async_tcp_source_text(r11)
        self.assertTrue(upgraded_changed)
        self.assertEqual(patched_source, upgraded)
        upgraded_again, upgraded_again_changed = PATCH_MODULE._patch_async_tcp_source_text(upgraded)
        self.assertFalse(upgraded_again_changed)
        self.assertEqual(upgraded, upgraded_again)

    def test_async_tcp_gate_upgrades_legacy_patch_and_is_idempotent(self):
        header = "\n".join((
            PATCH_MODULE._ASYNCTCP_HEADER_PROTECTED_PATCHED,
            PATCH_MODULE._ASYNCTCP_HEADER_CONNECT_PORT_PATCHED_LEGACY,
        ))
        source = "\n".join((
            PATCH_MODULE._ASYNCTCP_EVENT_PACKET_CONSTRUCTOR,
            PATCH_MODULE._ASYNCTCP_QUEUE_GLOBALS,
            PATCH_MODULE._ASYNCTCP_FREE_EVENT,
            PATCH_MODULE._ASYNCTCP_CLIENT_SECTION_PATCHED_LEGACY,
            PATCH_MODULE._ASYNCTCP_CONSTRUCTOR_TAIL_PATCHED,
            PATCH_MODULE._ASYNCTCP_DESTRUCTOR_PATCHED_LEGACY,
            PATCH_MODULE._ASYNCTCP_CLOSE,
            PATCH_MODULE._ASYNCTCP_ABORT,
            PATCH_MODULE._ASYNCTCP_TCP_ERROR,
            PATCH_MODULE._ASYNCTCP_ACCEPT_HEAD_PATCHED,
            PATCH_MODULE._ASYNCTCP_ACCEPT_CLIENT_PATCHED,
            PATCH_MODULE._ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED_COUNTER,
        ))

        patched_header, header_changed = PATCH_MODULE._patch_async_tcp_header_text(header)
        patched_source, source_changed = PATCH_MODULE._patch_async_tcp_source_text(source)
        self.assertTrue(header_changed)
        self.assertTrue(source_changed)
        self.assertIn("void _fastbee_release_accepted();", patched_header)
        self.assertIn("__atomic_exchange_n(", patched_source)
        self.assertIn("client->_fastbee_release_accepted();", patched_source)
        self.assertIn("c->_fastbee_release_accepted();", patched_source)
        self.assertNotIn("if (_fastbee_server_accepted) {", patched_source)

        header_again, header_changed_again = PATCH_MODULE._patch_async_tcp_header_text(patched_header)
        source_again, source_changed_again = PATCH_MODULE._patch_async_tcp_source_text(patched_source)
        self.assertFalse(header_changed_again)
        self.assertFalse(source_changed_again)
        self.assertEqual(patched_header, header_again)
        self.assertEqual(patched_source, source_again)

    def test_async_tcp_gate_refuses_unknown_source(self):
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_async_tcp_header_text("class Unknown {};")
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_async_tcp_source_text("int unrelated() { return 0; }")

    def test_async_tcp_library_refuses_unknown_version(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            library_dir = pathlib.Path(temp_dir)
            (library_dir / "library.json").write_text(
                json.dumps({"version": "3.5.1"}), encoding="utf-8"
            )
            with self.assertRaises(PATCH_MODULE.PatchError):
                PATCH_MODULE.apply_to_async_tcp_library(library_dir, log=lambda _: None)

    def test_target_connection_limits_cover_low_memory_and_psram_profiles(self):
        for expected in (4, 6, 8):
            self.assertIn(
                f"-DCONFIG_ASYNC_TCP_MAX_CONNECTIONS={expected}", PLATFORMIO
            )

    def test_c6_has_bounded_weak_link_ack_recovery_window(self):
        c6_start = PLATFORMIO.index("[esp32c6_runtime_flags]")
        c6_end = PLATFORMIO.index("[esp32s3_runtime_flags]", c6_start)
        c6_flags = PLATFORMIO[c6_start:c6_end]
        self.assertIn("-DCONFIG_ASYNC_TCP_MAX_ACK_TIME=10000", c6_flags)
        self.assertNotIn("-DCONFIG_ASYNC_TCP_MAX_ACK_TIME=10000", PLATFORMIO[:c6_start])

    def test_zero_read_fix_installs_upgrades_and_is_idempotent(self):
        for source in (PATCH_MODULE._UPSTREAM_FILE_FILL, PATCH_MODULE._LEGACY_FILE_FILL):
            patched, changed = PATCH_MODULE._patch_file_zero_read(source)
            self.assertTrue(changed)
            self.assertIn("return RESPONSE_TRY_AGAIN;", patched)
            self.assertIn("_content.position() < _content.size()", patched)

            patched_again, changed_again = PATCH_MODULE._patch_file_zero_read(patched)
            self.assertFalse(changed_again)
            self.assertEqual(patched, patched_again)

    def test_request_ack_timeout_recovery_is_bounded_and_idempotent(self):
        header, header_changed = PATCH_MODULE._patch_web_request_header_text(
            PATCH_MODULE._WEB_REQUEST_HEADER_TIMEOUT_RECOVERY
        )
        source, source_changed = PATCH_MODULE._patch_web_request_source_text(
            "\n".join((PATCH_MODULE._WEB_REQUEST_ON_POLL, PATCH_MODULE._WEB_REQUEST_ON_ACK, PATCH_MODULE._WEB_REQUEST_ON_TIMEOUT))
        )
        self.assertTrue(header_changed)
        self.assertTrue(source_changed)
        self.assertIn("bool _ackTimeoutRecoveryAttempted = false;", header)
        self.assertIn("if (len) {", source)
        self.assertIn("_ackTimeoutRecoveryAttempted = false;", source)
        self.assertIn("!_ackTimeoutRecoveryAttempted", source)
        self.assertIn("_response && !_response->_finished()", source)
        self.assertIn("_client && _client->canSend()", source)
        self.assertIn("_response->_ack(this, 0, 0) > 0", source)
        self.assertLess(
            source.index("_response->_ack(this, 0, 0) > 0"),
            source.index("_client->close();", source.index("void AsyncWebServerRequest::_onTimeout")),
        )

        header_again, header_changed_again = PATCH_MODULE._patch_web_request_header_text(header)
        source_again, source_changed_again = PATCH_MODULE._patch_web_request_source_text(source)
        self.assertFalse(header_changed_again)
        self.assertFalse(source_changed_again)
        self.assertEqual(header, header_again)
        self.assertEqual(source, source_again)

    def test_request_ack_timeout_recovery_refuses_unknown_source(self):
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_web_request_header_text("class Unknown {};")
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_web_request_source_text("void unrelated() {}")

    def test_zero_read_fix_refuses_unknown_source(self):
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_file_zero_read("size_t unrelated() { return 0; }")

    def test_send_progress_accounting_installs_and_is_idempotent(self):
        source = "\n".join((
            PATCH_MODULE._UPSTREAM_HEADER_CREDIT_ACCOUNTING,
            PATCH_MODULE._UPSTREAM_PENDING_BUFFER_ACCOUNTING,
            PATCH_MODULE._UPSTREAM_BODY_CREDIT_ACCOUNTING,
        ))

        patched, changed = PATCH_MODULE._patch_send_progress_accounting(source)
        self.assertTrue(changed)
        self.assertIn("payloadlen += added_len;\n        if (added_len !=", patched)
        self.assertIn("if (pcb_written) {", patched)
        self.assertIn("if (payloadlen) {", patched)
        self.assertNotIn("_in_flight += payloadlen;\n    --_in_flight_credit", patched)

        patched_again, changed_again = PATCH_MODULE._patch_send_progress_accounting(patched)
        self.assertFalse(changed_again)
        self.assertEqual(patched, patched_again)

    def test_send_progress_accounting_refuses_unknown_source(self):
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_send_progress_accounting("int unrelated() { return 0; }")

    def test_inflight_liveness_guard_installs_upgrades_and_is_idempotent(self):
        for source in (
            PATCH_MODULE._UPSTREAM_INFLIGHT_GUARD,
            PATCH_MODULE._LEGACY_INFLIGHT_GUARD,
        ):
            patched, changed = PATCH_MODULE._patch_inflight_liveness(source)
            self.assertTrue(changed)
            self.assertIn("hasPendingSendBuffer", patched)
            self.assertIn(
                "writable TCP space is authoritative liveness evidence", patched
            )
            self.assertIn(
                "if (!len && !_in_flight_credit && tcpWindow > 0)", patched
            )
            self.assertIn(
                "(!hasPendingSendBuffer && responseBufferCap > tcpWindow)", patched
            )

            patched_again, changed_again = PATCH_MODULE._patch_inflight_liveness(patched)
            self.assertFalse(changed_again)
            self.assertEqual(patched, patched_again)

    def test_inflight_liveness_guard_refuses_unknown_source(self):
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_inflight_liveness("int unrelated() { return 0; }")

    def test_final_buffer_completion_waits_for_tcp_drain(self):
        source = "\n".join((
            PATCH_MODULE._UPSTREAM_FINAL_BUFFER_DRAIN,
            PATCH_MODULE._UPSTREAM_EAGER_FINAL_COMPLETION,
        ))

        patched, changed = PATCH_MODULE._patch_final_buffer_completion(source)
        self.assertTrue(changed)
        self.assertIn("ERR_CONTENT_LENGTH_MISMATCH under memory pressure", patched)
        self.assertIn("if (_sendContentLength && _sentLength == _contentLength)", patched)
        self.assertNotIn(PATCH_MODULE._UPSTREAM_EAGER_FINAL_COMPLETION, patched)

        patched_again, changed_again = PATCH_MODULE._patch_final_buffer_completion(patched)
        self.assertFalse(changed_again)
        self.assertEqual(patched, patched_again)

    def test_final_buffer_completion_refuses_unknown_source(self):
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_final_buffer_completion("int unrelated() { return 0; }")

    def test_installed_312_copies_are_patched_when_available(self):
        libdeps = ROOT / ".pio" / "libdeps"
        if not libdeps.exists():
            self.skipTest("PlatformIO dependencies are not installed in this checkout")
        checked = 0
        for library_json in libdeps.glob("*/ESPAsyncWebServer/library.json"):
            metadata = json.loads(library_json.read_text(encoding="utf-8"))
            self.assertEqual("3.12.0", str(metadata.get("version")))
            checked += 1
            source_dir = library_json.parent / "src"
            header = (source_dir / "WebResponseImpl.h").read_text(encoding="utf-8")
            source = (source_dir / "WebResponses.cpp").read_text(encoding="utf-8")
            request_header = (source_dir / "ESPAsyncWebServer.h").read_text(encoding="utf-8")
            request_source = (source_dir / "WebRequest.cpp").read_text(encoding="utf-8")
            self.assertIn("FastBee patch: dynamic send buffer", header)
            self.assertIn("kFallbackSizes", source)
            self.assertIn("chunkCap", source)
            self.assertIn("transient zero-read before EOF", source)
            self.assertIn("return RESPONSE_TRY_AGAIN;", source)
            self.assertIn("responseBufferCap > tcpWindow", source)
            self.assertIn("hasPendingSendBuffer", source)
            self.assertIn("writable TCP space is authoritative liveness evidence", source)
            self.assertIn("zero progress must not consume the finite send credit", source)
            self.assertIn("account bytes accepted by AsyncTCP before a partial", source)
            self.assertIn("ERR_CONTENT_LENGTH_MISMATCH under memory pressure", source)
            self.assertNotIn(PATCH_MODULE._UPSTREAM_EAGER_FINAL_COMPLETION, source)
            self.assertNotIn("_send_buffer->data()", source)
            self.assertIn("bool _ackTimeoutRecoveryAttempted = false;", request_header)
            self.assertIn("_response->_ack(this, 0, 0) > 0", request_source)
        self.assertGreater(checked, 0, "expected at least one installed ESPAsyncWebServer 3.12.0 copy")

    def test_installed_async_tcp_350_copies_are_patched_when_available(self):
        libdeps = ROOT / ".pio" / "libdeps"
        if not libdeps.exists():
            self.skipTest("PlatformIO dependencies are not installed in this checkout")
        checked = 0
        for library_json in libdeps.glob("*/AsyncTCP/library.json"):
            metadata = json.loads(library_json.read_text(encoding="utf-8"))
            self.assertEqual("3.5.0", str(metadata.get("version")))
            checked += 1
            source_dir = library_json.parent / "src"
            header = (source_dir / "AsyncTCP.h").read_text(encoding="utf-8")
            source = (source_dir / "AsyncTCP.cpp").read_text(encoding="utf-8")
            self.assertIn("_fastbee_accepted_count", header)
            self.assertIn("_fastbee_server_accepted", header)
            self.assertIn("void _fastbee_release_accepted();", header)
            self.assertIn("FastBee patch: reject a connection before allocating AsyncClient", source)
            self.assertIn("CONFIG_ASYNC_TCP_MAX_CONNECTIONS", source)
            self.assertIn(
                "heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
                source,
            )
            self.assertIn(
                "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
                source,
            )
            self.assertIn("__atomic_exchange_n(", source)
            self.assertIn("client->_fastbee_release_accepted();", source)
            self.assertIn("c->_fastbee_release_accepted();", source)
            self.assertIn("delete c;", source)
            self.assertIn("FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE", source)
            self.assertIn("_fastbee_alloc_error_event(client)", source)
            self.assertIn("_fastbee_release_error_event(evpkt)", source)
        self.assertGreater(checked, 0, "expected at least one installed AsyncTCP 3.5.0 copy")


if __name__ == "__main__":
    unittest.main()
