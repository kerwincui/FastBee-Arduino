"""Bounded FC03/04 comparison through a separately identified USB-RS485 master.

Dry-run by default. Never use an ESP32 debug port or two active bus masters.
No register writes, raw-frame option, scanning, retries or direction toggles.
"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import struct
import time

import serial
from serial.tools import list_ports


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def crc16(data):
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def read_request(slave, function, register):
    if not 1 <= slave <= 247 or function not in (3, 4):
        raise ValueError("Only unicast FC03/04 reads are allowed")
    if not 0 <= register <= 65535:
        raise ValueError("Register must be a zero-based uint16 address")
    body = struct.pack(">BBHH", slave, function, register, 1)
    return body + struct.pack("<H", crc16(body))


def decode_reply(request, reply):
    result = {"status": "FAIL", "rxHex": reply.hex(" "), "rxBytes": len(reply)}
    if not reply:
        reason = "NO_RESPONSE_BYTES"
    elif reply == request:
        reason = "REQUEST_ECHO_ONLY"
    elif len(reply) < 5:
        reason = "FRAME_TOO_SHORT"
    elif len(reply) > 256:
        reason = "RX_OVERFLOW"
    elif crc16(reply[:-2]) != int.from_bytes(reply[-2:], "little"):
        reason = "CRC_INVALID"
    elif reply[0] != request[0]:
        reason = "SLAVE_ADDRESS_MISMATCH"
    elif reply[1] == request[1] | 0x80:
        reason = "MODBUS_EXCEPTION" if len(reply) == 5 else "ADU_LENGTH_MISMATCH"
        if len(reply) == 5:
            result["exceptionCode"] = reply[2]
    elif reply[1] != request[1]:
        reason = "FUNCTION_MISMATCH"
    elif len(reply) != 7 or reply[2] != 2:
        reason = "ADU_LENGTH_MISMATCH"
    else:
        reason = "READBACK_CONFIRMED"
        result.update(status="PASS", value=int.from_bytes(reply[3:5], "big"))
    result["reason"] = reason
    return result


def port_inventory():
    return [{"port": p.device, "description": p.description, "hwid": p.hwid,
             "vid": p.vid, "pid": p.pid, "serialNumber": p.serial_number,
             "location": p.location} for p in list_ports.comports()]


def verify_adapter(args, inventory):
    if not args.confirm_isolated_rs485 or not args.port or not args.expected_hwid:
        raise ValueError("Execution requires a port, exact expected HWID and --confirm-isolated-rs485")
    matches = [p for p in inventory if p["port"].casefold() == args.port.casefold()]
    if len(matches) != 1:
        raise ValueError("The specified adapter is absent or ambiguous")
    actual = matches[0]
    if actual["hwid"] != args.expected_hwid or actual["vid"] is None or actual["pid"] is None:
        raise ValueError("USB adapter identity does not match; no port opened")
    if actual["vid"] == 0x303A:
        raise ValueError("Espressif USB/debug ports are not independent RS485 adapters")
    return actual


def open_adapter(args):
    stream = serial.Serial(port=None, baudrate=args.baud, bytesize=8,
                           parity=args.parity, stopbits=args.stopbits,
                           timeout=0.05, write_timeout=0.5, rtscts=False,
                           dsrdtr=False, xonxoff=False)
    # Match serial-diagnostics.py: do not assert modem lines when opening.
    # Adapter/OS opening transients still require an isolated, safe bench.
    stream.dtr = False
    stream.rts = False
    stream.port = args.port
    try:
        stream.open()
    except Exception:
        stream.close()
        raise
    return stream


def read_window(stream, seconds):
    started = time.monotonic()
    deadline = started + seconds
    received = bytearray()
    first_byte_ms = None
    while time.monotonic() < deadline and len(received) < 257:
        data = stream.read(257 - len(received))
        if data:
            if first_byte_ms is None:
                first_byte_ms = round((time.monotonic() - started) * 1000, 3)
            received.extend(data)
    return bytes(received), first_byte_ms


def exchange(stream, request, timeout, result=None):
    started = time.monotonic()
    if result is None:
        result = {}
    result.update(startedAt=utc_now(), txHex=request.hex(" "), attempts=0,
                  status="RUNNING", txAcceptedBytes=None)
    # Unexpected bus traffic is evidence, not something to silently discard.
    pending, _ = read_window(stream, 0.1)
    if pending:
        result.update(status="FAIL", reason="PREEXISTING_RX",
                      rxHex=pending.hex(" "), rxBytes=len(pending))
        return result
    result["attempts"] = 1
    result["txAcceptedBytes"] = stream.write(request)
    if result["txAcceptedBytes"] != len(request):
        result.update(status="FAIL", reason="SHORT_WRITE")
        return result
    # flush() can wait indefinitely on some drivers; bound the queue wait.
    drain_deadline = time.monotonic() + 1.0
    while stream.out_waiting:
        if time.monotonic() >= drain_deadline:
            result.update(status="FAIL", reason="TX_DRAIN_TIMEOUT")
            return result
        time.sleep(0.01)
    reply, first_byte_ms = read_window(stream, timeout)
    result.update(decode_reply(request, reply))
    result.update(firstByteAfterDrainMs=first_byte_ms,
                  elapsedMs=round((time.monotonic() - started) * 1000, 3),
                  completedAt=utc_now())
    return result


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list-ports", action="store_true", help="Enumerate metadata only; no port opens")
    parser.add_argument("--slave", type=int)
    parser.add_argument("--function", type=int, choices=(3, 4), default=3)
    parser.add_argument("--register", type=int, action="append", default=[])
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--parity", choices=("N", "E", "O"), default="N")
    parser.add_argument("--stopbits", type=int, choices=(1, 2), default=1)
    parser.add_argument("--timeout", type=float, default=1.5)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--port")
    parser.add_argument("--expected-hwid")
    parser.add_argument("--confirm-isolated-rs485", action="store_true",
                        help="Confirm a safe mechanism, auto-direction USB-RS485 adapter, and disconnected screen/other masters")
    args = parser.parse_args(argv)
    if args.list_ports:
        if args.execute:
            parser.error("--list-ports cannot execute reads")
        return args
    if args.slave is None or not 1 <= args.slave <= 247:
        parser.error("Specify one slave in 1..247; broadcast/scanning is not allowed")
    if not 1 <= len(args.register) <= 8 or len(set(args.register)) != len(args.register):
        parser.error("Specify 1..8 distinct --register addresses")
    if any(not 0 <= r <= 65535 for r in args.register):
        parser.error("Registers must be zero-based uint16 addresses")
    if not 300 <= args.baud <= 1000000 or not 0.1 <= args.timeout <= 5:
        parser.error("Baud must be 300..1000000 and timeout 0.1..5 seconds")
    if args.out is None:
        parser.error("--out is required; existing reports are never overwritten")
    return args


def main(argv=None):
    args = parse_args(argv)
    if args.list_ports:
        print(json.dumps(port_inventory(), indent=2))
        return 0
    frames = [read_request(args.slave, args.function, r) for r in args.register]
    report = {"startedAt": utc_now(), "status": "DRY_RUN_NOT_EXECUTED",
              "portOpened": False, "slave": args.slave, "function": args.function,
              "baud": args.baud, "framing": f"8{args.parity}{args.stopbits}",
              "plannedReads": [{"register": r, "quantity": 1, "txHex": f.hex(" ")}
                               for r, f in zip(args.register, frames)],
              "reads": [], "registerWrites": 0, "motorActionsSent": 0,
              "retries": 0, "physicalMotorVerified": False,
              "continuous24hStatus": "NOT_RUN"}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Reserve evidence before serial access; never replace a previous run.
    with args.out.open("x", encoding="utf-8") as output:
        def persist():
            output.seek(0)
            json.dump(report, output, indent=2)
            output.write("\n")
            output.truncate()
            output.flush()

        persist()
        if args.execute:
            stream = None
            result = None
            try:
                report["adapter"] = verify_adapter(args, port_inventory())
                report["status"] = "RUNNING"
                persist()
                stream = open_adapter(args)
                report["portOpened"] = True
                persist()
                for register, frame in zip(args.register, frames):
                    result = {"register": register, "status": "RUNNING", "attempts": 0}
                    report["reads"].append(result)
                    persist()
                    exchange(stream, frame, args.timeout, result)
                    persist()
                    if result["status"] != "PASS":
                        break
                report["status"] = ("READBACK_PASS_NOT_MOTION_ACCEPTANCE"
                                    if len(report["reads"]) == len(frames)
                                    and all(r["status"] == "PASS" for r in report["reads"])
                                    else "FAIL")
            except Exception as exc:
                report["status"] = "EXECUTION_ERROR" if report["portOpened"] else "PREFLIGHT_FAILED"
                report["error"] = f"{type(exc).__name__}: {exc}"
                if result is not None and result["status"] == "RUNNING":
                    result.update(status="ERROR", reason="SERIAL_IO_ERROR",
                                  error=report["error"], completedAt=utc_now())
            finally:
                if stream is not None:
                    try:
                        stream.close()
                    except Exception as exc:
                        report["closeError"] = str(exc)
                        report["status"] = "EXECUTION_ERROR"
                report["completedAt"] = utc_now()
                report["unattemptedRegisters"] = args.register[len(report["reads"]):]
                persist()
    print(json.dumps({"status": report["status"], "out": str(args.out),
                      "readResults": len(report["reads"]), "portOpened": report["portOpened"]}))
    return 0 if report["status"] in ("DRY_RUN_NOT_EXECUTED", "READBACK_PASS_NOT_MOTION_ACCEPTANCE") else 1


if __name__ == "__main__":
    raise SystemExit(main())
