"""Keep generic peripheral maintenance independent from optional subsystems."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
FRAMEWORK = (ROOT / "src/core/FastBeeFramework.cpp").read_text(encoding="utf-8")
PERIPHERALS = "\n".join(
    (ROOT / path).read_text(encoding="utf-8")
    for path in (
        "src/core/PeripheralManager.cpp",
        "src/core/PeripheralManagerDiagnostics.cpp",
    )
)


class PeripheralMaintenanceContractTests(unittest.TestCase):
    def test_framework_registers_bounded_high_priority_maintenance(self):
        task = re.search(
            r'addTask\("peripheral_io".*?performMaintenance\(\);\s*'
            r'\},\s*nullptr,\s*20,\s*TaskPriority::PRIORITY_HIGH\)',
            FRAMEWORK,
            re.DOTALL,
        )
        self.assertIsNotNone(task)

        system_tasks = FRAMEWORK.find("bool FastBeeFramework::addSystemTasks()")
        task_pos = FRAMEWORK.find('addTask("peripheral_io"')
        self.assertGreater(task_pos, system_tasks)
        previous_guard = FRAMEWORK.rfind("#if FASTBEE_ENABLE_PERIPH_EXEC", system_tasks, task_pos)
        previous_end = FRAMEWORK.rfind("#endif", system_tasks, task_pos)
        next_guard = FRAMEWORK.find("#if FASTBEE_ENABLE_PERIPH_EXEC", task_pos)
        self.assertGreater(previous_end, previous_guard)
        self.assertGreater(next_guard, task_pos)

    def test_maintenance_drives_polling_and_interrupt_dispatch(self):
        maintenance = re.search(
            r"void PeripheralManager::performMaintenance\(\)\s*\{(.*?)\n\}",
            PERIPHERALS,
            re.DOTALL,
        )
        self.assertIsNotNone(maintenance)
        body = maintenance.group(1)
        self.assertIn("pollIoExpanders();", body)
        self.assertIn("processInterruptQueue();", body)

    def test_io_polling_is_budgeted_and_retries_retired_channels(self):
        self.assertIn("IO_MAINTENANCE_BUDGET_MS", PERIPHERALS)
        self.assertIn("providerOffset", PERIPHERALS)
        self.assertIn("FastBee::I2CBusLock busGuard(0)", PERIPHERALS)
        self.assertIn("ioPendingSyncs()[providerId]", PERIPHERALS)
        self.assertIn('parentState->second.lastError = "IO_CONFIG_PENDING"', PERIPHERALS)
        self.assertIn("if (syncIoExpander(it->first))", PERIPHERALS)


if __name__ == "__main__":
    unittest.main()
