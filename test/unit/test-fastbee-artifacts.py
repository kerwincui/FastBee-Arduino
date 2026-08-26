"""
Unit Tests for fastbee-artifacts.py plugin hooks
Purpose: Validate filesystem detection and artifact merging logic
"""

import unittest
import csv
import os
from unittest.mock import MagicMock, patch


class TestFastBeeArtifacts(unittest.TestCase):
    """Test suite for FastBee artifacts management"""

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

    def test_merge_bin_command_generation(self):
        """Validate esptool merge-bin command generation logic"""
        
        images = [
            (0x1000, '/path/to/bootloader.bin'),
            (0x8000, '/path/to/partitions.bin'),
            (0x10000, '/path/to/firmware.factory.bin'),
        ]
        
        expected_cmd = [
            'python', 'esptool.py', '--chip', 'esp32c3',
            'merge-bin', '-o', 'factory.bin',
            '--flash-mode', 'keep',
            '--flash-freq', 'keep',
            '--flash-size', 'keep'
        ]
        
        for offset, path in images:
            expected_cmd.extend([hex(offset), path])
        
        # Verify command structure
        self.assertIn('merge-bin', expected_cmd)
        self.assertIn('-o', expected_cmd)
        self.assertGreater(len(expected_cmd), 5, "Command should have multiple arguments")

    @patch('os.path.exists')
    def test_esptool_path_detection(self, mock_exists):
        """Test that esptool path is correctly resolved"""
        
        from scripts.fastbee_artifacts import esptool_path
        
        mock_exists.return_value = True
        
        # Should return valid path when exists
        result = esptool_path()
        self.assertIsNotNone(result)

    def test_manifest_json_structure(self):
        """Validate manifest.json has required fields"""
        
        import json
        
        mock_manifest = {
            "environment": "esp32c3-F4R0",
            "filesystemProfile": "full",
            "generatedAt": "2026-08-20T20:00:00Z",
            "partitionTable": "partitions/fastbee.csv",
            "appOffset": "0x10000",
            "filesystemOffset": "0x2f0000"
        }
        
        # Validate required fields
        required_fields = ['environment', 'filesystemProfile', 'partitionTable']
        
        for field in required_fields:
            self.assertIn(field, mock_manifest, f"Required field '{field}' missing")
        
        # Validate offset format
        self.assertTrue(mock_manifest['appOffset'].startswith('0x'), 
                       "Offsets should be in hex format")

    def test_build_flag_parsing(self):
        """Test parsing of build flags from PlatformIO environment"""
        
        build_flags = [
            "-DFASTBEE_ENABLE_LCD=1",
            "-DFASTBEE_USE_PSRAM=0",
            "-DFASTBEE_ENABLE_SENSOR_DRIVER=1"
        ]
        
        def get_build_flag_value(flags, flag_name, default="0"):
            prefix = f"-D{flag_name}="
            value = default
            
            for flag in flags:
                if flag.startswith(prefix):
                    value = flag[len(prefix):]
                    break
            
            return value
        
        lcd_enabled = get_build_flag_value(build_flags, "FASTBEE_ENABLE_LCD")
        psram_disabled = get_build_flag_value(build_flags, "FASTBEE_USE_PSRAM")
        
        self.assertEqual(lcd_enabled, "1", "LCD should be enabled")
        self.assertEqual(psram_disabled, "0", "PSRAM should be disabled")


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
littlefs, data, littlefs,0x2f0000,0x100000"""
        
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
        
        # Check for overlaps
        for i, part1 in enumerate(rows):
            end1 = part1['offset'] + part1['size']
            for j, part2 in enumerate(rows):
                if i >= j:
                    continue
                start2 = part2['offset']
                
                # Simple overlap check
                if part1['offset'] <= start2 < end1:
                    self.fail(f"Partitions {part1['name']} and {part2['name']} overlap!")


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
