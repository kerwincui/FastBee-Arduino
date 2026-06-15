"""
PlatformIO pre-build hook: kill stale compiler processes & detect locked files.

On Windows, a failed/interrupted build can leave behind compiler processes
(collect2, cc1, xtensa-esp32*-elf-g++, etc.) that hold locks on .o files
in the build directory.  The next build then fails with:
  "另一个程序正在使用此文件，进程无法访问"

This script runs as a ``pre:`` extra_scripts hook (outside the SCons sandbox),
so ``subprocess`` calls are safe.  It performs two tasks:

1. **Kill stale compiler processes** – terminate orphaned cross-compiler
   toolchain processes that may be locking intermediate object files.
2. **Check locked files** – detect .bin / .o files still held by other
   processes and print actionable warnings.
"""
import os
import sys
import subprocess

Import("env")  # type: ignore[name-defined]

# ---------------------------------------------------------------------------
# Stale compiler process names that should be safe to kill unconditionally.
# These are specific to the xtensa / esp-riscv cross-compilation toolchain
# and are never legitimate long-running processes.
# ---------------------------------------------------------------------------
STALE_COMPILER_PROCESSES = [
    # GCC compiler backend
    "cc1.exe",
    "cc1plus.exe",
    # GCC driver (various chip prefixes)
    "xtensa-esp-elf-gcc.exe",
    "xtensa-esp-elf-g++.exe",
    "xtensa-esp32-elf-gcc.exe",
    "xtensa-esp32-elf-g++.exe",
    "xtensa-esp32s2-elf-gcc.exe",
    "xtensa-esp32s2-elf-g++.exe",
    "xtensa-esp32s3-elf-gcc.exe",
    "xtensa-esp32s3-elf-g++.exe",
    "xtensa-esp32c3-elf-gcc.exe",
    "xtensa-esp32c3-elf-g++.exe",
    "xtensa-esp32c6-elf-gcc.exe",
    "xtensa-esp32c6-elf-g++.exe",
    "riscv32-esp-elf-gcc.exe",
    "riscv32-esp-elf-g++.exe",
    # Linker
    "collect2.exe",
    "ld.exe",
]

# Processes we deliberately do NOT kill:
#   - python.exe   : PlatformIO's own runtime; killing it aborts the build
#   - esptool.exe  : may be in use by Serial Monitor (handled by .bin lock check)
#   - ar.exe       : generic archiver, could belong to another parallel build


def _taskkill_on_windows(process_names):
    """Kill stale compiler processes on Windows using taskkill /F /IM.

    Uses subprocess.call (not Popen) to avoid any async complications.
    Returns the list of process names that were actually terminated.
    """
    killed = []
    for pname in process_names:
        try:
            # /F  = force kill
            # /IM = image name (process name)
            # stderr redirected to DEVNULL to suppress "not found" noise
            rc = subprocess.call(
                ["taskkill", "/F", "/IM", pname],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
            if rc == 0:
                killed.append(pname)
        except (OSError, subprocess.TimeoutExpired):
            pass
    return killed


def _check_o_files_locked(build_dir):
    """Walk the lib* subdirectories and check if any .o file is locked.

    Returns a list of (relative_path) strings for locked files.
    """
    locked = []
    if not os.path.isdir(build_dir):
        return locked

    for dirpath, _dirnames, filenames in os.walk(build_dir):
        for fname in filenames:
            if not fname.endswith(".o"):
                continue
            fpath = os.path.join(dirpath, fname)
            try:
                with open(fpath, "r+b"):
                    pass
            except (IOError, OSError, PermissionError):
                locked.append(os.path.relpath(fpath, build_dir))
    return locked


def _check_bin_locked(build_dir):
    """Check if critical .bin files are locked by another process."""
    critical_files = ["bootloader.bin", "firmware.bin", "partitions.bin"]
    locked = []
    for fname in critical_files:
        fpath = os.path.join(build_dir, fname)
        if os.path.exists(fpath):
            try:
                with open(fpath, "r+b"):
                    pass
            except (IOError, OSError, PermissionError):
                locked.append(fname)
    return locked


# ---------------------------------------------------------------------------
# Phase 1 (runs at script load, before any build action):
#   Kill stale compiler processes
# ---------------------------------------------------------------------------
if sys.platform == "win32":
    build_dir = env.subst("$BUILD_DIR")  # type: ignore[name-defined]

    killed = _taskkill_on_windows(STALE_COMPILER_PROCESSES)
    if killed:
        print(f"[FastBee] Killed {len(killed)} stale compiler process(es): {', '.join(killed)}")

    # After killing, verify no .o files remain locked
    locked_o = _check_o_files_locked(build_dir)
    if locked_o:
        print("\n" + "=" * 60)
        print(f"[FastBee] WARNING: {len(locked_o)} .o file(s) still locked after killing stale processes:")
        for f in locked_o[:10]:
            print(f"  - {f}")
        if len(locked_o) > 10:
            print(f"  ... and {len(locked_o) - 10} more")
        print("\nFix: Close any antivirus / indexer scanning the build directory,")
        print("     then delete the build folder and rebuild:")
        print(f"     Remove-Item -Recurse -Force \"{build_dir}\"")
        print("=" * 60 + "\n")

# ---------------------------------------------------------------------------
# Phase 2 (runs before firmware.bin linking, as SCons pre-action):
#   Check .bin lock from Serial Monitor / esptool
# ---------------------------------------------------------------------------
def _pre_link_bin_check(source, target, env):
    """Pre-link hook: warn if .bin files are locked by Serial Monitor."""
    build_dir = env.subst("$BUILD_DIR")
    locked = _check_bin_locked(build_dir)
    if locked:
        print("\n" + "=" * 60)
        print("[FastBee] WARNING: The following .bin files are locked:")
        for f in locked:
            print(f"  - {f}")
        print("\nFix: Close Serial Monitor or run:")
        print("     taskkill /F /IM esptool.exe")
        print("=" * 60 + "\n")


if sys.platform == "win32":
    env.AddPreAction("$BUILD_DIR/firmware.bin", _pre_link_bin_check)  # type: ignore[name-defined]
