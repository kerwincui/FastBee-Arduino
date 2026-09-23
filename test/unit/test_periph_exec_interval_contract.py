"""Contracts for PeriphExec timer/poll interval persistence consistency."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANAGER_H = (ROOT / "include/core/PeriphExecManager.h").read_text(encoding="utf-8")
MANAGER_CPP = (ROOT / "src/core/PeriphExecManager.cpp").read_text(encoding="utf-8")
SCHEDULER_H = (ROOT / "include/core/PeriphExecScheduler.h").read_text(encoding="utf-8")
SCHEDULER_CPP = (ROOT / "src/core/PeriphExecScheduler.cpp").read_text(encoding="utf-8")
ROUTE_CPP = (
    ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp"
).read_text(encoding="utf-8")
FORM_JS = (
    ROOT / "web-src/modules/runtime/periph-exec-form.js"
).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    match = re.search(signature + r"[^;{]*\{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function matching {signature!r}")
    opening = source.find("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated function matching {signature!r}")


class PeriphExecIntervalContractTests(unittest.TestCase):
    def test_manager_owns_timer_and_poll_canonical_ranges(self):
        self.assertIn("PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC = 1", MANAGER_CPP)
        self.assertIn("PERIPH_EXEC_MIN_POLL_INTERVAL_SEC = 5", MANAGER_CPP)
        self.assertIn("PERIPH_EXEC_MAX_INTERVAL_SEC = 86400UL", MANAGER_CPP)

        sanitize = function_body(
            MANAGER_CPP,
            r"\bPeriphExecManager::sanitizeTriggerForSafety\s*\(",
        )
        self.assertIn("ExecTriggerType::TIMER_TRIGGER", sanitize)
        self.assertIn("PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC", sanitize)
        self.assertIn("ExecTriggerType::POLL_TRIGGER", sanitize)
        self.assertIn("PERIPH_EXEC_MIN_POLL_INTERVAL_SEC", sanitize)
        self.assertGreaterEqual(sanitize.count("PERIPH_EXEC_MAX_INTERVAL_SEC"), 4)
        self.assertIn("return modified", sanitize)
        self.assertRegex(
            MANAGER_H,
            r"bool\s+sanitizeTriggerForSafety\s*\(",
        )

    def test_save_and_load_publish_only_canonical_intervals(self):
        save = function_body(
            MANAGER_CPP, r"\bPeriphExecManager::saveConfigurationLocked\s*\("
        )
        self.assertIn("writeConfigurationLocked(rules, _irKeysJson)", save)
        # Saving must not mutate unrelated rules before an atomic commit fails.
        self.assertNotIn("sanitizeRuleForSafety(pair.second)", save)
        for method, variable in (("addRuleLocked", "newRule"), ("updateRuleLocked", "updated")):
            mutation = function_body(MANAGER_CPP, rf"\bPeriphExecManager::{method}\s*\(")
            self.assertIn(f"sanitizeRuleForSafety({variable})", mutation)

        decode = function_body(MANAGER_CPP, r"\bdecodeConfigRule\s*\(")
        self.assertIn('t.intervalSec = tObj["intervalSec"] | uint32_t(60)', decode)
        load = function_body(
            MANAGER_CPP, r"\bPeriphExecManager::loadConfiguration\s*\("
        )
        self.assertLess(load.index("decodeConfigRule"), load.index("sanitizeRuleForSafety(r)"))
        self.assertLess(load.index("sanitizeRuleForSafety(r)"), load.index("candidate->emplace"))
        self.assertIn("normalizedLoadedConfig", load)
        self.assertIn("normalizedLoadedConfig || migratedLegacyIrKeys", load)
        self.assertLess(load.index("writeConfigurationLocked(candidate, candidateIr)"),
                        load.index("rules.swap(candidate)"))
        self.assertNotIn("rules.clear()", load)

    def test_scheduler_is_diagnostic_only(self):
        validate = function_body(
            SCHEDULER_CPP,
            r"\bPeriphExecScheduler::validateLoadedConfig\s*\(",
        )
        self.assertIn("const auto& rulesMap", validate)
        self.assertIn("MIN_TIMER_INTERVAL_SEC", validate)
        self.assertIn("MIN_POLL_INTERVAL_SEC", validate)
        self.assertIn("MAX_INTERVAL_SEC", validate)
        self.assertIn("Aggressive %s interval", validate)
        self.assertNotIn("trigger.intervalSec =", validate)
        self.assertNotIn("runtime only", validate)
        self.assertNotIn("adjusted to 30s", validate)
        self.assertNotIn("SAFE_POLL_INTERVAL", SCHEDULER_H + SCHEDULER_CPP)

    def test_route_preflight_and_parsing_use_the_same_ranges(self):
        self.assertIn("PERIPH_EXEC_TIMER_INTERVAL_MIN_SEC = 1", ROUTE_CPP)
        self.assertIn("PERIPH_EXEC_POLL_INTERVAL_MIN_SEC = 5", ROUTE_CPP)
        self.assertIn("PERIPH_EXEC_INTERVAL_MAX_SEC = 86400UL", ROUTE_CPP)
        self.assertIn("intervalSecondsFromInput", ROUTE_CPP)
        self.assertIn("value < 0 ? 0U", ROUTE_CPP)
        self.assertIn('addIssue("INVALID_TIMER_INTERVAL"', ROUTE_CPP)
        self.assertIn('addIssue("INVALID_POLL_INTERVAL"', ROUTE_CPP)
        self.assertIn("Poll interval must be between 5 and 86400 seconds", ROUTE_CPP)
        self.assertIn('tObj["intervalSec"] = t.intervalSec', ROUTE_CPP)

    def test_web_form_rejects_values_the_firmware_would_normalize(self):
        self.assertRegex(
            FORM_JS,
            r'class="pe-poll-interval"[^>]*min="5"[^>]*max="86400"',
        )
        self.assertRegex(
            FORM_JS,
            r'class="pe-interval"[^>]*min="1"[^>]*max="86400"',
        )
        self.assertIn("trigger.intervalSec < 1 || trigger.intervalSec > 86400", FORM_JS)
        self.assertIn("trigger.intervalSec < 5 || trigger.intervalSec > 86400", FORM_JS)
        self.assertIn("pef-err-timer-interval", FORM_JS)
        self.assertIn("pef-err-poll-interval", FORM_JS)

    def test_timer_round_robin_cursor_advances_only_after_admission(self):
        dispatch = function_body(
            MANAGER_CPP,
            r"\bPeriphExecManager::checkTimerTriggers\s*\(",
        )
        loop_start = dispatch.index(
            "for (size_t offset = 0; offset < candidates.size(); ++offset)"
        )
        accepted = dispatch.index(
            "const bool accepted = dispatchByRuleId", loop_start
        )
        rejected = dispatch.index("if (!accepted)", accepted)
        rejection_continue = dispatch.index("continue;", rejected)
        cursor_update = dispatch.index("_lastTimerDispatchRuleId = c.id", accepted)
        commit = dispatch.index("lastTriggerTime = now", cursor_update)

        self.assertLess(rejection_continue, cursor_update)
        self.assertLess(cursor_update, commit)
        self.assertEqual(
            dispatch.count("_lastTimerDispatchRuleId = c.id"),
            1,
            "rejected or skipped candidates must not consume the fairness cursor",
        )


if __name__ == "__main__":
    unittest.main()
