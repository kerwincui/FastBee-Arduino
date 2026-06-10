"""
PlatformIO hook for FastBee filesystem profiles and release artifacts.

Responsibilities:
1. Map each PlatformIO environment to a Lite/Standard/Full filesystem profile.
2. Build a clean profile-specific staging directory before buildfs/uploadfs.
3. Copy the latest per-environment binaries into dist/firmware/<env>/.
4. Generate merged factory images with esptool when the required pieces exist.
"""

import csv
import json
import os
import shutil
import subprocess
from datetime import datetime, timezone

from SCons.Script import COMMAND_LINE_TARGETS  # type: ignore

Import("env")  # type: ignore[name-defined]


ENV_TO_PROFILE = {
    "esp32c3": "lite",
    "esp32c6": "lite",
    "esp32": "standard",
    "esp32s3": "standard",
    "esp32s3-full": "full",
}

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}


def project_dir():
    return env.subst("$PROJECT_DIR")


def pio_env_name():
    return env.subst("$PIOENV")


def profile_for_env():
    return ENV_TO_PROFILE.get(pio_env_name(), "full")


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


def prepare_filesystem_staging():
    if not has_target(*FS_TARGETS):
        return
    if env.get("__FASTBEE_FS_STAGE_READY"):
        return

    profile = profile_for_env()
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    stage_dir = os.path.join(
        project_dir(),
        ".pio",
        "fs-staging",
        "{}-{}-{}".format(profile, pio_env_name(), stamp),
    )
    script = os.path.join(project_dir(), "scripts", "gzip-www.js")
    cmd = [
        "node",
        script,
        "--no-upload",
        "--no-monitor",
        "--web-profile={}".format(profile),
        "--stage-dir={}".format(stage_dir),
    ]

    print("[FastBee] Preparing {} filesystem staging for {}...".format(profile, pio_env_name()))
    subprocess.check_call(cmd, cwd=project_dir())
    env.Replace(PROJECT_DATA_DIR=stage_dir)
    env.Replace(__FASTBEE_FS_STAGE_READY=True)
    print("[FastBee] PROJECT_DATA_DIR -> {}".format(stage_dir))


def build_dist_dir():
    out_dir = os.path.join(project_dir(), "dist", "firmware", pio_env_name())
    os.makedirs(out_dir, exist_ok=True)
    return out_dir


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
        "keep",
    ]
    for offset, image_path in images:
        cmd.extend([hex(offset), image_path])

    subprocess.check_call(cmd, cwd=project_dir())
    return True


def write_manifest(out_dir):
    manifest = {
        "environment": pio_env_name(),
        "filesystemProfile": profile_for_env(),
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "partitionTable": project_relative_path(partition_table_path()),
        "appOffset": env.subst("$ESP32_APP_OFFSET"),
        "filesystemOffset": find_partition_offset("spiffs", "fat", "littlefs"),
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
    out_dir = build_dist_dir()
    archive_common_outputs(out_dir)
    copy_if_exists(firmware_path, out_dir, "firmware.bin")
    write_manifest(out_dir)

    factory_path = os.path.join(out_dir, "factory.bin")
    try:
        if merge_factory(factory_path, firmware_path, include_fs=False):
            print("[FastBee] Archived factory image: {}".format(factory_path))
    except Exception as exc:
        print("[FastBee] WARNING: factory merge failed: {}".format(exc))


def archive_filesystem(target, source, env):
    fs_path = normalize_path(target[0])
    out_dir = build_dist_dir()
    archive_common_outputs(out_dir)
    copy_if_exists(fs_path, out_dir, "littlefs.bin")
    copy_if_exists(fs_path, out_dir, "{}-littlefs.bin".format(profile_for_env()))
    write_manifest(out_dir)

    firmware_path = os.path.join(normalize_path(env.subst("$BUILD_DIR")), "{}.bin".format(env.subst("$PROGNAME")))
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


prepare_filesystem_staging()
install_archive_hooks()
