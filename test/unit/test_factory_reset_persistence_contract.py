"""Contracts for durable and fail-safe factory reset behavior."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
STORAGE = (ROOT / "src/systems/ConfigStorage.cpp").read_text(encoding="utf-8")
SYSTEM_ROUTE = (ROOT / "src/network/handlers/SystemRouteHandler.cpp").read_text(
    encoding="utf-8"
)
EXECUTOR = (ROOT / "src/core/PeriphExecExecutor.cpp").read_text(encoding="utf-8")


def function_body(source, signature, next_signature):
    start = source.index(signature)
    end = source.index(next_signature, start + len(signature))
    return source[start:end]


def braced_block(source, marker):
    marker_at = source.index(marker)
    block_start = source.index("{", marker_at + len(marker))
    depth = 0
    for index in range(block_start, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[block_start + 1 : index]
    raise AssertionError(f"unterminated block after {marker!r}")


class FactoryResetPersistenceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.save_factory = function_body(
            STORAGE,
            "bool ConfigStorage::saveFactoryJSONConfig(",
            "bool ConfigStorage::deleteJSONConfigGenerations(",
        )
        cls.delete_generations = function_body(
            STORAGE,
            "bool ConfigStorage::deleteJSONConfigGenerations(",
            "bool ConfigStorage::loadJSONConfig(",
        )
        route_start = SYSTEM_ROUTE.index('/api/system/factory-reset')
        route_end = SYSTEM_ROUTE.index("// Config", route_start)
        cls.web_reset = SYSTEM_ROUTE[route_start:route_end]
        cls.exec_reset = braced_block(
            EXECUTOR, "case ExecActionType::ACTION_SYS_FACTORY_RESET:"
        )

    def test_factory_write_validates_and_removes_older_generations(self):
        body = self.save_factory
        self.assertIn("deserializeJson(parsed, jsonContent)", body)
        self.assertIn("findInCache(filename)", body)
        self.assertIn('filename + ".factory.tmp"', body)
        self.assertIn("verifyJsonFile(stagedPath)", body)
        self.assertIn("atomicReplaceFromTmp(stagedPath, filename)", body)
        self.assertIn('filename + ".bak"', body)
        self.assertIn('filename + ".tmp"', body)
        self.assertIn("verifyJsonFile(filename)", body)

    def test_generation_delete_covers_main_backup_and_both_temp_files(self):
        body = self.delete_generations
        self.assertIn("findInCache(filename)", body)
        self.assertIn("filename,", body)
        self.assertIn('filename + ".bak"', body)
        self.assertIn('filename + ".tmp"', body)
        self.assertIn('filename + ".factory.tmp"', body)
        self.assertIn("LittleFS.exists(path) && !LittleFS.remove(path)", body)

    def test_web_reset_covers_every_known_json_config(self):
        source_files = list((ROOT / "src").rglob("*.cpp"))
        source_files += list((ROOT / "include").rglob("*.h"))
        all_source = "\n".join(path.read_text(encoding="utf-8") for path in source_files)
        known = set(
            re.findall(r'"(/config/[A-Za-z0-9_.-]+\.json)"', all_source)
        )
        covered = set(
            re.findall(r'"(/config/[A-Za-z0-9_.-]+\.json)"', self.web_reset)
        )
        self.assertEqual(known, covered)
        self.assertIn("RuntimeFeatureManager::instance().resetToDefaults()", self.web_reset)
        self.assertIn("networkManager->clearPersistentFallback()", self.web_reset)
        self.assertIn('"/config/screen_panel.json.touch-v1.bak"', self.web_reset)

    def test_web_reset_reports_partial_failure_without_scheduling_reboot(self):
        failure_at = self.web_reset.index("if (resetFailureCount > 0)")
        failure_return = self.web_reset.index("return;", failure_at)
        restart_at = self.web_reset.index("SystemRebooter::scheduleReboot", failure_return)
        self.assertLess(failure_return, restart_at)
        self.assertIn('request->send(500, "application/json", errorJson)', self.web_reset)

    def test_rule_triggered_reset_aborts_reboot_on_any_cleanup_failure(self):
        body = self.exec_reset
        self.assertIn("Factory reset rejected: use authenticated reset endpoint", body)
        self.assertIn("return false;", body)
        self.assertNotIn("deleteJSONConfigGenerations", body)
        self.assertNotIn("SystemRebooter::scheduleReboot", body)


if __name__ == "__main__":
    unittest.main()
