"""P1 storage and hardware-driver safety contracts."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SD_HEADER = (ROOT / "include/peripherals/SDCardManager.h").read_text(encoding="utf-8")
SD_SOURCE = (ROOT / "src/peripherals/SDCardManager.cpp").read_text(encoding="utf-8")
RFID_HEADER = (ROOT / "include/peripherals/drivers/RFIDDriver.h").read_text(encoding="utf-8")
RFID_SOURCE = (ROOT / "src/peripherals/drivers/RFIDDriver.cpp").read_text(encoding="utf-8")
CACHE_HEADER = (ROOT / "include/core/SensorDriverCache.h").read_text(encoding="utf-8")
CACHE_SOURCE = (ROOT / "src/core/SensorDriverCache.cpp").read_text(encoding="utf-8")


class P1StorageDriverContractTests(unittest.TestCase):
    def test_sd_uses_real_bounded_sdspi_mount(self):
        self.assertIn("SPI.begin(_status.sckPin, _status.misoPin", SD_SOURCE)
        self.assertIn("SD.begin(_status.csPin, SPI, _status.frequency", SD_SOURCE)
        self.assertIn("SD.cardType() == CARD_NONE", SD_SOURCE)
        self.assertIn("MAX_FILE_BYTES = 16384", SD_HEADER)
        self.assertIn("MAX_LIST_ENTRIES = 32", SD_HEADER)

    def test_every_sd_file_operation_normalizes_into_sandbox(self):
        self.assertIn('SD_SANDBOX_ROOT = "/fastbee"', SD_SOURCE)
        self.assertIn('normalized.indexOf("..")', SD_SOURCE)
        self.assertIn("SD_PATH_FORBIDDEN", SD_SOURCE)
        for signature in ("SDCardManager::list", "SDCardManager::readFile",
                          "SDCardManager::writeFile", "SDCardManager::removeFile"):
            start = SD_SOURCE.index(signature)
            block = SD_SOURCE[start:start + 900]
            self.assertIn("normalizeSandboxPath", block, signature)

    def test_sd_mount_and_mutations_are_serialized(self):
        self.assertIn("xSemaphoreCreateMutex", SD_SOURCE)
        self.assertGreaterEqual(SD_SOURCE.count("SDMutexGuard guard(_mutex)"), 7)
        self.assertIn("SD_BUSY", SD_SOURCE)

    def test_rfid_explicit_spi_and_safe_blocks_are_enforced(self):
        normalized_header = " ".join(RFID_HEADER.split())
        self.assertIn(
            "sckPin, uint8_t mosiPin, uint8_t misoPin",
            normalized_header,
        )
        self.assertIn("SPI.begin(sckPin, misoPin, mosiPin, ssPin)", RFID_SOURCE)
        self.assertIn("block > 0 && block <= 62 && (block % 4) != 3", RFID_SOURCE)
        self.assertIn("RFID_BLOCK_FORBIDDEN", RFID_SOURCE)

    def test_rfid_write_requires_authentication_and_readback_verification(self):
        start = RFID_SOURCE.index("RFIDDriver::writeBlockVerified")
        end = RFID_SOURCE.index("\nvoid RFIDDriver::stop()", start)
        block = RFID_SOURCE[start:end]
        self.assertIn("authenticateBlock", block)
        self.assertIn("MIFARE_Write", block)
        self.assertIn("MIFARE_Read", block)
        self.assertIn("memcmp(data, verify, 16)", block)
        self.assertNotIn("LOGGER", block)

    def test_sensor_cache_is_fixed_bounded_and_reuses_instances(self):
        self.assertIn("MAX_ENTRIES = 6", CACHE_HEADER)
        self.assertIn("Entry _entries[MAX_ENTRIES]", CACHE_HEADER)
        self.assertIn("findExact", CACHE_SOURCE)
        self.assertIn("allocateEntry", CACHE_SOURCE)
        self.assertEqual(CACHE_SOURCE.count("createDriver(driverName)"), 1)
        self.assertIn("entry->driver->read(reading)", CACHE_SOURCE)


if __name__ == "__main__":
    unittest.main()
