"""Contracts for transactional execution-rule mutations and route usage."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANAGER_H = (ROOT / "include/core/PeriphExecManager.h").read_text(encoding="utf-8")
MANAGER_CPP = (ROOT / "src/core/PeriphExecManager.cpp").read_text(encoding="utf-8")
MANAGER_CPP += '\n' + (ROOT / "src/core/PeriphExecConfigStore.cpp").read_text(encoding="utf-8")
ROUTE_CPP = (
    ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp"
).read_text(encoding="utf-8")
EXECUTOR_CPP = (ROOT / "src/core/PeriphExecExecutor.cpp").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    match = re.search(signature + r"[^;{]*\{", source, re.S)
    if match is None:
        raise AssertionError(f"missing C++ function matching {signature!r}")
    opening = source.find("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated C++ function matching {signature!r}")


TRANSACTIONAL_METHODS = {
    "addRuleAndSave": "ADD",
    "updateRuleAndSave": "UPDATE",
    "removeRuleAndSave": "REMOVE",
    "enableRuleAndSave": "ENABLE",
    "disableRuleAndSave": "DISABLE",
}

ROUTE_METHODS = {
    "handleAddRule": "addRuleAndSave",
    "handleAddRuleJson": "addRuleAndSave",
    "handleUpdateRule": "updateRuleAndSave",
    "handleUpdateRuleJson": "updateRuleAndSave",
    "handleDeleteRule": "removeRuleAndSave",
    "handleEnableRule": "enableRuleAndSave",
    "handleDisableRule": "disableRuleAndSave",
}


class PeriphExecTransactionContractTests(unittest.TestCase):
    def test_manager_declares_all_transactional_rule_mutations(self):
        for method in TRANSACTIONAL_METHODS:
            self.assertRegex(
                MANAGER_H,
                rf"bool\s+{method}\s*\([^;]*String\s*&\s*\w+[^;]*\)\s*;",
            )
        self.assertIn("applyRuleMutationTransactional", MANAGER_H)
        self.assertIn("restoreRuleMutationLocked", MANAGER_H)
        self.assertIn("saveConfigurationLocked", MANAGER_H)

    def test_each_wrapper_uses_the_shared_transaction_with_the_right_operation(self):
        for method, operation in TRANSACTIONAL_METHODS.items():
            body = function_body(
                MANAGER_CPP, rf"\bPeriphExecManager::{method}\s*\("
            )
            self.assertIn("applyRuleMutationTransactional", body)
            self.assertIn(f"RuleMutationType::{operation}", body)

    def test_transaction_holds_one_lock_across_mutation_save_and_rollback(self):
        body = function_body(
            MANAGER_CPP,
            r"\bPeriphExecManager::applyRuleMutationTransactional\s*\(",
        )
        lock = body.index("MutexGuard lock(_rulesMutex")
        snapshot = body.index("RuleMutationSnapshot snapshot")
        mutation = body.index("switch (operation)")
        save = body.index("saveConfigurationLocked()")
        publish = body.index("refreshRuleRuntimeIndexesLocked(false)")
        rollback = body.index("restoreRuleMutationLocked")
        self.assertLess(lock, snapshot)
        self.assertLess(snapshot, mutation)
        self.assertLess(mutation, save)
        self.assertLess(save, publish)
        self.assertLess(publish, rollback)
        self.assertLess(save, rollback)
        self.assertNotIn("saveConfiguration()", body)

    def test_transaction_lock_wait_is_bounded_for_web_requests(self):
        body = function_body(
            MANAGER_CPP,
            r"\bPeriphExecManager::applyRuleMutationTransactional\s*\(",
        )
        self.assertIn("PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS = 2000", MANAGER_CPP)
        self.assertIn(
            "pdMS_TO_TICKS(PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS)", body
        )
        self.assertIn("RULE_SERVICE_BUSY", body)

    def test_persistence_failure_has_distinct_rollback_outcomes(self):
        body = function_body(
            MANAGER_CPP,
            r"\bPeriphExecManager::applyRuleMutationTransactional\s*\(",
        )
        self.assertIn("PERSISTENCE_ROLLBACK_OK", body)
        self.assertIn("ROLLBACK_FAILED", body)
        self.assertIn("if (restored)", body)

    def test_rollback_restores_rule_and_rebuilds_runtime_indexes(self):
        restore = function_body(
            MANAGER_CPP, r"\bPeriphExecManager::restoreRuleMutationLocked\s*\("
        )
        refresh = function_body(
            MANAGER_CPP,
            r"\bPeriphExecManager::refreshRuleRuntimeIndexesLocked\s*\(",
        )
        self.assertIn("existing->second = snapshot.rule", restore)
        self.assertIn("rules.erase(resultId)", restore)
        self.assertIn("rules[snapshot.id] = snapshot.rule", restore)
        self.assertIn("rulesMatch(it->second, snapshot.rule)", restore)
        self.assertIn("refreshRuleRuntimeIndexesLocked(false)", restore)
        self.assertIn("rebuildButtonEventCache()", refresh)
        self.assertIn("rebuildDataSourceCache()", refresh)
        self.assertIn("_scheduler->validateLoadedConfig()", refresh)

    def test_rollback_verifies_every_persisted_rule_field(self):
        compare = function_body(MANAGER_CPP, r"\brulesMatch\s*\(")
        trigger_compare = function_body(MANAGER_CPP, r"\btriggersMatch\s*\(")
        action_compare = function_body(MANAGER_CPP, r"\bactionsMatch\s*\(")
        for field in (
            "id",
            "name",
            "enabled",
            "execMode",
            "protocolType",
            "scriptContent",
            "reportAfterExec",
            "triggers",
            "actions",
        ):
            self.assertIn(field, compare)
        for field in (
            "triggerType",
            "triggerPeriphId",
            "operatorType",
            "compareValue",
            "timerMode",
            "intervalSec",
            "timePoint",
            "eventId",
            "pollResponseTimeout",
            "pollMaxRetries",
            "pollInterPollDelay",
        ):
            self.assertIn(field, trigger_compare)
        for field in (
            "targetPeriphId",
            "actionType",
            "actionValue",
            "useReceivedValue",
            "syncDelayMs",
            "execMode",
        ):
            self.assertIn(field, action_compare)

    def test_public_save_delegates_to_non_locking_helper(self):
        public_save = function_body(
            MANAGER_CPP, r"\bPeriphExecManager::saveConfiguration\s*\("
        )
        locked_save = function_body(
            MANAGER_CPP, r"\bPeriphExecManager::saveConfigurationLocked\s*\("
        )
        self.assertIn("MutexGuard lock(_rulesMutex)", public_save)
        self.assertIn("saveConfigurationLocked()", public_save)
        self.assertNotIn("MutexGuard lock(_rulesMutex)", locked_save)
        self.assertIn("writeConfigurationLocked(rules, _irKeysJson)", locked_save)
        writer = function_body(
            MANAGER_CPP, r"\bPeriphExecManager::writeConfigurationLocked\s*\("
        )
        self.assertIn("FileUtils::commitAtomicWrite", writer)
        self.assertLess(writer.index("validateRuleProfileLimits"), writer.index("FileUtils::openAtomicWrite"))

    def test_all_crud_and_toggle_routes_use_transactional_methods(self):
        legacy_calls = {
            "addRuleAndSave": "addRule(",
            "updateRuleAndSave": "updateRule(",
            "removeRuleAndSave": "removeRule(",
            "enableRuleAndSave": "enableRule(",
            "disableRuleAndSave": "disableRule(",
        }
        for handler, transactional_method in ROUTE_METHODS.items():
            body = function_body(
                ROUTE_CPP, rf"\bPeriphExecRouteHandler::{handler}\s*\("
            )
            self.assertIn(transactional_method, body)
            self.assertNotIn("saveConfiguration()", body)
            self.assertNotIn(legacy_calls[transactional_method], body)

    def test_route_preserves_transaction_result_codes(self):
        status = function_body(ROUTE_CPP, r"\bruleMutationErrorStatus\s*\(")
        message = function_body(ROUTE_CPP, r"\bruleMutationErrorMessage\s*\(")
        for code in ("PERSISTENCE_ROLLBACK_OK", "ROLLBACK_FAILED"):
            self.assertIn(code, ROUTE_CPP)
        self.assertIn("isRulePersistenceFailure", status)
        self.assertIn("return 500", status)
        self.assertIn("errorMsg", message)
        self.assertIn("buildFlashSaveErrorMessage", message)

    def test_rule_control_actions_use_transactional_toggle(self):
        body = function_body(
            EXECUTOR_CPP,
            r"\bPeriphExecExecutor::executeRuleControlAction\s*\(",
        )
        self.assertIn("enableRuleAndSave", body)
        self.assertIn("disableRuleAndSave", body)
        self.assertNotIn("saveConfiguration()", body)
        self.assertNotRegex(body, r"->(?:enableRule|disableRule)\s*\(")


if __name__ == "__main__":
    unittest.main()
