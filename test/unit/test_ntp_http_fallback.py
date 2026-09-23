"""Contracts for the bounded PSRAM-only NTP fallback policy."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'src/core/FastBeeFramework.cpp').read_text(encoding='utf-8')


class NtpHttpFallbackTests(unittest.TestCase):
    def test_policy_is_psram_only_after_three_sntp_failures(self):
        self.assertIn('http://iot.fastbee.cn/prod-api/iot/tool/ntp', SOURCE)
        task = SOURCE[SOURCE.index('// NTP同步任务'):SOURCE.index('// 内存监控任务')]
        self.assertIn('framework->ntpRetryCount >= 3', task)
        self.assertIn('framework->ntpHttpFallbackAttempts < 2', task)
        self.assertIn('ESP.getPsramSize() > 0', task)
        self.assertIn('TimeUtils::syncNTPFromHTTP(\n                                HTTP_NTP_FALLBACK_URL, 3000)', task)
        self.assertIn('framework->ntpSynced = true', task)
        self.assertIn('EVENT_NTP_SYNCED', task)

    def test_sntp_remains_first_path_and_timezone_is_preserved(self):
        standard = SOURCE.index('// Standard SNTP remains')
        self.assertLess(SOURCE.index('syncTimeFromConfig()'), standard)
        self.assertIn('configTzTime(tz, primaryServer.c_str(), secondaryServer.c_str(), nullptr);', SOURCE)
        self.assertIn('setenv("TZ", tz, 1);', SOURCE)
        self.assertIn('LOG_INFO("[NTP] HTTP fallback sync successful")', SOURCE)

    def test_failed_fallback_reuses_existing_retry_and_cooldown(self):
        task = SOURCE[SOURCE.index('// NTP同步任务'):SOURCE.index('// 内存监控任务')]
        self.assertIn('framework->ntpRetryCount++;', task)
        self.assertIn('framework->ntpRetryCount < 10', task)
        self.assertIn('30min cooldown', task)
        self.assertNotIn('delay(3000', SOURCE)


if __name__ == '__main__':
    unittest.main()
