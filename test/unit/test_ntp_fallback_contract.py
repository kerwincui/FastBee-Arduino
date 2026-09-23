"""Contract checks for the field-safe NTP fallback path."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class NtpFallbackContractTests(unittest.TestCase):
    def test_psram_http_fallback_is_bounded_and_marks_sync(self):
        source = (ROOT / "src/core/FastBeeFramework.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/core/FastBeeFramework.h").read_text(encoding="utf-8")
        self.assertIn("http://iot.fastbee.cn/prod-api/iot/tool/ntp", source)
        self.assertIn("ESP.getPsramSize() > 0", source)
        self.assertIn("TimeUtils::syncNTPFromHTTP", source)
        self.assertIn("HTTP_NTP_FALLBACK_URL, 3000", source)
        self.assertIn("ntpHttpFallbackAttempts < 2", source)
        self.assertIn("ntpHttpFallbackAttempts", header)
        self.assertIn("framework->ntpSynced = true", source)
        self.assertIn("framework->ntpSyncPending = false", source)

    def test_fallback_does_not_replace_async_sntp_for_no_psram(self):
        source = (ROOT / "src/core/FastBeeFramework.cpp").read_text(encoding="utf-8")
        guard = "framework->ntpHttpFallbackAttempts < 2"
        start = source.index(guard)
        block = source[start:source.index("LOG_WARNINGF(\"[NTP] Sync failed", start)]
        self.assertIn("ESP.getPsramSize() > 0", block)
        self.assertIn("ntpHttpFallbackAttempts", block)


if __name__ == "__main__":
    unittest.main()
