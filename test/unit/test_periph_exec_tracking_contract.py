"""Source contracts for traceable peripheral-execution operations."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
TYPES = (ROOT / "include/core/AsyncExecTypes.h").read_text(encoding="utf-8")
MANAGER = (ROOT / "src/core/PeriphExecManager.cpp").read_text(encoding="utf-8")
EXECUTOR = (ROOT / "src/core/PeriphExecExecutor.cpp").read_text(encoding="utf-8")
ROUTE = (ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp").read_text(
    encoding="utf-8"
)
BROWSER_E2E = (ROOT / "test/browser/suites/06-periph-exec.spec.ts").read_text(
    encoding="utf-8"
)


def braced_block(source: str, marker: str) -> str:
    marker_at = source.find(marker)
    if marker_at < 0:
        raise AssertionError(f"missing source marker {marker!r}")
    opening = source.find("{", marker_at + len(marker))
    if opening < 0:
        raise AssertionError(f"missing block after {marker!r}")

    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated block after {marker!r}")


class PeriphExecTrackingContractTests(unittest.TestCase):
    def test_result_model_has_bounded_operation_and_action_lifecycle(self):
        for token in (
            "uint32_t         operationId",
            "unsigned long    queuedAt",
            "unsigned long    startTime",
            "unsigned long    endTime",
            "unsigned long    deadlineAt",
            "AsyncExecStatus::PENDING",
            "AsyncExecActionTrace actionTraces[ASYNC_EXEC_ACTION_TRACE_LIMIT]",
            "ASYNC_EXEC_ACTION_TRACE_LIMIT = MAX_ACTIONS_PER_RULE",
            "ASYNC_EXEC_RESULT_HISTORY_LIMIT = MAX_ASYNC_TASKS * 2",
            "ASYNC_EXEC_OPERATION_TIMEOUT_MS = 30000UL",
            "TIMED_OUT",
            "REJECTED",
        ):
            self.assertIn(token, TYPES)
        self.assertNotIn("std::vector<AsyncExecActionTrace>", TYPES)

    def test_admission_record_precedes_worker_queue_handoff(self):
        dispatch = braced_block(
            MANAGER, "bool PeriphExecManager::dispatchAsync(const PeriphExecRule& rule"
        )
        begin_at = dispatch.index("beginOperation(rule, true, queuePosition, operation, uartOrigin, capturedControl)")
        copy_at = dispatch.index("ctx->operationId = operation.operationId", begin_at)
        enqueue_at = dispatch.index("_workerPool->enqueue(ctx)", copy_at)
        accepted_at = dispatch.index("*admittedOperation = operation", enqueue_at)
        self.assertLess(begin_at, copy_at)
        self.assertLess(copy_at, enqueue_at)
        self.assertLess(dispatch.index("ctx->ordinaryControl = operation.ordinaryControl"), enqueue_at)
        self.assertLess(enqueue_at, accepted_at)
        self.assertIn('"tracking_unavailable"', dispatch)

    def test_existing_operation_updates_are_not_dropped_under_low_memory(self):
        record = braced_block(
            MANAGER, "bool PeriphExecManager::recordResult(const AsyncExecResult& result)"
        )
        lookup_at = record.index("existing.operationId == result.operationId")
        update_at = record.index("existing = result", lookup_at)
        # Result storage requires byte-addressable DRAM; updates must still
        # reach existing records before the new-record admission guard.
        memory_gate_at = record.index("dram8Free() < 12000", update_at)
        self.assertNotIn("dramFree() < 12000", record)
        self.assertLess(lookup_at, update_at)
        self.assertLess(update_at, memory_gate_at)
        self.assertIn("isTerminalExecStatus(it->status)", record)
        self.assertIn("if (evict == executionResults.end()) return false", record)

    def test_worker_publishes_running_and_exactly_one_terminal_update(self):
        worker = braced_block(
            MANAGER, "void PeriphExecManager::executeWorkerJob(AsyncExecContext* ctx)"
        )
        running_at = worker.index("result.status = AsyncExecStatus::RUNNING")
        running_record_at = worker.index("recordResult(result)", running_at)
        memory_gate_at = worker.index("dramFree() < 16384", running_record_at)
        ended_at = worker.index("result.endTime = millis()", memory_gate_at)
        terminal_record_at = worker.index("recordResult(result)", ended_at)
        self.assertLess(running_at, running_record_at)
        self.assertLess(running_record_at, memory_gate_at)
        self.assertLess(ended_at, terminal_record_at)
        self.assertEqual(worker.count("recordResult(result)"), 2)
        self.assertIn("AsyncExecStatus::TIMED_OUT", worker)
        self.assertIn("AsyncExecError::LOW_MEMORY", worker)

    def test_executor_captures_one_bounded_trace_per_configured_action(self):
        execute = braced_block(
            EXECUTOR,
            "std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(",
        )
        self.assertIn("actionIndex < rule.actions.size()", execute)
        self.assertIn("appendActionTrace(traceOut, actionIndex", execute)
        self.assertIn("safeActionTraceValue(", execute)
        self.assertIn("operationDeadlineReached(deadlineAt)", execute)
        self.assertIn("AsyncExecError::DEADLINE_EXCEEDED", execute)

    def test_run_api_returns_admission_metadata_and_results_expose_lifecycle(self):
        run = braced_block(
            ROUTE,
            "void PeriphExecRouteHandler::handleRunOnce(AsyncWebServerRequest* request)",
        )
        accepted = braced_block(run, "if (submitted)")
        for field in ("operationId", "queuedAt", "deadlineAt", "queuePosition"):
            self.assertIn(
                f'FastBee::setFixedJsonField(doc, "{field}", operation.{field});', accepted
            )
        self.assertIn(
            'FastBee::setFixedJsonField(doc, "state", asyncExecStatusName(operation.status));', accepted
        )
        for token in (
            "operation.asyncExecution && !asyncExecStatusIsTerminal(operation.status) ? 202 : 200",
            "runOnceRejectionStatus(rejectionReason)",
        ):
            self.assertIn(token, run)

        results = braced_block(
            ROUTE,
            "void PeriphExecRouteHandler::handleGetRecentResults(AsyncWebServerRequest* request)",
        )
        for field in (
            r'\"operationId\"',
            r'\"state\"',
            r'\"terminal\"',
            r'\"queuedAt\"',
            r'\"startedAt\"',
            r'\"endedAt\"',
            r'\"deadlineAt\"',
            r'\"actions\"',
        ):
            self.assertIn(field, results)
        self.assertIn('request->hasParam("operationId")', results)

    def test_device_e2e_tracks_the_exact_operation_id(self):
        helper_start = BROWSER_E2E.index("async function runRuleAndWait(")
        helper_end = BROWSER_E2E.index("/** 读取 GPIO", helper_start)
        helper = BROWSER_E2E[helper_start:helper_end]
        self.assertIn("const operationId = Number", helper)
        self.assertIn("results?operationId=${encodeURIComponent", helper)
        self.assertIn("it.terminal === true", helper)
        self.assertNotIn("baseline", helper)


if __name__ == "__main__":
    unittest.main()
