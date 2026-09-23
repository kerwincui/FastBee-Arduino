"""
Unit Tests for fastbee-artifacts.py plugin hooks
Purpose: Validate filesystem detection and artifact merging logic
"""

import unittest
import csv
import json
import os
import tempfile
import types
from unittest.mock import MagicMock, patch


class TestFastBeeArtifacts(unittest.TestCase):
    """Test suite for FastBee artifacts management"""

    @staticmethod
    def load_hook():
        # PlatformIO normally injects SCons and Import before loading extra_scripts.
        import builtins
        import importlib.util
        import sys
        import types

        scons_script = types.ModuleType('SCons.Script')
        scons_script.COMMAND_LINE_TARGETS = []
        scons = types.ModuleType('SCons')
        scons.Script = scons_script
        sys.modules['SCons'] = scons
        sys.modules['SCons.Script'] = scons_script
        builtins.Import = lambda name: None
        hook_path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.dirname(__file__))),
            'scripts',
            'fastbee-artifacts.py',
        )
        spec = importlib.util.spec_from_file_location('fastbee_artifacts_hook', hook_path)
        hook = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(hook)
        return hook

    def test_partition_parser_detects_littlefs(self):
        """Validate that partition table parser correctly identifies LittleFS partitions"""
        
        # Simulate a correct partition table
        mock_csv = """# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x2F0000,
littlefs, data, littlefs,0x2f0000,0x100000,
"""
        
        # Parse the CSV
        rows = []
        for line in mock_csv.strip().split('\n'):
            if line.startswith('#') or not line.strip():
                continue
            rows.append(line.split(','))
        
        # Find LittleFS partition
        littlefs_offset = None
        for row in rows:
            if len(row) >= 4 and row[0].strip() == 'littlefs' and row[2].strip() == 'littlefs':
                littlefs_offset = int(row[3].strip(), 16)
                break
        
        self.assertIsNotNone(littlefs_offset, "LittleFS partition should be found")
        self.assertEqual(littlefs_offset, 0x2f0000, "LittleFS offset should match")

    def test_final_binary_capacity_checks_smallest_ota_slot(self):
        hook = self.load_hook()
        with tempfile.TemporaryDirectory() as directory:
            table = os.path.join(directory, "partitions.csv")
            firmware = os.path.join(directory, "firmware.bin")
            with open(table, "w", encoding="utf-8") as fp:
                fp.write("# app slot may be smaller than the ELF load-size budget\n"
                         "test,app,test,0x10000,1K\n"
                         "app0, app, ota_0, 0x20000, 4K\n"
                         "app1, 0x00, 0x11, 0x30000, 0x800 # smallest slot\n")
            hook.partition_table_path = lambda: table
            with open(firmware, "wb") as fp:
                fp.truncate(2048)
            self.assertEqual(hook.validate_firmware_capacity(firmware), 0)
            with open(firmware, "ab") as fp:
                fp.write(b"x")
            with self.assertRaisesRegex(RuntimeError, r"app1.*excess 1 bytes"):
                hook.validate_firmware_capacity(firmware)

    def test_oversized_binary_does_not_replace_published_artifacts(self):
        hook = self.load_hook()
        hook.env = MagicMock()
        hook.env.subst.side_effect = str
        hook.build_dist_dir = MagicMock()
        hook.archive_common_outputs = MagicMock()
        hook.copy_if_exists = MagicMock()
        hook.validate_firmware_capacity = MagicMock(side_effect=RuntimeError("overflow"))
        with self.assertRaisesRegex(RuntimeError, "overflow"):
            hook.archive_firmware(["firmware.bin"], [], hook.env)
        hook.build_dist_dir.assert_not_called()
        hook.archive_common_outputs.assert_not_called()
        hook.copy_if_exists.assert_not_called()

    def test_capacity_rejects_empty_image_and_missing_application_slot(self):
        hook = self.load_hook()
        with tempfile.TemporaryDirectory() as directory:
            table = os.path.join(directory, "partitions.csv")
            firmware = os.path.join(directory, "firmware.bin")
            with open(firmware, "wb"):
                pass
            hook.partition_table_path = lambda: table
            with open(table, "w", encoding="utf-8") as fp:
                fp.write("app0,app,factory,0x10000,1M\n")
            with self.assertRaisesRegex(RuntimeError, "BIN 0 bytes"):
                hook.validate_firmware_capacity(firmware)
            with open(table, "w", encoding="utf-8") as fp:
                fp.write("nvs,data,nvs,0x9000,0x5000\n")
            with self.assertRaisesRegex(RuntimeError, "No factory/OTA"):
                hook.validate_firmware_capacity(firmware)

    def test_partition_parser_rejects_spiffs(self):
        """Ensure deprecated SPiFFS filesystem is detected as error"""
        
        mock_csv_with_spiffs = """# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,  0x5000,
spiffs,   data, spiffs,  0x300000,0x100000,
"""
        
        rows = []
        for line in mock_csv_with_spiffs.strip().split('\n'):
            if line.startswith('#') or not line.strip():
                continue
            rows.append(line.split(','))
        
        spiffs_found = False
        for row in rows:
            if len(row) >= 3 and row[2].strip() == 'spiffs':
                spiffs_found = True
                break
        
        self.assertTrue(spiffs_found, "SPiFFS partition should be detected")

    def test_merge_bin_uses_configured_flash_size(self):
        """Merged factory images must encode the active board flash size."""

        hook = self.load_hook()
        images = [
            (0x1000, '/path/to/bootloader.bin'),
            (0x8000, '/path/to/partitions.bin'),
            (0x10000, '/path/to/firmware.factory.bin'),
        ]
        hook.collect_flash_images = MagicMock(return_value=images)
        hook.esptool_path = MagicMock(return_value='esptool.py')
        hook.env = MagicMock()
        hook.env.subst.side_effect = lambda value: {
            '$PYTHONEXE': 'python',
            '$PROJECT_DIR': '/project',
        }.get(value, value)
        hook.env.BoardConfig.return_value = {
            'build.mcu': 'esp32',
            'upload.flash_size': '8MB',
        }

        with patch.object(hook.subprocess, 'check_call') as check_call:
            self.assertTrue(hook.merge_factory('factory.bin', 'firmware.bin'))

        command = check_call.call_args.args[0]
        flash_size_index = command.index('--flash-size')
        self.assertEqual(command[flash_size_index + 1], '8MB')
        self.assertIn('merge-bin', command)
        check_call.assert_called_once_with(command, cwd='/project')

    @patch('os.path.exists')
    def test_esptool_path_detection(self, mock_exists):
        """Test that esptool path is correctly resolved"""

        hook = self.load_hook()
        hook.env = types.SimpleNamespace(
            subst=lambda value: 'esptool.py' if value == '$OBJCOPY' else '',
            PioPlatform=lambda: types.SimpleNamespace(get_package_dir=lambda name: None)
        )
        esptool_path = hook.esptool_path
        
        mock_exists.return_value = True
        
        # Should return valid path when exists
        result = esptool_path()
        self.assertIsNotNone(result)

    def test_manifest_json_structure(self):
        """Manifest must archive and describe the exact collected flash inputs."""

        hook = self.load_hook()
        with tempfile.TemporaryDirectory() as temp_dir:
            source_dir = os.path.join(temp_dir, 'source')
            output_dir = os.path.join(temp_dir, 'output')
            os.makedirs(source_dir)
            os.makedirs(output_dir)
            names_and_offsets = [
                ('bootloader.bin', 0x2000),
                ('partitions.bin', 0x9000),
                ('firmware.bin', 0x18000),
                ('littlefs.bin', 0x350000),
            ]
            images = []
            for name, offset in names_and_offsets:
                source = os.path.join(source_dir, name)
                with open(source, 'wb') as fp:
                    fp.write(('payload-' + name).encode('ascii'))
                images.append((offset, source))

            hook.collect_flash_images = MagicMock(return_value=images)
            hook.pio_env_name = MagicMock(return_value='esp32c3-F4R0')
            hook.partition_table_path = MagicMock(return_value='partitions/4MB.csv')
            hook.project_relative_path = MagicMock(return_value='partitions/4MB.csv')
            hook.find_partition_offset = MagicMock(return_value='0x350000')
            hook.env = MagicMock()
            hook.env.subst.side_effect = lambda value: '0x18000' if value == '$ESP32_APP_OFFSET' else value

            hook.write_manifest(output_dir, images[2][1], include_fs=True)
            with open(os.path.join(output_dir, 'manifest.json'), 'r', encoding='utf-8') as fp:
                manifest = json.load(fp)

            self.assertEqual(
                manifest['flashImages'],
                [
                    {'offset': hex(offset), 'file': name}
                    for name, offset in names_and_offsets
                ],
            )
            for name, _offset in names_and_offsets:
                with open(os.path.join(output_dir, name), 'rb') as archived:
                    self.assertEqual(archived.read(), ('payload-' + name).encode('ascii'))
            hook.collect_flash_images.assert_called_once_with(
                images[2][1],
                include_fs=True,
                out_dir=output_dir,
            )

    def test_build_flag_parsing(self):
        """The hook must preserve the active PlatformIO BUILD_FLAGS."""

        hook = self.load_hook()
        hook.env = MagicMock()
        hook.env.get.return_value = [
            "-DFASTBEE_ENABLE_LCD=1",
            ["-DFASTBEE_USE_PSRAM=0", "-DFASTBEE_ENABLE_SENSOR_DRIVER=1"],
        ]

        self.assertEqual(
            hook.resolved_build_flags(),
            [
                "-DFASTBEE_ENABLE_LCD=1",
                "-DFASTBEE_USE_PSRAM=0",
                "-DFASTBEE_ENABLE_SENSOR_DRIVER=1",
            ],
        )

    def test_filesystem_staging_forwards_resolved_capabilities(self):
        """Gzip build receives env and a temporary JSON with resolved flags."""

        hook = self.load_hook()
        build_flags = [
            "-DFASTBEE_ENABLE_WEB_SERVER=1",
            "-DFASTBEE_ENABLE_DEVICE_CONTROL=0",
            "-DFASTBEE_ENABLE_MODBUS=1",
        ]

        class FakeEnv:
            def __init__(self, root):
                self.root = root
                self.values = {
                    "BUILD_FLAGS": build_flags,
                    "__FASTBEE_FS_STAGE_READY": False,
                }
                self.replacements = {}

            def subst(self, value):
                return {
                    "$PROJECT_DIR": self.root,
                    "$PIOENV": "esp32c3-F4R0",
                }.get(value, value)

            def get(self, key, default=None):
                return self.values.get(key, default)

            def Replace(self, **kwargs):
                self.replacements.update(kwargs)
                self.values.update(kwargs)

        observed = {}

        def capture_command(command, cwd):
            observed["command"] = list(command)
            observed["cwd"] = cwd
            capability_arg = next(
                item for item in command if item.startswith("--capability-input=")
            )
            capability_path = capability_arg.split("=", 1)[1]
            observed["capability_path"] = capability_path
            with open(capability_path, "r", encoding="utf-8") as fp:
                observed["capability_input"] = json.load(fp)

        with tempfile.TemporaryDirectory() as temp_dir:
            hook.env = FakeEnv(temp_dir)
            hook.COMMAND_LINE_TARGETS = ["buildfs"]
            with patch.object(hook.subprocess, "check_call", side_effect=capture_command) as check_call:
                hook.prepare_filesystem_staging()

            command = observed["command"]
            self.assertEqual(observed["cwd"], temp_dir)
            self.assertIn("--env=esp32c3-F4R0", command)
            self.assertEqual(
                observed["capability_input"],
                {
                    "environment": "esp32c3-F4R0",
                    "buildFlags": build_flags,
                },
            )
            self.assertFalse(any(arg.startswith("--web-profile") for arg in command))
            self.assertNotIn("--device-control", command)
            self.assertFalse(os.path.exists(observed["capability_path"]))
            self.assertTrue(
                hook.env.replacements["PROJECT_DATA_DIR"].startswith(
                    os.path.join(temp_dir, ".pio", "fs-staging", "esp32c3-F4R0-")
                )
            )
            self.assertTrue(hook.env.replacements["__FASTBEE_FS_STAGE_READY"])
            check_call.assert_called_once()

            hook.env.values["__FASTBEE_FS_STAGE_READY"] = False
            failed = {}

            def fail_command(command, cwd):
                capability_arg = next(
                    item for item in command if item.startswith("--capability-input=")
                )
                failed["capability_path"] = capability_arg.split("=", 1)[1]
                raise hook.subprocess.CalledProcessError(1, command)

            with patch.object(hook.subprocess, "check_call", side_effect=fail_command):
                with self.assertRaises(hook.subprocess.CalledProcessError):
                    hook.prepare_filesystem_staging()

            self.assertFalse(os.path.exists(failed["capability_path"]))
            self.assertFalse(hook.env.values["__FASTBEE_FS_STAGE_READY"])

    def test_web_archive_copies_exact_staging_tree_and_verifies_environment(self):
        """The dist Web tree must come from the active buildfs staging directory."""

        hook = self.load_hook()
        with tempfile.TemporaryDirectory() as temp_dir:
            stage_dir = os.path.join(temp_dir, "stage")
            source_www = os.path.join(stage_dir, "www")
            output_dir = os.path.join(temp_dir, "dist", "firmware", "esp32-F4R0")
            os.makedirs(os.path.join(source_www, "js"), exist_ok=True)
            os.makedirs(output_dir, exist_ok=True)
            with open(os.path.join(source_www, "web-resource-manifest.json"), "w", encoding="utf-8") as fp:
                fp.write('{"environment":"esp32-F4R0"}')
            with open(os.path.join(source_www, "js", "chunk-1-core.js.gz"), "wb") as fp:
                fp.write(b"target-specific-web")
            stale_dir = os.path.join(output_dir, "www")
            os.makedirs(stale_dir)
            with open(os.path.join(stale_dir, "stale.txt"), "w", encoding="utf-8") as fp:
                fp.write("stale")

            hook.env = MagicMock()
            hook.env.subst.side_effect = lambda value: {
                "$PROJECT_DATA_DIR": stage_dir,
                "$PROJECT_DIR": temp_dir,
                "$PIOENV": "esp32-F4R0",
            }.get(value, value)
            with patch.object(hook.subprocess, "check_call") as check_call:
                hook.archive_web_assets(output_dir)

            destination = os.path.join(output_dir, "www")
            self.assertFalse(os.path.exists(os.path.join(destination, "stale.txt")))
            with open(os.path.join(destination, "js", "chunk-1-core.js.gz"), "rb") as fp:
                self.assertEqual(fp.read(), b"target-specific-web")
            command = check_call.call_args.args[0]
            self.assertEqual(command[0], "node")
            self.assertIn("--env=esp32-F4R0", command)
            self.assertIn("--www-dir={}".format(source_www), command)
            self.assertEqual(check_call.call_args.kwargs["cwd"], temp_dir)


