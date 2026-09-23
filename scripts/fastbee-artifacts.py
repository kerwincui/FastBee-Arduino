"""
PlatformIO hook for FastBee filesystem and release artifacts.

Responsibilities:
1. Build capability-matched web filesystem images.
2. Build a clean staging directory before buildfs/uploadfs.
3. Copy the latest per-environment binaries into dist/firmware/<env>/.
4. Generate merged factory images with esptool when the required pieces exist.
"""

import csv
import json
import os
import secrets
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone

from SCons.Script import COMMAND_LINE_TARGETS  # type: ignore

Import("env")  # type: ignore[name-defined]


FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}


def project_dir():
    return env.subst("$PROJECT_DIR")


def pio_env_name():
    return env.subst("$PIOENV")


def configured_flash_size():
    """Return the flash size PlatformIO encodes into bootable image headers."""
    value = env.BoardConfig().get("upload.flash_size", "4MB")
    return str(value).strip() or "4MB"





def resolved_build_flags():
    """Return the BUILD_FLAGS already resolved by the active PlatformIO env."""
    try:
        flags = env.get("BUILD_FLAGS") or []
    except Exception:
        return []
    if isinstance(flags, str):
        flags = flags.split()

    flattened = []
    pending = list(flags)
    while pending:
        flag = pending.pop(0)
        if isinstance(flag, (list, tuple)):
            pending[0:0] = list(flag)
            continue
        text = str(flag).strip()
        if text:
            flattened.append(text)
    return flattened


def write_web_capability_input():
    """Persist the active build capabilities for the Node asset pipeline."""
    handle = tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        newline="\n",
        prefix="fastbee-web-capabilities-",
        suffix=".json",
        delete=False,
    )
    try:
        with handle:
            json.dump(
                {
                    "environment": pio_env_name(),
                    "buildFlags": resolved_build_flags(),
                },
                handle,
                indent=2,
            )
            handle.write("\n")
    except Exception:
        try:
            os.remove(handle.name)
        except OSError:
            pass
        raise
    return handle.name


def has_target(*names):
    active = set(COMMAND_LINE_TARGETS)
    return any(name in active for name in names)


def normalize_path(path):
    return os.path.normpath(env.subst(str(path)))


def project_relative_path(path):
    path = normalize_path(path)
    try:
        rel_path = os.path.relpath(path, project_dir())
    except ValueError:
        return path.replace("\\", "/")

    if rel_path == os.curdir or rel_path.startswith("..{}".format(os.sep)) or os.path.isabs(rel_path):
        return path.replace("\\", "/")
    return rel_path.replace("\\", "/")


def copy_if_exists(src, dest_dir, dest_name=None):
    src = normalize_path(src)
    if not os.path.exists(src):
        return None
    os.makedirs(dest_dir, exist_ok=True)
    dest = os.path.join(dest_dir, dest_name or os.path.basename(src))
    shutil.copy2(src, dest)
    return dest


def resolve_image_path(path, out_dir=None, dest_name=None):
    path = normalize_path(path)
    if os.path.exists(path):
        return path
    if out_dir:
        fallback = os.path.join(out_dir, dest_name or os.path.basename(path))
        if os.path.exists(fallback):
            return fallback
    return None


def parse_int(value):
    if isinstance(value, int):
        return value
    text = str(value).strip()
    if not text:
        raise ValueError("empty integer")
    return int(text, 0)


def partition_table_path():
    return normalize_path(env.subst("$PARTITIONS_TABLE_CSV"))


def find_partition_offset(*subtypes):
    csv_path = partition_table_path()
    if not os.path.exists(csv_path):
        return None

    with open(csv_path, "r", encoding="utf-8") as fp:
        rows = []
        for raw in fp:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            rows.append(line)

    for row in csv.reader(rows):
        cells = [cell.strip() for cell in row]
        if len(cells) < 5:
            continue
        name, part_type, subtype, offset, _size = cells[:5]
        if part_type == "data" and subtype in subtypes:
            return offset
        if name in subtypes:
            return offset
    return None


def validate_firmware_capacity(firmware_path):
    """Check the final BIN, including image headers/digest, before publishing it."""
    slots = []
    with open(partition_table_path(), "r", encoding="utf-8-sig") as fp:
        lines = (raw.split("#", 1)[0].strip() for raw in fp)
        for row in csv.reader(line for line in lines if line):
            cells = [cell.strip() for cell in row]
            if len(cells) < 5 or cells[1].lower() not in ("app", "0", "0x00", "0x0"):
                continue
            # A dedicated test image is not a factory/OTA destination.
            if cells[2].lower() in ("test", "0x20", "32"):
                continue
            size_text = cells[4].upper()
            multiplier = {"K": 1024, "M": 1024 * 1024}.get(size_text[-1:], 1)
            size = parse_int(size_text[:-1] if multiplier != 1 else size_text) * multiplier
            if size <= 0:
                raise RuntimeError("Invalid application partition size: {}".format(cells[0]))
            slots.append((cells[0], size))
    if not slots:
        raise RuntimeError("No factory/OTA application partition found")
    name, capacity = min(slots, key=lambda slot: slot[1])
    actual = os.path.getsize(firmware_path)
    if actual <= 0 or actual > capacity:
        raise RuntimeError(
            "Firmware BIN {} bytes does not fit application partition {} ({} bytes; excess {} bytes)".format(
                actual, name, capacity, max(0, actual - capacity)
            )
        )
    print("[FastBee] Final BIN capacity: {} / {} bytes ({} bytes free)".format(
        actual, capacity, capacity - actual))
    return capacity - actual


