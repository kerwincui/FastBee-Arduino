"""Execute the production post-link hook with the SCons environment injected.

No firmware compilation, device access, or SDK mutation is needed here.
"""
from pathlib import Path
import runpy
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/enable-project-lto.py"


class ScriptEnvironment(dict):
    def Replace(self, **values):
        self.update(values)

    def AppendUnique(self, **values):
        for name, incoming in values.items():
            current = list(self.get(name, []))
            for value in incoming:
                if value not in current:
                    current.append(value)
            self[name] = current


def run_actual_hook(environment):
    imported = []

    def import_environment(*names):
        imported.extend(names)
        if names != ("env",):
            raise AssertionError("Unexpected SCons import: " + repr(names))

    runpy.run_path(str(SCRIPT), init_globals={"env": environment, "Import": import_environment})
    if imported != ["env"]:
        raise AssertionError("Production hook did not import exactly its injected environment")
    return environment


class ProjectLtoHookTests(unittest.TestCase):
    def assert_lto(self, environment):
        flags = environment["LINKFLAGS"]
        self.assertNotIn("-fno-lto", flags)
        for flag in ("-flto=2", "-fuse-linker-plugin", "-Werror=lto-type-mismatch"):
            self.assertEqual(flags.count(flag), 1)

    def test_real_script_removes_sdk_no_lto_and_preserves_unrelated_link_options(self):
        kept = ["-fexceptions", "-funwind-tables", "-fasynchronous-unwind-tables",
                "-Wl,--eh-frame-hdr", "-Wl,--gc-sections", "-Wl,--no-undefined",
                "-Wl,-fno-lto"]
        environment = ScriptEnvironment(LINKFLAGS=kept[:3] + ["-fno-lto"] + kept[3:])
        run_actual_hook(environment)
        self.assert_lto(environment)
        self.assertEqual([flag for flag in environment["LINKFLAGS"] if flag in kept], kept)

    def test_all_exact_sdk_no_lto_entries_are_removed(self):
        environment = ScriptEnvironment(LINKFLAGS=["-fno-lto", "-Wl,--gc-sections", "-fno-lto"])
        run_actual_hook(environment)
        self.assert_lto(environment)
        self.assertIn("-Wl,--gc-sections", environment["LINKFLAGS"])

    def test_reexecuting_actual_post_hook_is_idempotent(self):
        environment = ScriptEnvironment(LINKFLAGS=["-fno-lto", "-fexceptions", "-flto=2"])
        run_actual_hook(environment)
        first = list(environment["LINKFLAGS"])
        run_actual_hook(environment)
        self.assertEqual(environment["LINKFLAGS"], first)
        self.assert_lto(environment)

    def test_post_link_hook_does_not_rewrite_compile_flags_or_other_environment(self):
        environment = ScriptEnvironment(
            LINKFLAGS=["-fno-lto"], CCFLAGS=["-Os", "-fexceptions", "-funwind-tables"],
            CXXFLAGS=["-std=gnu++17"], CPPDEFINES=["FEATURE_ONE"], LIBS=["wifi"])
        before = {key: list(value) for key, value in environment.items() if key != "LINKFLAGS"}
        run_actual_hook(environment)
        self.assert_lto(environment)
        self.assertEqual({key: environment[key] for key in before}, before)

    def test_empty_and_independent_environments_do_not_share_mutable_flags(self):
        first, second = ScriptEnvironment(), ScriptEnvironment(LINKFLAGS=["-fexceptions"])
        run_actual_hook(first)
        first["LINKFLAGS"].append("-Wl,--first-only")
        run_actual_hook(second)
        self.assert_lto(first)
        self.assert_lto(second)
        self.assertNotIn("-Wl,--first-only", second["LINKFLAGS"])
        self.assertIn("-fexceptions", second["LINKFLAGS"])


if __name__ == "__main__":
    unittest.main()
