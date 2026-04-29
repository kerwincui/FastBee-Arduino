"""Open COM4, trigger reset via esptool in subprocess, capture CDC output."""
import sys, time, subprocess, threading, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
DURATION = int(sys.argv[2]) if len(sys.argv) > 2 else 40

PY = r'C:\Users\kerwin\.platformio\penv\Scripts\python.exe'
ESPTOOL = r'C:\Users\kerwin\.platformio\packages\tool-esptoolpy\esptool.py'

# Note: we can't open the port while esptool uses it. Strategy:
# 1. esptool resets chip (takes ~2s, ends with "Hard resetting")
# 2. Immediately open port and capture for DURATION seconds

print(f'[1] Resetting {PORT} via esptool...', flush=True)
r = subprocess.run([PY, ESPTOOL, '--chip', 'esp32s3', '--port', PORT,
                    '--before', 'default_reset', '--after', 'hard_reset', 'chip_id'],
                   capture_output=True, text=True, timeout=30)
print(f'[1] esptool exit={r.returncode}', flush=True)

# Wait briefly for USB-CDC re-enumeration
time.sleep(0.5)

print(f'[2] Opening {PORT} and capturing for {DURATION}s...', flush=True)
s = None
for attempt in range(20):
    try:
        s = serial.Serial(PORT, 115200, timeout=0.5, dsrdtr=False, rtscts=False)
        break
    except Exception as e:
        time.sleep(0.25)

if s is None:
    print('[FATAL] Could not open port', flush=True)
    sys.exit(1)

end = time.time() + DURATION
total = 0
while time.time() < end:
    try:
        data = s.read(4096)
        if data:
            total += len(data)
            sys.stdout.write(data.decode('utf-8', errors='replace'))
            sys.stdout.flush()
    except Exception as e:
        print(f'\n[err] {e}', flush=True)
        try: s.close()
        except: pass
        time.sleep(0.3)
        try: s = serial.Serial(PORT, 115200, timeout=0.5, dsrdtr=False, rtscts=False)
        except: break

print(f'\n[done] total bytes: {total}', flush=True)
try: s.close()
except: pass
