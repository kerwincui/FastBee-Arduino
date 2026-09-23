"""Source contracts for transactional peripheral configuration mutations."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANAGER_H = (ROOT / "include/core/PeripheralManager.h").read_text(encoding="utf-8")
MANAGER_CPP = "\n".join(
    (ROOT / path).read_text(encoding="utf-8")
    for path in (
        "src/core/PeripheralManager.cpp",
        "src/core/PeripheralManagerPersistence.cpp",
    )
)
ROUTE_CPP = (
    ROOT / "src/network/handlers/PeripheralRouteHandler.cpp"
).read_text(encoding="utf-8")


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


MUTATIONS = {
    "addPeripheralAndSave": "addPeripheral",
    "updatePeripheralAndSave": "updatePeripheral",
    "removePeripheralAndSave": "removePeripheral",
    "enablePeripheralAndSave": "enablePeripheral",
    "disablePeripheralAndSave": "disablePeripheral",
}


class PeripheralTransactionContractTests(unittest.TestCase):
    def test_manager_declares_all_transactional_mutations(self):
        for wrapper in MUTATIONS:
            self.assertRegex(
                MANAGER_H,
                rf"bool\s+{wrapper}\s*\([^;]*String\s*&\s*\w+[^;]*\)\s*;",
                f"{wrapper} must return a persistence-aware error",
            )
        self.assertIn("applyMutationTransactional", MANAGER_H)
        self.assertIn("restoreMutationSnapshot", MANAGER_H)

    def test_every_and_save_wrapper_uses_the_shared_transaction(self):
        for wrapper, mutation in MUTATIONS.items():
            body = function_body(
                MANAGER_CPP, rf"\bPeripheralManager::{wrapper}\s*\("
            )
            self.assertIn(
                "applyMutationTransactional",
                body,
                f"{wrapper} must use the common rollback boundary",
            )
            self.assertRegex(
                body,
                rf"(?<!AndSave)\b{mutation}\s*\(",
                f"{wrapper} must invoke {mutation} inside its transaction",
            )

    def test_flash_failure_restores_configuration_and_runtime_state(self):
        transaction = function_body(
            MANAGER_CPP, r"\bPeripheralManager::applyMutationTransactional\s*\("
        )
        restore = function_body(
            MANAGER_CPP, r"\bPeripheralManager::restoreMutationSnapshot\s*\("
        )

        for token in ("peripherals", "runtimeStates"):
            self.assertIn(token, transaction)
            self.assertIn(token, restore)
        self.assertIn("saveConfiguration()", transaction)
        self.assertIn("restoreMutationSnapshot", transaction)
        self.assertIn("PERSISTENCE_", transaction)

        mutation_call = re.search(r"\bmutation\s*\(", transaction)
        save_call = transaction.find("saveConfiguration()")
        restore_call = transaction.find("restoreMutationSnapshot")
        self.assertIsNotNone(mutation_call, "transaction must execute the requested mutation")
        self.assertLess(mutation_call.start(), save_call)
        self.assertLess(save_call, restore_call)

    def test_enable_init_failure_restores_the_complete_runtime_snapshot(self):
        enable = function_body(
            MANAGER_CPP, r"\bPeripheralManager::enablePeripheral\s*\("
        )
        init_failure = enable[enable.index("if (!initHardware(id))") :]

        for token in (
            "previousConfig",
            "previousState",
            "previousMemorySuspendedCount",
            "teardownHardware",
            "runtimeStates",
            "rebuildPinMapping",
        ):
            self.assertIn(token, init_failure)
        self.assertLess(
            init_failure.index("teardownHardware"),
            init_failure.index("*config = previousConfig"),
        )

    def test_update_does_not_claim_rollback_when_reinitialization_fails(self):
        update = function_body(
            MANAGER_CPP,
            r"\bPeripheralManager::updatePeripheral\s*\(\s*const String&\s+id\s*,\s*const PeripheralConfig&\s+config\s*,\s*String&",
        )
        self.assertIn("rollbackRestored", update)
        self.assertIn("ROLLBACK_FAILED", update)
        self.assertIn("HARDWARE_INIT_FAILED", update)
        self.assertLess(update.index("if (!rollbackRestored)"), update.index("ROLLBACK_FAILED"))

    def test_persistence_error_reports_the_actual_rollback_result(self):
        transaction = function_body(
            MANAGER_CPP, r"\bPeripheralManager::applyMutationTransactional\s*\("
        )
        self.assertIn("if (restored)", transaction)
        self.assertIn("运行态已回滚", transaction)
        self.assertIn("运行态回滚失败", transaction)
        self.assertIn("ROLLBACK_FAILED", transaction)

    def test_route_maps_rollback_failure_to_server_error(self):
        status_mapper = function_body(ROUTE_CPP, r"\bmutationErrorStatus\s*\(")
        self.assertIn('startsWith("ROLLBACK_FAILED")', status_mapper)
        self.assertIn("return 500", status_mapper)

    def test_routes_never_split_mutation_from_persistence(self):
        handlers = {
            "handleAddPeripheral": "addPeripheralAndSave",
            "handleUpdatePeripheral": "updatePeripheralAndSave",
            "handleDeletePeripheral": "removePeripheralAndSave",
            "handleEnablePeripheral": "enablePeripheralAndSave",
            "handleDisablePeripheral": "disablePeripheralAndSave",
        }
        for handler, wrapper in handlers.items():
            body = function_body(
                ROUTE_CPP, rf"\bPeripheralRouteHandler::{handler}\s*\("
            )
            self.assertIn(wrapper, body)
            self.assertNotIn(
                "saveConfiguration()",
                body,
                f"{handler} must not expose an in-memory success before persistence",
            )

    def test_save_configuration_holds_the_manager_lock_and_checks_overflow(self):
        save = function_body(
            MANAGER_CPP, r"\bPeripheralManager::saveConfiguration\s*\("
        )
        self.assertRegex(save, r"RecursiveMutexGuard\s+\w+\s*\(\s*_mutex\s*\)")
        self.assertRegex(save, r"\bdoc\s*\.\s*overflowed\s*\(\s*\)")

        guard = re.search(r"RecursiveMutexGuard", save)
        traversal = save.find("for (const auto& pair : peripherals)")
        overflow = re.search(r"\bdoc\s*\.\s*overflowed\s*\(\s*\)", save)
        self.assertIsNotNone(guard)
        self.assertGreaterEqual(traversal, 0)
        self.assertIsNotNone(overflow)
        self.assertLess(guard.start(), traversal)
        self.assertLess(traversal, overflow.start())

    def test_save_configuration_streams_to_an_atomic_temporary_file(self):
        save = function_body(
            MANAGER_CPP, r"\bPeripheralManager::saveConfiguration\s*\("
        )
        required = (
            "measureJson(doc)",
            "FileUtils::openAtomicWrite",
            "FileUtils::commitAtomicWrite",
        )
        for token in required:
            self.assertIn(token, save)
        output_match = re.search(
            r"File\s+(?P<name>\w+)\s*=\s*FileUtils::openAtomicWrite", save
        )
        self.assertIsNotNone(output_match)
        output = output_match.group("name")
        self.assertRegex(save, rf"serializeJson\s*\(\s*doc\s*,\s*{output}\s*\)")
        self.assertRegex(save, rf"\b{output}\s*\.\s*flush\s*\(\s*\)")
        self.assertRegex(save, rf"\b{output}\s*\.\s*close\s*\(\s*\)")
        self.assertRegex(save, r"\bwritten\s*!=\s*expected\b")
        self.assertNotRegex(save, r"serializeJson\s*\(\s*doc\s*,\s*jsonStr\s*\)")
        self.assertNotIn("FileUtils::atomicWriteFile", save)

        overflow = re.search(r"\bdoc\s*\.\s*overflowed\s*\(\s*\)", save)
        opened = save.find("FileUtils::openAtomicWrite")
        serialized_match = re.search(
            rf"serializeJson\s*\(\s*doc\s*,\s*{output}\s*\)", save
        )
        committed = save.find("FileUtils::commitAtomicWrite")
        self.assertIsNotNone(overflow)
        self.assertLess(overflow.start(), opened)
        self.assertIsNotNone(serialized_match)
        self.assertLess(opened, serialized_match.start())
        self.assertLess(serialized_match.start(), committed)


if __name__ == "__main__":
    unittest.main()
