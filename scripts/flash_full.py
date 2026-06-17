#!/usr/bin/env python3
"""Full erase + flash ESP32-S3."""
import subprocess, sys, os

ESPTOOL = r"C:\Users\kerwin\.platformio\packages\tool-esptoolpy\esptool.py"
PYTHON = r"C:\Users\kerwin\.platformio\penv\Scripts\python.exe"
PORT = "COM6"
CHIP = "esp32s3"
BUILD = r"D:\project\gitee\FastBee-Arduino\.pio\build\esp32s3-F16R8"
BOOT_APP0 = r"C:\Users\kerwin\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"

def run(args):
    print(f"\n>>> {' '.join(args)}")
    r = subprocess.run(args, capture_output=False)
    if r.returncode != 0:
        print(f"FAILED with code {r.returncode}")
        sys.exit(r.returncode)
    print("OK")

# Step 1: Erase entire flash
run([PYTHON, ESPTOOL, "--chip", CHIP, "--port", PORT, "erase-flash"])

# Step 2: Write bootloader + partitions + firmware
run([PYTHON, ESPTOOL, "--chip", CHIP, "--port", PORT, "--baud", "921600",
     "write-flash",
     "0x0", os.path.join(BUILD, "bootloader.bin"),
     "0x8000", os.path.join(BUILD, "partitions.bin"),
     "0xe000", BOOT_APP0,
     "0x10000", os.path.join(BUILD, "firmware.bin")])

print("\n=== Flash complete! ===")