class TestPartitionTableValidator(unittest.TestCase):
    """Validation tests for partition table integrity"""

    @staticmethod
    def parse_partition_csv(content):
        """Helper to parse CSV content into structured data"""
        rows = []
        for line in content.strip().split('\n'):
            line = line.strip()
            if line.startswith('#') or not line:
                continue
            parts = [p.strip() for p in line.split(',')]
            if len(parts) >= 5:
                rows.append({
                    'name': parts[0],
                    'type': parts[1],
                    'subtype': parts[2],
                    'offset': int(parts[3], 16),
                    'size': int(parts[4], 16)
                })
        return rows

    def test_valid_partition_table(self):
        """Test parsing of valid partition table"""
        
        valid_csv = """nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
app0,     app,  ota_0,   0x10000, 0x2F0000
littlefs, data, littlefs,0x300000,0x100000"""
        
        rows = self.parse_partition_csv(valid_csv)
        
        self.assertEqual(len(rows), 4, "Should have 4 partitions")
        self.assertEqual(rows[0]['name'], 'nvs')
        self.assertEqual(rows[-1]['name'], 'littlefs')

    def test_partition_overlap_detection(self):
        """Detect overlapping partition allocations"""
        
        overlapping_csv = """app0,     app,  ota_0,   0x10000, 0x200000
littlefs, data, lfs,     0x200000,0x100000
app1,     app,  ota_1,   0x200000,0x200000"""
        
        rows = self.parse_partition_csv(overlapping_csv)
        
        # Check for overlaps and assert that the fixture contains one.
        overlap_found = False
        for i, part1 in enumerate(rows):
            end1 = part1['offset'] + part1['size']
            for j, part2 in enumerate(rows):
                if i >= j:
                    continue
                start2 = part2['offset']
                
                if part1['offset'] <= start2 < end1:
                    overlap_found = True

        self.assertTrue(overlap_found, "The fixture should contain an overlapping partition")


def run_tests():
    """Run all unit tests"""
    
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    
    # Add all test classes
    suite.addTests(loader.loadTestsFromTestCase(TestFastBeeArtifacts))
    suite.addTests(loader.loadTestsFromTestCase(TestPartitionTableValidator))
    
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    
    return result.wasSuccessful()


if __name__ == '__main__':
    success = run_tests()
    exit(0 if success else 1)
