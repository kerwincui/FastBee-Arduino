import builtins
import contextlib
import io
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "scripts" / "kill-stale-processes.py"


class FakeBuildEnv:
    def __init__(self, project_dir, build_dir, pio_env):
        self.values = {
            "$PROJECT_DIR": str(project_dir),
            "$BUILD_DIR": str(build_dir),
            "$PIOENV": pio_env,
        }
        self.pre_actions = []

    def subst(self, value):
        return self.values[value]

    def AddPreAction(self, target, callback):
        self.pre_actions.append((target, callback))


class PrebuildProcessSafetyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="fastbee-prebuild-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.project = self.root / "project"
        self.build = self.project / ".pio" / "build" / "esp32-F8R0"
        self.pio_home = self.root / "platformio"
        self.build.mkdir(parents=True)
        self.pio_home.mkdir()

    def _run_hook(self, *, pio_env="esp32-F8R0", platform="win32",
                  path_value="original-bin", locked=(), run_pre_link=False):
        env = FakeBuildEnv(self.project, self.build, pio_env)
        output = io.StringIO()
        checked_files = []
        locked_paths = {Path(path).resolve() for path in locked}
        real_open = builtins.open

        def open_with_locks(file, mode="r", *args, **kwargs):
            if mode == "r+b":
                path = Path(file).resolve()
                checked_files.append(path)
                if path in locked_paths:
                    raise PermissionError("File held by a running build or upload")
            return real_open(file, mode, *args, **kwargs)

        # Running compilers may belong to this project, host tests, or another
        # checkout. Loading a hook must leave all of them alone, even on a lock.
        process_apis = (
            "subprocess.call", "subprocess.run", "subprocess.Popen",
            "subprocess.check_call", "subprocess.check_output", "os.kill",
            "os.system",
        )
        with contextlib.ExitStack() as stack:
            process_mocks = [
                stack.enter_context(mock.patch(
                    api, side_effect=AssertionError("Unexpected process action: " + api)
                ))
                for api in process_apis
            ]
            stack.enter_context(mock.patch("sys.platform", platform))
            stack.enter_context(mock.patch.dict(os.environ, {
                "PLATFORMIO_HOME_DIR": str(self.pio_home), "PATH": path_value,
            }))
            stack.enter_context(mock.patch("builtins.open", side_effect=open_with_locks))
            stack.enter_context(contextlib.redirect_stdout(output))
            runpy.run_path(str(HOOK), init_globals={"env": env, "Import": lambda _: None})
            if run_pre_link:
                for _, callback in env.pre_actions:
                    callback([], [], env)
            resulting_path = os.environ["PATH"]
            for process_mock in process_mocks:
                process_mock.assert_not_called()

        return env, output.getvalue(), checked_files, resulting_path

    def _create_file(self, relative_path, contents="build artifact"):
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
        return path

    def test_default_load_preserves_active_and_external_compiler_processes(self):
        env, output, checked, _ = self._run_hook()
        self.assertEqual(len(env.pre_actions), 1)
        self.assertEqual(env.pre_actions[0][0], "$BUILD_DIR/firmware.bin")
        self.assertEqual(output, "")
        self.assertEqual(checked, [])

    def test_locked_objects_are_reported_only_in_current_build_without_killing(self):
        current = self._create_file("project/.pio/build/esp32-F8R0/lib123/active.o")
        unrelated = self._create_file("project/.pio/build/other-env/lib456/external.o")
        env, output, checked, _ = self._run_hook(locked=[current, unrelated])
        self.assertEqual(checked, [current.resolve()])
        self.assertIn("1 .o file(s) locked", output)
        self.assertIn("active.o", output)
        self.assertIn(str(self.build), output)
        self.assertNotIn("external.o", output)
        self.assertNotIn("taskkill", output)
        self.assertNotIn("Remove-Item", output)
        self.assertEqual(len(env.pre_actions), 1)

    def test_pre_link_hook_checks_current_bin_files_without_terminating_uploads(self):
        current = self._create_file("project/.pio/build/esp32-F8R0/firmware.bin")
        bootloader = self._create_file("project/.pio/build/esp32-F8R0/bootloader.bin")
        unrelated = self._create_file("project/.pio/build/other-env/partitions.bin")
        _, output, checked, _ = self._run_hook(
            locked=[current, unrelated], run_pre_link=True,
        )
        self.assertCountEqual(checked, [current.resolve(), bootloader.resolve()])
        self.assertIn("following .bin files are locked", output)
        self.assertIn("  - firmware.bin", output)
        self.assertNotIn("  - bootloader.bin", output)
        self.assertNotIn("partitions.bin", output)
        self.assertIn(str(self.build), output)
        self.assertNotIn("taskkill", output)

    def test_toolchain_path_injection_is_preserved_and_idempotent(self):
        toolchains = [
            self.pio_home / "packages/toolchain-xtensa-esp-elf/xtensa-esp-elf/bin",
            self.pio_home / "tools/toolchain-riscv32-esp/riscv32-esp-elf/bin",
        ]
        for directory in toolchains:
            directory.mkdir(parents=True)
        _, output, _, updated_path = self._run_hook()
        self.assertEqual(output.count("Toolchain PATH injected:"), 2)
        self.assertEqual(updated_path.split(os.pathsep)[-1], "original-bin")
        for directory in toolchains:
            self.assertEqual(updated_path.split(os.pathsep).count(str(directory)), 1)
        _, repeated_output, _, repeated_path = self._run_hook(path_value=updated_path)
        self.assertEqual(repeated_path, updated_path)
        self.assertNotIn("Toolchain PATH injected:", repeated_output)

    def test_c6_onewire_patch_is_preserved_and_idempotent(self):
        original = "#if CONFIG_IDF_TARGET_ESP32C3\n#define GPIO_READ(reg) reg.val\n#endif\n"
        header = self._create_file(
            "project/.pio/libdeps/esp32c6-F4R0/OneWire/util/OneWire_direct_gpio.h", original,
        )
        _, output, _, _ = self._run_hook(pio_env="esp32c6-F4R0")
        self.assertIn("Applied ESP32-C6 OneWire GPIO compatibility patch", output)
        expected = original.replace(
            "CONFIG_IDF_TARGET_ESP32C3", "CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6",
        )
        self.assertEqual(header.read_text(encoding="utf-8"), expected)
        modified_bytes = header.read_bytes()
        _, repeated_output, _, _ = self._run_hook(pio_env="esp32c6-F4R0")
        self.assertEqual(header.read_bytes(), modified_bytes)
        self.assertNotIn("Applied ESP32-C6", repeated_output)

    def test_non_c6_environment_does_not_modify_onewire_header(self):
        original = "#if CONFIG_IDF_TARGET_ESP32C3\n#endif\n"
        header = self._create_file(
            "project/.pio/libdeps/esp32-F8R0/OneWire/util/OneWire_direct_gpio.h", original,
        )
        self._run_hook()
        self.assertEqual(header.read_text(encoding="utf-8"), original)

    def test_non_windows_load_skips_windows_lock_diagnostics(self):
        current = self._create_file("project/.pio/build/esp32-F8R0/lib123/active.o")
        env, output, checked, _ = self._run_hook(platform="linux", locked=[current])
        self.assertEqual(env.pre_actions, [])
        self.assertEqual(output, "")
        self.assertEqual(checked, [])


if __name__ == "__main__":
    unittest.main()