def prepare_filesystem_staging():
    if not has_target(*FS_TARGETS):
        return
    if env.get("__FASTBEE_FS_STAGE_READY"):
        return

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    stage_dir = os.path.join(
        project_dir(),
        ".pio",
        "fs-staging",
        "{}-{}-{}-{}".format(
            pio_env_name(),
            stamp,
            os.getpid(),
            secrets.token_hex(4),
        ),
    )
    script = os.path.join(project_dir(), "scripts", "gzip-www.js")
    capability_input = write_web_capability_input()
    cmd = [
        "node",
        script,
        "--no-upload",
        "--no-monitor",
        "--env={}".format(pio_env_name()),
        "--capability-input={}".format(capability_input),
        "--stage-dir={}".format(stage_dir),
    ]

    print("[FastBee] Preparing filesystem staging for {}...".format(pio_env_name()))
    try:
        subprocess.check_call(cmd, cwd=project_dir())
    finally:
        try:
            os.remove(capability_input)
        except OSError:
            pass
    env.Replace(PROJECT_DATA_DIR=stage_dir)
    env.Replace(__FASTBEE_FS_STAGE_READY=True)
    print("[FastBee] PROJECT_DATA_DIR -> {}".format(stage_dir))


def build_dist_dir():
    out_dir = os.path.join(project_dir(), "dist", "firmware", pio_env_name())
    os.makedirs(out_dir, exist_ok=True)
    return out_dir


def archive_web_assets(out_dir):
    """Archive the exact target-specific Web tree used by the filesystem build."""
    stage_root = normalize_path(env.subst("$PROJECT_DATA_DIR"))
    source_www = os.path.join(stage_root, "www")
    if not os.path.isdir(source_www):
        raise RuntimeError("Missing staged Web assets for {}: {}".format(pio_env_name(), source_www))

    verifier = os.path.join(project_dir(), "scripts", "verify-web-release-assets.js")
    subprocess.check_call(
        ["node", verifier, "--env={}".format(pio_env_name()), "--www-dir={}".format(source_www)],
        cwd=project_dir(),
    )

    destination_www = os.path.join(out_dir, "www")
    temp_parent = tempfile.mkdtemp(prefix=".www-archive-", dir=out_dir)
    temp_www = os.path.join(temp_parent, "www")
    try:
        shutil.copytree(source_www, temp_www)
        if os.path.isdir(destination_www):
            shutil.rmtree(destination_www)
        elif os.path.exists(destination_www):
            os.remove(destination_www)
        os.replace(temp_www, destination_www)
    finally:
        shutil.rmtree(temp_parent, ignore_errors=True)
    print("[FastBee] Archived target Web assets: {}".format(destination_www))


def collect_flash_images(firmware_path, include_fs=False, out_dir=None):
    images = []
    for item in env.get("FLASH_EXTRA_IMAGES", []):
        if not item or len(item) != 2:
            continue
        offset, image_path = item
        image_path = resolve_image_path(image_path, out_dir=out_dir)
        if image_path:
            images.append((parse_int(offset), image_path))

    firmware_path = resolve_image_path(firmware_path, out_dir=out_dir, dest_name="firmware.bin")
    if firmware_path:
        images.append((parse_int(env.subst("$ESP32_APP_OFFSET")), firmware_path))

    if include_fs:
        fs_path = os.path.join(normalize_path(env.subst("$BUILD_DIR")), "{}.bin".format(env.subst("$ESP32_FS_IMAGE_NAME")))
        fs_offset = find_partition_offset("spiffs", "fat", "littlefs")
        fs_path = resolve_image_path(fs_path, out_dir=out_dir, dest_name="littlefs.bin")
        if fs_offset and fs_path:
            images.append((parse_int(fs_offset), fs_path))

    deduped = {}
    for offset, image_path in images:
        deduped[offset] = image_path
    return sorted(deduped.items(), key=lambda item: item[0])


def archive_flash_images(out_dir, images):
    """Archive every merge input and return portable manifest entries."""
    entries = []
    used_names = set()
    for offset, image_path in images:
        base_name = os.path.basename(image_path)
        dest_name = base_name
        if dest_name in used_names:
            stem, extension = os.path.splitext(base_name)
            dest_name = "{}-{:x}{}".format(stem, offset, extension)
        used_names.add(dest_name)

        dest_path = os.path.join(out_dir, dest_name)
        if os.path.normcase(os.path.abspath(image_path)) != os.path.normcase(os.path.abspath(dest_path)):
            shutil.copy2(image_path, dest_path)
        entries.append({"offset": hex(offset), "file": dest_name})
    return entries


