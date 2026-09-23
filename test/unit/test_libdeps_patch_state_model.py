"""State-model checks for the bounded AsyncTCP/WebRequest recovery patches."""

from __future__ import annotations

import importlib.util
import pathlib
import re
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PATCH_SCRIPT = (ROOT / "scripts/apply-libdeps-patches.py").read_text(encoding="utf-8")
PLATFORMIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")
PATCH_SPEC = importlib.util.spec_from_file_location(
    "fastbee_apply_libdeps_patches_state_model",
    ROOT / "scripts/apply-libdeps-patches.py",
)
assert PATCH_SPEC and PATCH_SPEC.loader
PATCH_MODULE = importlib.util.module_from_spec(PATCH_SPEC)
sys.modules[PATCH_SPEC.name] = PATCH_MODULE
PATCH_SPEC.loader.exec_module(PATCH_MODULE)


class AckTimeoutModel:
    """Minimal model of the patched AsyncWebServerRequest timeout contract."""

    def __init__(self) -> None:
        self.recovery_attempted = False
        self.closed = False

    def ack(self, length: int) -> None:
        if length > 0:
            self.recovery_attempted = False

    def timeout(
        self,
        *,
        has_response: bool = True,
        finished: bool = False,
        has_client: bool = True,
        can_send: bool = True,
        queued_bytes: int = 0,
    ) -> str:
        if (
            not self.recovery_attempted
            and has_response
            and not finished
            and has_client
            and can_send
        ):
            self.recovery_attempted = True
            if queued_bytes > 0:
                return "recovered"

        if has_client:
            self.closed = True
        return "closed" if has_client else "no-client"


class ErrorEventPoolModel:
    """Fixed-slot ownership model used to exercise exhaustion and reuse."""

    def __init__(self, size: int) -> None:
        self.used = [False] * size
        self.exhausted = 0

    def allocate(self) -> int | None:
        for index, used in enumerate(self.used):
            if not used:
                self.used[index] = True
                return index
        self.exhausted += 1
        return None

    def release(self, index: int) -> bool:
        if index < 0 or index >= len(self.used) or not self.used[index]:
            return False
        self.used[index] = False
        return True


class LibdepsPatchStateModelTests(unittest.TestCase):
    def test_first_timeout_with_real_progress_recovers_once(self):
        model = AckTimeoutModel()

        self.assertEqual("recovered", model.timeout(queued_bytes=1436))
        self.assertTrue(model.recovery_attempted)
        self.assertFalse(model.closed)

        self.assertEqual("closed", model.timeout(queued_bytes=1436))
        self.assertTrue(model.closed)

    def test_real_ack_rearms_one_later_recovery(self):
        model = AckTimeoutModel()
        self.assertEqual("recovered", model.timeout(queued_bytes=512))

        model.ack(512)
        self.assertFalse(model.recovery_attempted)
        self.assertEqual("recovered", model.timeout(queued_bytes=512))

    def test_zero_progress_and_non_writable_states_fail_closed(self):
        cases = (
            {"queued_bytes": 0},
            {"can_send": False, "queued_bytes": 1436},
            {"finished": True, "queued_bytes": 1436},
            {"has_response": False, "queued_bytes": 1436},
        )
        for case in cases:
            with self.subTest(case=case):
                model = AckTimeoutModel()
                self.assertEqual("closed", model.timeout(**case))
                self.assertTrue(model.closed)

    def test_missing_client_does_not_dereference_or_claim_recovery(self):
        model = AckTimeoutModel()
        self.assertEqual(
            "no-client", model.timeout(has_client=False, queued_bytes=1436)
        )
        self.assertFalse(model.closed)

    def test_error_pool_is_ordered_bounded_and_reusable(self):
        pool = ErrorEventPoolModel(3)
        self.assertEqual([0, 1, 2], [pool.allocate() for _ in range(3)])
        self.assertIsNone(pool.allocate())
        self.assertEqual(1, pool.exhausted)

        self.assertTrue(pool.release(1))
        self.assertEqual(1, pool.allocate())
        self.assertFalse(pool.release(-1))
        self.assertFalse(pool.release(3))

    def test_error_pool_source_orders_cleanup_and_queue_wakeup(self):
        tcp_error = "\n".join(
            (
                PATCH_MODULE._ASYNCTCP_TCP_ERROR_PATCHED_POOLED,
                "  e->error.err = err;",
                "  queue_mutex_guard guard;",
                "  _send_async_event(e);",
            )
        )
        release_slot = tcp_error.index("client->_fastbee_release_accepted();")
        heap_alloc = tcp_error.index(
            "new (std::nothrow) lwip_tcp_event_packet_t{LWIP_TCP_ERROR, client}",
            release_slot,
        )
        fallback_alloc = tcp_error.index(
            "_fastbee_alloc_error_event(client)", heap_alloc
        )
        exhausted_return = tcp_error.index(
            "emergency pool exhausted", fallback_alloc
        )
        enqueue = tcp_error.index("_send_async_event(e);", exhausted_return)

        self.assertLess(release_slot, heap_alloc)
        self.assertLess(heap_alloc, fallback_alloc)
        self.assertLess(fallback_alloc, exhausted_return)
        self.assertLess(exhausted_return, enqueue)

        pool_release = PATCH_SCRIPT.index("_fastbee_release_error_event(evpkt)")
        delete_event = PATCH_SCRIPT.index("delete evpkt;", pool_release)
        self.assertLess(pool_release, delete_event)

        for source_path in (ROOT / ".pio" / "libdeps").glob(
            "*/AsyncTCP/src/AsyncTCP.cpp"
        ):
            source = source_path.read_text(encoding="utf-8")
            queue_push = source.index("_async_queue.push_back(e);")
            wake_task = source.index(
                "xTaskNotifyGive(_async_service_task_handle);", queue_push
            )
            self.assertLess(queue_push, wake_task, str(source_path))

    def test_error_pool_source_counts_exhaustion_and_uses_atomic_ownership(self):
        for marker in (
            "FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE 12U",
            "__atomic_compare_exchange_n(",
            "__ATOMIC_ACQ_REL",
            "_fastbee_error_event_pool_exhausted",
            "__atomic_fetch_add(",
            "__atomic_store_n(",
            "__ATOMIC_RELEASE",
        ):
            self.assertIn(marker, PATCH_SCRIPT)

    def test_error_pool_covers_every_configured_inbound_client_slot(self):
        pool_match = re.search(
            r"FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE\s+(\d+)U", PATCH_SCRIPT
        )
        self.assertIsNotNone(pool_match)
        connection_limits = [
            int(value)
            for value in re.findall(
                r"CONFIG_ASYNC_TCP_MAX_CONNECTIONS=(\d+)", PLATFORMIO
            )
        ]
        self.assertTrue(connection_limits)
        self.assertGreaterEqual(int(pool_match.group(1)), max(connection_limits))


if __name__ == "__main__":
    unittest.main()
