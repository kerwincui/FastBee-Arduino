"""
PlatformIO/SCons hook for Windows archive rename failures.

Some Windows environments block xtensa-esp32-elf-ar.exe when it replaces the
target archive with its temporary `st*` file. The archive content is already
written correctly, so this hook swaps in a small wrapper that copies the newest
temporary archive to the target only for that specific failure.
"""

import os
import sys

Import("env")  # type: ignore[name-defined]

wrapper = os.path.join(env.subst("$PROJECT_DIR"), "scripts", "platformio_ar_wrapper.py")


def patch_archive_tool(build_env):
    real_ar = build_env.subst("$AR")
    ar_name = os.path.basename(real_ar).lower()
    if os.name != "nt" or "ar" not in ar_name or "platformio_ar_wrapper.py" in real_ar:
        return
    build_env.Replace(AR='"{}" "{}" "{}"'.format(sys.executable, wrapper, real_ar))


def install_build_library_hook(build_env):
    if build_env.get("__FASTBEE_AR_BUILD_LIBRARY_HOOK"):
        return
    try:
        from platformio.builder.tools import piobuild
    except Exception:
        return

    def BuildLibraryWithArchivePatch(current_env, variant_dir, src_dir, src_filter=None, nodes=None):
        patch_archive_tool(current_env)
        return piobuild.BuildLibrary(current_env, variant_dir, src_dir, src_filter, nodes)

    build_env.AddMethod(BuildLibraryWithArchivePatch, "BuildLibrary")
    build_env.Replace(__FASTBEE_AR_BUILD_LIBRARY_HOOK=True)


install_build_library_hook(env)
patch_archive_tool(env)

if env.get("__PIO_LIB_BUILDERS", None) is not None:
    for lib_builder in env.GetLibBuilders():
        install_build_library_hook(lib_builder.env)
        patch_archive_tool(lib_builder.env)
