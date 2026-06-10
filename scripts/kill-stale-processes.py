"""
PlatformIO pre-build hook: detect stale processes locking .bin files.

On Windows, Serial Monitor's esptool.exe / python.exe may remain running
after disconnect, locking bootloader.bin / firmware.bin and causing:
  "另一个程序正在使用此文件，进程无法访问"

Fix options (run in a separate terminal):
  1. taskkill /F /IM esptool.exe
  2. Close Serial Monitor in VSCode before building
  3. Use build-all-artifacts.ps1 (auto-kills stale processes)

Note: subprocess.Popen() hangs in PIO's SCons sandbox on Windows,
so this hook only checks for locked files and prints warnings.
"""
import os
import sys

Import("env")  # type: ignore[name-defined]


def check_bin_locked(build_dir):
    """Check if critical .bin files are locked by another process."""
    critical_files = ["bootloader.bin", "firmware.bin", "partitions.bin"]
    locked = []
    for fname in critical_files:
        fpath = os.path.join(build_dir, fname)
        if os.path.exists(fpath):
            try:
                # Try to open for writing - if locked, will raise
                with open(fpath, "r+b"):
                    pass
            except (IOError, OSError, PermissionError):
                locked.append(fname)
    return locked


def pre_build_check(source, target, env):
    """Pre-build hook: warn if .bin files are locked."""
    build_dir = env.subst("$BUILD_DIR")
    locked = check_bin_locked(build_dir)
    if locked:
        print("\n" + "=" * 60)
        print("WARNING: The following files are locked by another process:")
        for f in locked:
            print(f"  - {f}")
        print("\nFix: Close Serial Monitor or run in another terminal:")
        print("  taskkill /F /IM esptool.exe")
        print("=" * 60 + "\n")
        # Don't abort - let PIO report the actual error with context


if sys.platform == "win32":
    env.AddPreAction("$BUILD_DIR/firmware.bin", pre_build_check)  # type: ignore[name-defined]
