"""Read-only full flash backup with a bounded serial receive window.

Some USB UART drivers lose bytes with esptool's 64-frame read window. This
uses the same stub read command with a single 1 KiB frame in flight and checks
both the streamed MD5 and a fresh flash MD5 before creating the backup file.
It never erases or writes device flash and refuses to replace local backups.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esptool-dir", required=True)
    parser.add_argument("--port", required=True)
    parser.add_argument("--chip", choices=["ESP32", "ESP32-S3", "ESP32-C3", "ESP32-C6"], required=True)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--baud", type=int, default=460800)
    parser.add_argument("--length", type=int, default=0, help="Optional short read for transport diagnostics")
    parser.add_argument("--offset", type=lambda value: int(value, 0), default=0,
                        help="Start address for a bounded partition snapshot")
    parser.add_argument("output")
    args = parser.parse_args()
    destination = Path(args.output).resolve()
    if destination.exists():
        raise ValueError("Backup destination already exists")
    if args.size not in (4194304, 8388608, 16777216):
        raise ValueError("Unsupported expected flash size")
    length = args.length or args.size
    if not 0 < length <= args.size or not 0 <= args.offset <= args.size - length:
        raise ValueError("Invalid read length")
    sys.path.insert(0, str(Path(args.esptool_dir).resolve()))
    from esptool.cmds import detect_chip, attach_flash

    esp = detect_chip(port=args.port)
    try:
        if esp.CHIP_NAME != args.chip:
            raise ValueError("Connected chip does not match deployment target")
        esp = esp.run_stub()
        attach_flash(esp)
        capacity = (esp.flash_id() >> 16) & 255
        if (1 << capacity) != args.size:
            raise ValueError("Detected flash capacity differs from expected size")
        esp.flash_set_parameters(args.size)
        esp.change_baud(args.baud)
        block = 1024
        esp.check_command("read flash", esp.ESP_CMDS["READ_FLASH"],
                          struct.pack("<IIII", args.offset, length, block, 1))
        content = bytearray()
        started = time.monotonic()
        while len(content) < length:
            esp._port.timeout = 5
            packet = esp.read()
            expected = min(block, length - len(content))
            if len(packet) != expected:
                raise ValueError("Short or oversized flash frame; backup discarded")
            content.extend(packet)
            esp.write(struct.pack("<I", len(content)))
            if len(content) % (1024 * 1024) == 0 or len(content) == length:
                print(f"{args.port}: {len(content)}/{length} verified-frame bytes received", flush=True)
        digest = esp.read()
        if len(digest) != 16 or hashlib.md5(content).digest() != digest:
            raise ValueError("Streamed flash MD5 mismatch; backup discarded")
        expected_md5 = esp.flash_md5sum(args.offset, length)
        if expected_md5.lower() != hashlib.md5(content).hexdigest():
            raise ValueError("Independent flash MD5 mismatch; backup discarded")
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("xb") as output:
            output.write(content)
        print(f"Verified backup: {destination}")
        print(f"Bytes: {len(content)}; SHA256: {hashlib.sha256(content).hexdigest()}")
        print(f"Elapsed seconds: {time.monotonic() - started:.1f}")
    finally:
        try:
            esp.hard_reset()
        finally:
            esp._port.close()


if __name__ == "__main__":
    main()
