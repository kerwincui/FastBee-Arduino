"""Offline tests; fake serial only, never open a real adapter."""

from contextlib import redirect_stdout, redirect_stderr
import importlib.util
import io
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("serial_readback", ROOT / "scripts/test-modbus-serial-readback.py")
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def adu(data):
    return bytes(data) + M.crc16(data).to_bytes(2, "little")


class FakeClock:
    def __init__(self):
        self.now = 0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class FakeSerial:
    def __init__(self, clock, replies=(), pending=b""):
        self.clock, self.replies, self.pending = clock, list(replies), pending
        self.writes, self.rx, self.out_waiting = [], b"", 0
        self.opened, self.closed = False, False

    def open(self):
        self.lines_at_open = (self.dtr, self.rts)
        self.opened = True

    def close(self):
        self.closed = True

    def write(self, data):
        self.writes.append(data)
        self.rx = self.replies.pop(0) if self.replies else b""
        return len(data)

    def read(self, size):
        self.clock.sleep(0.05)
        if self.pending:
            data, self.pending = self.pending[:size], self.pending[size:]
            return data
        data, self.rx = self.rx[:size], self.rx[size:]
        return data


class SerialReadbackTests(unittest.TestCase):
    def test_documented_slave8_requests(self):
        self.assertEqual(M.read_request(8, 3, 5).hex(), "0803000500019492")
        self.assertEqual(M.read_request(8, 3, 7).hex(), "0803000700013552")

    def test_other_device_fc04_and_zero_address(self):
        frame = M.read_request(17, 4, 0)
        reply = adu([17, 4, 2, 0, 0])
        self.assertEqual(M.decode_reply(frame, reply)["value"], 0)
        self.assertEqual(frame[:6], bytes([17, 4, 0, 0, 0, 1]))

    def test_rejects_writes_broadcast_and_out_of_range(self):
        for slave, function, register in [(0, 3, 5), (248, 3, 5), (8, 6, 0),
                                         (8, 16, 7), (8, 3, -1), (8, 3, 65536)]:
            with self.subTest(values=(slave, function, register)), self.assertRaises(ValueError):
                M.read_request(slave, function, register)

    def test_reply_failures_cannot_be_success(self):
        frame = M.read_request(8, 3, 5)
        cases = [(b"", "NO_RESPONSE_BYTES"), (frame, "REQUEST_ECHO_ONLY"),
                 (b"\x08\x03", "FRAME_TOO_SHORT"),
                 (b"x" * 257, "RX_OVERFLOW"),
                 (bytes([8, 3, 2, 0, 50, 0, 0]), "CRC_INVALID"),
                 (adu([9, 3, 2, 0, 50]), "SLAVE_ADDRESS_MISMATCH"),
                 (adu([8, 4, 2, 0, 50]), "FUNCTION_MISMATCH"),
                 (adu([8, 3, 2, 0]), "ADU_LENGTH_MISMATCH"),
                 (adu([8, 3, 4, 0, 0, 0, 50]), "ADU_LENGTH_MISMATCH"),
                 (adu([8, 0x83, 2, 0]), "ADU_LENGTH_MISMATCH")]
        for reply, reason in cases:
            with self.subTest(reason=reason):
                result = M.decode_reply(frame, reply)
                self.assertEqual((result["status"], result["reason"]), ("FAIL", reason))

    def test_exception_is_not_silence_or_read_success(self):
        result = M.decode_reply(M.read_request(8, 3, 5), adu([8, 0x83, 2]))
        self.assertEqual(result["reason"], "MODBUS_EXCEPTION")
        self.assertEqual(result["exceptionCode"], 2)

    def test_cli_bounds_and_execute_list_conflict(self):
        base = ["--slave", "8", "--register", "5", "--out", "unused.json"]
        for extra in [["--timeout", "nan"], ["--timeout", "inf"],
                      ["--baud", "0"], ["--register", "5"],
                      ["--function", "6"], ["--list-ports", "--execute"]]:
            with self.subTest(extra=extra), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                M.parse_args(base + extra)

    def test_identity_gates(self):
        args = SimpleNamespace(confirm_isolated_rs485=True, port="COM77", expected_hwid="fake-adapter")
        good = dict(port="COM77", hwid="fake-adapter", vid=0x0403, pid=0x6001)
        self.assertEqual(M.verify_adapter(args, [good]), good)
        for inventory in [[], [good, good], [dict(good, hwid="other")],
                          [dict(good, vid=0x303A)], [dict(good, vid=None)]]:
            with self.subTest(inventory=inventory), self.assertRaises(ValueError):
                M.verify_adapter(args, inventory)
        args.confirm_isolated_rs485 = False
        with self.assertRaises(ValueError):
            M.verify_adapter(args, [good])

    def test_dry_run_never_opens_or_enumerates_serial_and_preserves_file(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "dry.json"
            argv = ["--slave", "8", "--register", "5", "--out", str(output)]
            with patch.object(M, "port_inventory") as inventory, patch.object(M.serial, "Serial") as create:
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(M.main(argv), 0)
                inventory.assert_not_called()
                create.assert_not_called()
                original = output.read_bytes()
                with self.assertRaises(FileExistsError):
                    M.main(argv + ["--execute"])
                self.assertEqual(output.read_bytes(), original)
                create.assert_not_called()
            self.assertEqual(json.loads(original)["status"], "DRY_RUN_NOT_EXECUTED")

    def run_fake(self, replies, *, pending=b"", identity=None, configure=None):
        clock = FakeClock()
        stream = FakeSerial(clock, replies, pending)
        if configure:
            configure(stream)
        good = dict(port="COM77", hwid="fake-adapter", vid=0x0403, pid=0x6001)
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "run.json"
            argv = ["--slave", "8", "--register", "5", "--register", "7",
                    "--out", str(output), "--execute", "--port", "COM77",
                    "--expected-hwid", "fake-adapter", "--confirm-isolated-rs485"]
            with patch.object(M, "time", clock), patch.object(M, "port_inventory", return_value=[identity or good]), \
                    patch.object(M.serial, "Serial", return_value=stream), redirect_stdout(io.StringIO()):
                rc = M.main(argv)
            report = json.loads(output.read_text(encoding="utf-8"))
        return rc, report, stream, clock

    def test_success_is_two_single_reads_not_motion_acceptance(self):
        rc, report, stream, clock = self.run_fake([adu([8, 3, 2, 0, 50]), adu([8, 3, 2, 6, 64])])
        self.assertEqual(rc, 0)
        self.assertEqual(report["status"], "READBACK_PASS_NOT_MOTION_ACCEPTANCE")
        self.assertEqual([r["value"] for r in report["reads"]], [50, 1600])
        self.assertEqual(stream.writes, [M.read_request(8, 3, 5), M.read_request(8, 3, 7)])
        self.assertEqual(stream.lines_at_open, (False, False))
        self.assertTrue(stream.closed)
        self.assertFalse(report["physicalMotorVerified"])
        self.assertLess(clock.now, 4)

    def test_silence_stops_without_retry_or_second_request(self):
        rc, report, stream, clock = self.run_fake([])
        self.assertEqual(rc, 1)
        self.assertEqual(report["reads"][0]["reason"], "NO_RESPONSE_BYTES")
        self.assertEqual(len(stream.writes), 1)
        self.assertEqual(report["unattemptedRegisters"], [7])
        self.assertTrue(stream.closed)
        self.assertLess(clock.now, 2)

    def test_existing_bus_traffic_prevents_transmission(self):
        rc, report, stream, _ = self.run_fake([], pending=b"unexpected")
        self.assertEqual(rc, 1)
        self.assertEqual(report["reads"][0]["reason"], "PREEXISTING_RX")
        self.assertEqual(stream.writes, [])

    def test_wrong_usb_device_never_opens_port(self):
        identity = dict(port="COM77", hwid="fake-adapter", vid=0x303A, pid=0x1001)
        rc, report, stream, _ = self.run_fake([], identity=identity)
        self.assertEqual(rc, 1)
        self.assertEqual(report["status"], "PREFLIGHT_FAILED")
        self.assertFalse(stream.opened)
        self.assertEqual(stream.writes, [])

    def test_short_write_is_not_retried(self):
        def configure(stream):
            def write(data):
                stream.writes.append(data)
                return 3
            stream.write = write
        rc, report, stream, _ = self.run_fake([], configure=configure)
        self.assertEqual(rc, 1)
        self.assertEqual(report["reads"][0]["reason"], "SHORT_WRITE")
        self.assertEqual(report["reads"][0]["txAcceptedBytes"], 3)
        self.assertEqual(len(stream.writes), 1)
        self.assertTrue(stream.closed)

    def test_tx_queue_wait_is_bounded(self):
        rc, report, stream, clock = self.run_fake([], configure=lambda s: setattr(s, "out_waiting", 4))
        self.assertEqual(rc, 1)
        self.assertEqual(report["reads"][0]["reason"], "TX_DRAIN_TIMEOUT")
        self.assertLess(clock.now, 2)
        self.assertTrue(stream.closed)

    def test_write_exception_preserves_uncertain_attempt_and_closes(self):
        def configure(stream):
            def write(data):
                stream.writes.append(data)
                raise M.serial.SerialTimeoutException("synthetic write timeout")
            stream.write = write
        rc, report, stream, _ = self.run_fake([], configure=configure)
        self.assertEqual(rc, 1)
        self.assertEqual(report["status"], "EXECUTION_ERROR")
        self.assertEqual(report["reads"][0]["reason"], "SERIAL_IO_ERROR")
        self.assertEqual(report["reads"][0]["attempts"], 1)
        self.assertIsNone(report["reads"][0]["txAcceptedBytes"])
        self.assertEqual(report["unattemptedRegisters"], [7])
        self.assertEqual(len(stream.writes), 1)
        self.assertTrue(stream.closed)

    def test_port_open_failure_retains_zero_read_attempts(self):
        def configure(stream):
            def open_port():
                raise M.serial.SerialException("synthetic busy adapter")
            stream.open = open_port
        rc, report, stream, _ = self.run_fake([], configure=configure)
        self.assertEqual(rc, 1)
        self.assertEqual(report["status"], "PREFLIGHT_FAILED")
        self.assertEqual(report["reads"], [])
        self.assertEqual(report["unattemptedRegisters"], [5, 7])
        self.assertEqual(stream.writes, [])
        self.assertTrue(stream.closed)


if __name__ == "__main__":
    unittest.main()
