import serial, time, sys

PORT = 'COM6'
BAUD = 115200
CAPTURE_SECONDS = 90

try:
    ser = serial.Serial(PORT, BAUD, timeout=1, rtscts=False, dsrdtr=False)
    # Reset device via RTS pulse
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    print("=== Device reset, capturing for %d seconds ===" % CAPTURE_SECONDS)
    sys.stdout.flush()
    start = time.time()
    while time.time() - start < CAPTURE_SECONDS:
        data = ser.read(2048)
        if data:
            sys.stdout.write(data.decode('utf-8', errors='replace'))
            sys.stdout.flush()
    print("\n=== Capture complete ===")
    ser.close()
except Exception as e:
    print("Error: %s" % str(e))
