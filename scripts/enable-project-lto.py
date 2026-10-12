"""Enable project-local LTO linking without modifying the installed SDK.

The SDK builder appends -fno-lto to LINKFLAGS. This post script overrides only
that option after framework configuration; compilation uses esp32_base flags.
Keep ABI diagnostics fatal: a successful size check cannot hide C/C++ drift.
"""


def configure_project_lto(build_env):
    flags = [flag for flag in build_env.get("LINKFLAGS", []) if str(flag) != "-fno-lto"]
    build_env.Replace(LINKFLAGS=flags)
    build_env.AppendUnique(LINKFLAGS=["-flto=2", "-fuse-linker-plugin", "-Werror=lto-type-mismatch"])


Import("env")
configure_project_lto(env)
