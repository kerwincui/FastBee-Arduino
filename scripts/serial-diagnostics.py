"""Serial log capture helper for ESP32 boards.

This replaces the old one-off sniff/reset scripts with a single entry point:

    python scripts/serial-diagnostics.py COM6 --duration 30
    python scripts/serial-diagnostics.py COM6 --reset --chip esp32s3 --duration 45
"""

import argparse
import subprocess
import sys
import time

try:
    import serial
    from serial.serialutil import SerialException
except ImportError as exc:
    raise SystemExit("pyserial is required: pip install pyserial") from exc


def reset_board(args):
    command = [
        args.python,
        "-m",
        "esptool",
        "--chip",
        args.chip,
        "--port",
        args.port,
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "chip_id",
    ]
    print("[reset] " + " ".join(command), flush=True)
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=args.reset_timeout,
            check=False,
        )
    except Exception as exc:
        print(f"[warn] reset failed: {exc}", flush=True)
        return
    if result.returncode != 0:
        print(f"[warn] esptool exit={result.returncode}", flush=True)
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)


def open_serial(args, deadline):
    last_error = None
    while time.time() < deadline:
        try:
            return serial.Serial(
                args.port,
                args.baud,
                timeout=0.2,
                dsrdtr=False,
                rtscts=False,
            )
        except (SerialException, OSError) as exc:
            last_error = exc
            time.sleep(0.3)
    raise SystemExit(f"Could not open {args.port}: {last_error}")


def capture_logs(args):
    deadline = time.time() + args.open_timeout
    stream = open_serial(args, deadline)
    print(f"[capture] {args.port} @ {args.baud}, {args.duration}s", flush=True)
    end_time = time.time() + args.duration
    total = 0
    while time.time() < end_time:
        try:
            data = stream.read(4096)
            if data:
                total += len(data)
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()
        except (SerialException, OSError):
            try:
                stream.close()
            except Exception:
                pass
            time.sleep(0.4)
            stream = open_serial(args, time.time() + args.open_timeout)
    try:
        stream.close()
    except Exception:
        pass
    print(f"\n[done] captured {total} byte(s)", flush=True)


def parse_args():
    parser = argparse.ArgumentParser(description="Capture ESP32 serial logs.")
    parser.add_argument("port", nargs="?", default="COM6", help="Serial port, for example COM6.")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate.")
    parser.add_argument("--duration", type=int, default=30, help="Capture duration in seconds.")
    parser.add_argument("--open-timeout", type=int, default=10, help="Seconds to wait for port open.")
    parser.add_argument("--reset", action="store_true", help="Reset the board before capturing.")
    parser.add_argument("--chip", default="esp32s3", help="esptool chip name used with --reset.")
    parser.add_argument("--python", default=sys.executable, help="Python executable used for esptool.")
    parser.add_argument("--reset-timeout", type=int, default=30, help="Reset command timeout.")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.reset:
        reset_board(args)
        time.sleep(0.5)
    capture_logs(args)


if __name__ == "__main__":
    main()