def esptool_path():
    tool = env.subst("$OBJCOPY")
    if tool and os.path.exists(tool):
        return tool
    platform = env.PioPlatform()
    package_dir = platform.get_package_dir("tool-esptoolpy")
    if not package_dir:
        return None
    candidate = os.path.join(package_dir, "esptool.py")
    return candidate if os.path.exists(candidate) else None


def merge_factory(output_path, firmware_path, include_fs=False):
    images = collect_flash_images(
        firmware_path,
        include_fs=include_fs,
        out_dir=os.path.dirname(output_path),
    )
    if len(images) < 3:
        print("[FastBee] Skip factory merge, missing bootloader/partition/app images")
        return False

    tool = esptool_path()
    if not tool:
        print("[FastBee] Skip factory merge, esptool not found")
        return False

    cmd = [
        env.subst("$PYTHONEXE"),
        tool,
        "--chip",
        env.BoardConfig().get("build.mcu", "esp32"),
        "merge-bin",
        "-o",
        output_path,
        "--flash-mode",
        "keep",
        "--flash-freq",
        "keep",
        "--flash-size",
        configured_flash_size(),
    ]
    for offset, image_path in images:
        cmd.extend([hex(offset), image_path])

    subprocess.check_call(cmd, cwd=project_dir())
    return True


def write_manifest(out_dir, firmware_path, include_fs=False):
    images = collect_flash_images(
        firmware_path,
        include_fs=include_fs,
        out_dir=out_dir,
    )
    manifest = {
        "environment": pio_env_name(),
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "partitionTable": project_relative_path(partition_table_path()),
        "appOffset": env.subst("$ESP32_APP_OFFSET"),
        "filesystemOffset": find_partition_offset("spiffs", "fat", "littlefs"),
        "flashImages": archive_flash_images(out_dir, images),
    }
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as fp:
        json.dump(manifest, fp, indent=2)
        fp.write("\n")


def archive_common_outputs(out_dir):
    build_dir = normalize_path(env.subst("$BUILD_DIR"))
    copy_if_exists(os.path.join(build_dir, "bootloader.bin"), out_dir)
    copy_if_exists(os.path.join(build_dir, "partitions.bin"), out_dir)
    copy_if_exists(os.path.join(build_dir, "ota_data_initial.bin"), out_dir)


def archive_firmware(target, source, env):
    firmware_path = normalize_path(target[0])
    validate_firmware_capacity(firmware_path)
    out_dir = build_dist_dir()
    archive_common_outputs(out_dir)
    copy_if_exists(firmware_path, out_dir, "firmware.bin")
    write_manifest(out_dir, firmware_path, include_fs=False)

    factory_path = os.path.join(out_dir, "factory.bin")
    try:
        if merge_factory(factory_path, firmware_path, include_fs=False):
            print("[FastBee] Archived factory image: {}".format(factory_path))
    except Exception as exc:
        print("[FastBee] WARNING: factory merge failed: {}".format(exc))


def archive_filesystem(target, source, env):
    fs_path = normalize_path(target[0])
    firmware_path = os.path.join(normalize_path(env.subst("$BUILD_DIR")), "{}.bin".format(env.subst("$PROGNAME")))
    available_firmware = resolve_image_path(
        firmware_path,
        out_dir=os.path.join(project_dir(), "dist", "firmware", pio_env_name()),
        dest_name="firmware.bin",
    )
    if available_firmware:
        validate_firmware_capacity(available_firmware)
    out_dir = build_dist_dir()
    archive_common_outputs(out_dir)
    copy_if_exists(fs_path, out_dir, "littlefs.bin")
    copy_if_exists(fs_path, out_dir, "factory-littlefs.bin")
    archive_web_assets(out_dir)
    write_manifest(out_dir, firmware_path, include_fs=True)
    factory_fs_path = os.path.join(out_dir, "factory-with-fs.bin")
    try:
        if merge_factory(factory_fs_path, firmware_path, include_fs=True):
            print("[FastBee] Archived factory+fs image: {}".format(factory_fs_path))
    except Exception as exc:
        print("[FastBee] WARNING: factory+fs merge failed: {}".format(exc))


def install_archive_hooks():
    builders = env.get("BUILDERS", {})
    if "ElfToBin" not in builders:
        return
    if env.get("__FASTBEE_ARTIFACT_HOOKS_READY"):
        return

    firmware_target = "$BUILD_DIR/${PROGNAME}.bin"
    filesystem_target = "$BUILD_DIR/${ESP32_FS_IMAGE_NAME}.bin"
    env.AddPostAction(firmware_target, archive_firmware)
    env.AddPostAction(filesystem_target, archive_filesystem)
    env.Replace(__FASTBEE_ARTIFACT_HOOKS_READY=True)


# PlatformIO injects ``env`` before evaluating extra_scripts. Keeping the
# import-time work guarded also lets host-side unit tests load helper
# functions without a live SCons environment.
if "env" in globals():
    prepare_filesystem_staging()
    install_archive_hooks()
