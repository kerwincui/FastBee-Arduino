"""
PlatformIO pre-build hook: prepare toolchains and report locked build files.

On Windows, a failed/interrupted build can leave behind compiler processes
(collect2, cc1, xtensa-esp32*-elf-g++, etc.) that hold locks on .o files
in the build directory.  The next build then fails with:
  "另一个程序正在使用此文件，进程无法访问"

This script runs as a ``pre:`` extra_scripts hook. It prepares the toolchain
PATH, applies the C6 OneWire compatibility patch, and checks .bin / .o files
in the current build directory. Compiler names do not prove project ownership
or that a process is orphaned, so this hook reports locks without terminating
processes. Active builds and other projects may use the same executables.
"""
import os
import sys

Import("env")  # type: ignore[name-defined]

# ---------------------------------------------------------------------------
# Phase 0: Ensure toolchain is in PATH
# The pioarduino platform's idf_tools.py sometimes fails to set PATH
# (especially after toolchain reinstallation). This ensures the compiler
# is always findable regardless of PlatformIO's tool resolution state.
# Works on all OS (Windows/Linux/macOS) and any user.
# ---------------------------------------------------------------------------
def _ensure_toolchain_in_path():
    """Add toolchain bin directories to PATH if not already present."""
    # Respect PLATFORMIO_HOME_DIR env var; fall back to ~/.platformio
    pio_home = os.environ.get(
        "PLATFORMIO_HOME_DIR",
        os.path.join(os.path.expanduser("~"), ".platformio")
    )
    # Xtensa (ESP32, ESP32-S2, ESP32-S3) and RISC-V (ESP32-C3, C6, H2)
    toolchain_dirs = [
        # Xtensa toolchain
        os.path.join(pio_home, "tools", "toolchain-xtensa-esp-elf", "xtensa-esp-elf", "bin"),
        os.path.join(pio_home, "packages", "toolchain-xtensa-esp-elf", "xtensa-esp-elf", "bin"),
        # RISC-V toolchain
        os.path.join(pio_home, "tools", "toolchain-riscv32-esp", "riscv32-esp-elf", "bin"),
        os.path.join(pio_home, "packages", "toolchain-riscv32-esp", "riscv32-esp-elf", "bin"),
    ]
    current_path = os.environ.get("PATH", "")
    for toolchain_bin in toolchain_dirs:
        if os.path.isdir(toolchain_bin) and toolchain_bin not in current_path:
            os.environ["PATH"] = toolchain_bin + os.pathsep + current_path
            current_path = os.environ["PATH"]
            print(f"[FastBee] Toolchain PATH injected: {toolchain_bin}")

_ensure_toolchain_in_path()

# ---------------------------------------------------------------------------
# Phase 0.5: Patch OneWire's legacy ESP32 GPIO access for ESP32-C6.
# OneWire 2.3.8 still treats GPIO registers as plain integers on the C6,
# while Arduino-ESP32 3.x exposes them as unions that must be accessed via
# ``.val``.  Keep the dependency pinned to the registry release and apply
# this small, deterministic compatibility patch after PlatformIO installs it.
# ---------------------------------------------------------------------------
def _patch_onewire_for_esp32c6():
    """Make the vendored OneWire GPIO header compile against C6 register types."""
    try:
        pio_env = env.subst("$PIOENV")  # type: ignore[name-defined]
        if not pio_env.lower().startswith("esp32c6-"):
            return
        project_dir = env.subst("$PROJECT_DIR")  # type: ignore[name-defined]
    except Exception:
        return

    header_path = os.path.join(
        project_dir,
        ".pio",
        "libdeps",
        pio_env,
        "OneWire",
        "util",
        "OneWire_direct_gpio.h",
    )
    if not os.path.isfile(header_path):
        print(f"[FastBee] WARNING: C6 OneWire header not found: {header_path}")
        return

    try:
        with open(header_path, "r", encoding="utf-8") as header_file:
            source = header_file.read()
        marker = "CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6"
        if marker in source:
            return
        updated = source.replace(
            "CONFIG_IDF_TARGET_ESP32C3",
            marker,
        )
        if updated == source:
            print(f"[FastBee] WARNING: C6 OneWire compatibility marker not found: {header_path}")
            return
        with open(header_path, "w", encoding="utf-8", newline="") as header_file:
            header_file.write(updated)
        print(f"[FastBee] Applied ESP32-C6 OneWire GPIO compatibility patch: {header_path}")
    except (OSError, UnicodeError) as exc:
        print(f"[FastBee] WARNING: failed to patch C6 OneWire header: {exc}")


_patch_onewire_for_esp32c6()

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
#   Diagnose locked objects in this build directory without stopping processes.
# ---------------------------------------------------------------------------
if sys.platform == "win32":
    build_dir = env.subst("$BUILD_DIR")  # type: ignore[name-defined]

    locked_o = _check_o_files_locked(build_dir)
    if locked_o:
        print("\n" + "=" * 60)
        print(f"[FastBee] WARNING: {len(locked_o)} .o file(s) locked in the current build directory:")
        for f in locked_o[:10]:
            print(f"  - {f}")
        if len(locked_o) > 10:
            print(f"  ... and {len(locked_o) - 10} more")
        print("\nFix: Wait for active builds using this build directory to finish.")
        print("     Confirm a stale owner's PID and project before stopping it.")
        print(f"     Build directory: {build_dir}")
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
        print("\nFix: Close this project's Serial Monitor or wait for its upload to finish.")
        print("     Confirm a stale owner's PID and project before stopping it.")
        print(f"     Build directory: {build_dir}")
        print("=" * 60 + "\n")


if sys.platform == "win32":
    env.AddPreAction("$BUILD_DIR/firmware.bin", _pre_link_bin_check)  # type: ignore[name-defined]
