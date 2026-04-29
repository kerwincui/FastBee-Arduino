import serial
import time
import sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
duration = int(sys.argv[2]) if len(sys.argv) > 2 else 15

# retry open loop (USB-CDC may be re-enumerating)
s = None
deadline = time.time() + 10
while time.time() < deadline:
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
        break
    except Exception:
        time.sleep(0.3)
if s is None:
    sys.stderr.write('Could not open ' + port + '\n')
    sys.exit(1)

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
    except Exception:
        try: s.close()
        except: pass
        time.sleep(0.4)
        try:
            s = serial.Serial(port, 115200, timeout=0.2)
        except Exception:
            continue
try: s.close()
except: pass
