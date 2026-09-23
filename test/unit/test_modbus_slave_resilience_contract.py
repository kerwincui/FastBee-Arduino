import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "include/protocols/ModbusHandler.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "src/protocols/ModbusHandler.cpp").read_text(encoding="utf-8")


def section(source, start_marker, end_marker):
    start = source.index(start_marker)
    end = source.index(end_marker, start)
    return source[start:end]


class ModbusSlaveResilienceContractTests(unittest.TestCase):
    def test_breaker_uses_bounded_per_slave_storage(self):
        self.assertIn(
            "SlaveBackoffState _slaveBackoff[SLAVE_BACKOFF_SLOT_COUNT]", HEADER
        )
        self.assertIn("Protocols::MODBUS_MAX_POLL_TASKS + 4", HEADER)
        self.assertNotIn("_consecutiveTimeouts", HEADER)
        self.assertNotIn("_cooldownUntil", HEADER)
        self.assertNotRegex(HEADER, r"std::(?:map|unordered_map|vector)\s*<")

    def test_deadline_and_backoff_math_are_wrap_safe_and_bounded(self):
        deadline = section(
            SOURCE,
            "bool ModbusHandler::_deadlineReached",
            "ModbusHandler::SlaveBackoffState* ModbusHandler::_findSlaveBackoffState",
        )
        self.assertIn("static_cast<int32_t>(now - deadline) >= 0", deadline)

        backoff = section(
            SOURCE,
            "uint32_t ModbusHandler::_computeSlaveBackoffMs",
            "bool ModbusHandler::_shouldDeferSlaveRequest",
        )
        self.assertIn("baseMs << shift", backoff)
        self.assertIn("SLAVE_BACKOFF_JITTER_WINDOW_MS", backoff)
        self.assertIn("SLAVE_BACKOFF_MAX_MS - jitterMs", backoff)
        self.assertIn("ONESHOT_CRC_ERROR", backoff)
        self.assertIn("ONESHOT_EXCEPTION", backoff)

    def test_each_error_class_is_tracked_and_success_recovers(self):
        outcome = section(
            SOURCE,
            "void ModbusHandler::_recordSlaveOutcome",
            "ModbusHandler::~ModbusHandler",
        )
        for token in (
            "ONESHOT_TIMEOUT",
            "ONESHOT_CRC_ERROR",
            "ONESHOT_EXCEPTION",
            "timeoutCount",
            "crcErrorCount",
            "exceptionCount",
        ):
            self.assertIn(token, outcome)
        self.assertIn("state->consecutiveFailures = 0", outcome)
        self.assertIn("state->backoffLevel = 0", outcome)
        self.assertIn("state->cooldownUntil = 0", outcome)
        self.assertIn("state->halfOpen = true", SOURCE)
        self.assertIn("state->halfOpenProbeCount", SOURCE)

    def test_control_and_write_traffic_bypass_poll_breaker(self):
        transaction = section(
            SOURCE,
            "OneShotError ModbusHandler::_executeModbusTransactionRaw",
            "OneShotResult ModbusHandler::sendOneShotRequest",
        )
        self.assertIn("!bypassSlaveBackoff", transaction)
        self.assertIn("isReadFunctionCode(request[1])", transaction)
        self.assertLess(
            transaction.index("_shouldDeferSlaveRequest"),
            transaction.index("modbusSerial->write"),
        )

        control = section(
            SOURCE,
            "OneShotResult ModbusHandler::sendControlRequest",
            "OneShotResult ModbusHandler::writeCoilOnce",
        )
        self.assertIn("retriesOverride, true", control)
        raw_control = section(
            SOURCE,
            "OneShotError ModbusHandler::sendRawFrameBytesOnce",
            "OneShotResult ModbusHandler::sendRawFrameOnce",
        )
        self.assertRegex(raw_control, r"0, 0xFF, isControl\)")

    def test_retry_spread_and_metrics_remain_observable(self):
        transaction = section(
            SOURCE,
            "OneShotError ModbusHandler::_executeModbusTransactionRaw",
            "OneShotResult ModbusHandler::sendOneShotRequest",
        )
        self.assertIn("error == ONESHOT_CRC_ERROR ? 20U : 50U", transaction)
        self.assertIn("expectedSlaveAddr * 17U", transaction)
        self.assertIn("retryBaseMs << retryShift", transaction)

        stats = section(
            SOURCE,
            "String ModbusHandler::getPollStatistics",
            "const ModbusHandler::PollTaskCache* ModbusHandler::getTaskCache",
        )
        for key in (
            '"backoffSkips"',
            '"backoffEvictions"',
            '"slaveHealth"',
            '"slaveTimeouts"',
            '"slaveCrcErrors"',
            '"slaveExceptions"',
            '"halfOpenProbes"',
        ):
            self.assertIn(key, stats)


if __name__ == "__main__":
    unittest.main()
