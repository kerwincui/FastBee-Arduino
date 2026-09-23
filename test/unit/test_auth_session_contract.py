"""Contracts for low-memory login execution and session probing."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "network" / "handlers" / "AuthRouteHandler.cpp").read_text(
    encoding="utf-8"
)
BROWSER_FIXTURE = (ROOT / "test" / "browser" / "fixtures" / "base.fixture.ts").read_text(
    encoding="utf-8"
)


class AuthSessionContractTests(unittest.TestCase):
    def test_measured_login_stack_is_transient_on_all_targets(self):
        self.assertIn("#if CHIP_DUAL_CORE", SOURCE)
        self.assertNotIn("LOGIN_TASK_STACK_BYTES = 8192U", SOURCE)
        self.assertIn("LOGIN_TASK_STACK_BYTES = 6144U", SOURCE)
        self.assertIn('xTaskCreate(loginTask, "fb_login", LOGIN_TASK_STACK_BYTES', SOURCE)
        self.assertNotIn("ensureC3LoginWorker", SOURCE)
        self.assertNotIn("c3LoginQueue", SOURCE)

    def test_login_has_contiguous_memory_admission_and_metrics(self):
        self.assertIn("FastBee::MemoryBudget::dram8Free()", SOURCE)
        self.assertIn("FastBee::MemoryBudget::dram8Largest()", SOURCE)
        self.assertIn("LOGIN_TASK_STACK_ALLOCATOR_OVERHEAD_BYTES = 256U", SOURCE)
        self.assertIn("LOGIN_TASK_MIN_REMAINING_DRAM_BYTES = 12288U", SOURCE)
        self.assertIn("largestBlock < requiredBlock || freeDram < requiredFree", SOURCE)
        self.assertIn("Login needs largest>=%lu and free>=%lu DRAM bytes", SOURCE)
        self.assertNotIn("LOGIN_TASK_STACK_HEADROOM_BYTES", SOURCE)
        self.assertNotIn("xTaskCreatePinnedToCoreWithCaps", SOURCE)
        self.assertNotIn("vTaskDeleteWithCaps(nullptr)", SOURCE)
        self.assertIn("uxTaskGetStackHighWaterMark(nullptr)", SOURCE)
        self.assertIn("freeStackBytes < LOGIN_TASK_STACK_BYTES", SOURCE)
        self.assertIn("[LOGIN_METRICS]", SOURCE)

    def test_fragmented_internal_dram_is_rejected_safely(self):
        stack_bytes = 6144
        allocator_overhead = 256
        minimum_remaining_dram = 12288
        required_block = stack_bytes + allocator_overhead
        internal_largest = 6000
        internal_free = 24224
        required_free = minimum_remaining_dram + required_block
        admitted = internal_largest >= required_block and internal_free >= required_free
        self.assertFalse(admitted)
        self.assertLess(internal_largest, required_block)

    def test_login_handles_scheduler_allocation_and_pause_order(self):
        self.assertIn("if (!slot)", SOURCE)
        self.assertIn("job->slot = slot;", SOURCE)
        self.assertIn("xSemaphoreGive(job->slot);", SOURCE)
        create = SOURCE.index("ok = xTaskCreate", SOURCE.index("void AuthRouteHandler::handleLogin"))
        pause = SOURCE.index("job->held = request->pause()", create)
        notify = SOURCE.index("xTaskNotifyGive(taskHandle)", pause)
        self.assertLess(create, pause)
        self.assertLess(pause, notify)
        self.assertIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)", SOURCE)

    def test_login_remains_off_async_tcp_task(self):
        self.assertIn('xTaskCreatePinnedToCore(loginTask, "fb_login", LOGIN_TASK_STACK_BYTES', SOURCE)
        self.assertIn('xTaskCreate(loginTask, "fb_login", LOGIN_TASK_STACK_BYTES', SOURCE)

    def test_invalid_session_copies_username_before_erasing_map_entry(self):
        auth_source = (ROOT / "src" / "security" / "AuthManager.cpp").read_text(
            encoding="utf-8"
        )
        start = auth_source.index("SessionStatus status = validateSession(session);")
        end = auth_source.index("result.success = true;", start)
        block = auth_source[start:end]
        copy_pos = block.index("const String invalidUsername = session.username;")
        erase_pos = block.index("activeSessions.erase(it);")
        log_pos = block.index('logAuthEvent("session_invalid", invalidUsername')
        self.assertLess(copy_pos, erase_pos)
        self.assertLess(erase_pos, log_pos)

    def test_missing_session_is_normal_probe_result(self):
        start = SOURCE.index("void AuthRouteHandler::handleVerifySession")
        end = SOURCE.index("void AuthRouteHandler::handleChangePassword", start)
        block = SOURCE[start:end]
        self.assertIn('responseDoc["sessionValid"] = false;', block)
        self.assertIn("ctx->sendSuccess(request, responseDoc);", block)
        self.assertNotIn("ctx->sendUnauthorized(request);", block)

    def test_browser_health_checks_do_not_bypass_cached_session_restore(self):
        start = BROWSER_FIXTURE.index("if (cachedAuthState) {")
        end = BROWSER_FIXTURE.index("// 缓存未命中或失效", start)
        block = BROWSER_FIXTURE[start:end]
        restore_pos = block.index("authenticated = await restoreAuthState(page);")
        health_pos = block.index("Date.now() - lastFullHealthCheck")
        self.assertLess(restore_pos, health_pos)
        self.assertNotIn("需要完整检查，不用快速恢复", block)

    def test_browser_login_retries_follow_server_retry_after(self):
        self.assertIn("class RetryableLoginError extends Error", BROWSER_FIXTURE)
        self.assertIn("diagnostic.retryAfter", BROWSER_FIXTURE)
        self.assertIn(
            "e instanceof RetryableLoginError ? e.retryAfterMs : 3000",
            BROWSER_FIXTURE,
        )

    def test_browser_login_failure_diagnostics_are_attached_and_redacted(self):
        self.assertIn("captureHttpFailureDiagnostic(", BROWSER_FIXTURE)
        self.assertIn("testInfo.attach(", BROWSER_FIXTURE)
        self.assertIn("sanitizeHttpDiagnosticBody", BROWSER_FIXTURE)
        self.assertIn("SENSITIVE_DIAGNOSTIC_KEY", BROWSER_FIXTURE)
        self.assertIn("Request bodies and auth headers are intentionally excluded", BROWSER_FIXTURE)
        self.assertNotIn("postData()", BROWSER_FIXTURE)


if __name__ == "__main__":
    unittest.main()
