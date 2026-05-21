#!/usr/bin/env python3
"""Wrap GNU ar on Windows and recover from target archive rename denial."""

import glob
import os
import shutil
import subprocess
import sys


def archive_target(args):
    if len(args) < 2:
        return None
    return args[1]


def newest_temp_archive(target):
    target_dir = os.path.dirname(os.path.abspath(target)) or "."
    target_name = os.path.basename(target)
    candidates = []
    for path in glob.glob(os.path.join(target_dir, "st*")):
        if os.path.basename(path) == target_name or not os.path.isfile(path):
            continue
        try:
            size = os.path.getsize(path)
            mtime = os.path.getmtime(path)
        except OSError:
            continue
        if size > 1024:
            candidates.append((mtime, size, path))
    if not candidates:
        return None
    candidates.sort(reverse=True)
    return candidates[0][2]


def main():
    if len(sys.argv) < 3:
        return 2

    real_ar = sys.argv[1]
    args = sys.argv[2:]
    target = archive_target(args)

    proc = subprocess.run([real_ar] + args, text=True, capture_output=True)
    if proc.stdout:
        sys.stdout.write(proc.stdout)
    if proc.stderr:
        sys.stderr.write(proc.stderr)
    if proc.returncode == 0:
        return 0

    combined = "{}\n{}".format(proc.stdout or "", proc.stderr or "")
    if "unable to rename" not in combined or not target:
        return proc.returncode

    temp_archive = newest_temp_archive(target)
    if not temp_archive:
        return proc.returncode

    try:
        shutil.copyfile(temp_archive, target)
        if os.path.getsize(target) > 1024:
            sys.stderr.write(
                "[platformio-ar-wrapper] recovered archive rename failure: {} <- {}\n".format(
                    target, os.path.basename(temp_archive)
                )
            )
            return 0
    except OSError as exc:
        sys.stderr.write("[platformio-ar-wrapper] recovery failed: {}\n".format(exc))

    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
