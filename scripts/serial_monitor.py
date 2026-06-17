#!/usr/bin/env python3
"""Reset ESP32 via RTS and capture serial output."""
import serial
import time
import sys

PORT = 'COM6'
BAUD = 115200
CAPTURE_SECONDS = 45

ser = serial.Serial(PORT, BAUD, timeout=1)

# Reset device via RTS pulse
print(f"=== Resetting device on {PORT} ===")
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.05)

print(f"=== Capturing serial output for {CAPTURE_SECONDS}s ===")
start = time.time()
buf = b""
try:
    while time.time() - start < CAPTURE_SECONDS:
        data = ser.read(1024)
        if data:
            buf += data
            try:
                text = data.decode('utf-8', errors='replace')
                sys.stdout.write(text)
                sys.stdout.flush()
            except:
                pass
except KeyboardInterrupt:
    pass
finally:
    ser.close()
    print(f"\n=== Captured {len(buf)} bytes in {time.time()-start:.1f}s ===")
