"""
Hard-reset ESP32-S3 via esptool and stream USB-CDC logs while re-enumerating.
Strategy:
1. Call esptool --after hard_reset chip_id to reset the chip cleanly.
2. Close port handle and wait for USB-CDC re-enumeration.
3. Re-open port repeatedly until it appears, then stream for <duration> seconds.
"""
import subprocess
import time
import sys
import serial
from serial.serialutil import SerialException

port = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
duration = int(sys.argv[2]) if len(sys.argv) > 2 else 30

print(f"[1/3] Hard-resetting {port} via esptool...", flush=True)
try:
    r = subprocess.run(
        ['python', '-m', 'esptool', '--chip', 'esp32s3', '--port', port,
         '--before', 'default_reset', '--after', 'hard_reset', 'chip_id'],
        capture_output=True, text=True, timeout=25
    )
    if 'Hard resetting' not in r.stdout and 'Hard resetting' not in r.stderr:
        print('[warn] esptool output lacked "Hard resetting":')
        print(r.stdout)
        print(r.stderr)
except Exception as e:
    print(f"[warn] esptool reset failed: {e}")

print("[2/3] Waiting for USB-CDC re-enumeration...", flush=True)
s = None
deadline = time.time() + 8
while time.time() < deadline:
    try:
        s = serial.Serial(port, 115200, timeout=0.2, dsrdtr=False, rtscts=False)
        break
    except (SerialException, OSError):
        time.sleep(0.3)
if s is None:
    print(f"[error] Could not reopen {port} within 8s", flush=True)
    sys.exit(1)
print(f"[3/3] Port reopened, streaming {duration}s...", flush=True)

# Don't touch DTR/RTS after open (prevents accidental download mode)
end = time.time() + duration
while time.time() < end:
    try:
        data = s.read(4096)
        if data:
            try:
                sys.stdout.write(data.decode('utf-8', errors='replace'))
                sys.stdout.flush()
            except Exception:
                pass
    except SerialException:
        # Re-enumeration might drop the handle
        try:
            s.close()
        except Exception:
            pass
        time.sleep(0.5)
        try:
            s = serial.Serial(port, 115200, timeout=0.2, dsrdtr=False, rtscts=False)
        except Exception:
            continue
try:
    s.close()
except Exception:
    pass
