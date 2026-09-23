"""Execute the production weather worker admission and retry policy."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/protocols/RemoteResourceManager.cpp").read_text(encoding="utf-8")


def production_policy() -> str:
    start = SOURCE.index("bool due(uint32_t now,uint32_t deadline)")
    end = SOURCE.index("bool weatherNetworkConnected()", start)
    return SOURCE[start:end]


class RemoteWeatherSchedulerTests(unittest.TestCase):
    def test_tick_and_collection_use_production_policy(self):
        tick_start = SOURCE.index("void RemoteResourceManager::tick()")
        tick_end = SOURCE.index("void RemoteResourceManager::weatherTask(", tick_start)
        tick = SOURCE[tick_start:tick_end]
        collect_start = SOURCE.index("void RemoteResourceManager::collectWeather()")
        collect = SOURCE[collect_start:]

        self.assertIn("weatherNetworkConnected()", tick)
        self.assertIn("weatherWorkerEligible(", tick)
        self.assertIn("WEATHER_WORKER_MIN_INTERNAL_BYTES", production_policy())
        self.assertIn('error=="Network offline"', collect)
        self.assertIn('error=="Weather network request failed"', collect)
        self.assertIn("weatherFailureDelayMs(", collect)


    def test_deployed_ntp_blocker_has_recoverable_retry_and_user_friendly_status(self):
        evidence = ROOT / 'artifacts/touch-optimization-20260922/postdeploy-api.json'
        data = json.loads(evidence.read_text(encoding='utf-8'))
        weather = next(entry['data'] for entry in data['checks'] if entry['route'] == '/api/screen-panel/weather')
        status = weather['status']
        self.assertEqual('error', status['status'])
        # Field baseline: no live device is available here, so the deployed API
        # capture is the reproducible contract. NTP/TLS admission failures should
        # be transient and user-actionable instead of a raw internal blocker.
        self.assertGreater(status['retryAfterMs'], 0)
        self.assertNotIn('NTP before TLS', status['error'])

    def test_production_policy_offline_recovery_and_failure_limits(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("A C++17 compiler is required")

        harness = f"""
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

{production_policy()}

int main() {{
    // A due source remains idle while offline, then is admitted immediately
    // once networking returns without consuming its configured interval.
    assert(due(1000U, 0U));
    assert(!weatherWorkerEligible(false, false, true, false, due(1000U, 0U), 24000U));
    assert(weatherWorkerEligible(true, false, true, false, due(1000U, 0U), 24000U));

    // Existing OTA, worker, due-state and internal-memory gates remain active.
    assert(!weatherWorkerEligible(true, true, true, false, true, 24000U));
    assert(!weatherWorkerEligible(true, false, false, false, true, 24000U));
    assert(!weatherWorkerEligible(true, false, true, true, true, 24000U));
    assert(!weatherWorkerEligible(true, false, true, false, false, 24000U));
    assert(!weatherWorkerEligible(true, false, true, false, true, 23999U));

    // A disconnect race retries with bounded exponential spacing instead of
    // consuming the normal 30-minute successful collection period.
    assert(weatherFailureDelayMs(1, true, 1800000U) == 30000U);
    assert(weatherFailureDelayMs(2, true, 1800000U) == 60000U);
    assert(weatherFailureDelayMs(3, true, 1800000U) == 120000U);
    assert(weatherFailureDelayMs(4, true, 1800000U) == 240000U);
    assert(weatherFailureDelayMs(5, true, 1800000U) == 300000U);
    assert(weatherFailureDelayMs(6, true, 1800000U) == 300000U);
    const uint32_t retryAt = 1000U + weatherFailureDelayMs(1, true, 1800000U);
    assert(!due(30999U, retryAt));
    assert(due(31000U, retryAt));

    // Non-transport failures retain the configured interval and original
    // backoff floor, so provider failures cannot cause request bursts.
    assert(weatherFailureDelayMs(1, false, 1800000U) == 1800000U);
    assert(weatherFailureDelayMs(1, false, 60000U) == 120000U);
    assert(weatherFailureDelayMs(6, false, 60000U) == 3840000U);
}}
"""
        with tempfile.TemporaryDirectory(prefix="fastbee-weather-scheduler-") as directory:
            directory_path = Path(directory)
            cpp = directory_path / "scheduler.cpp"
            executable = directory_path / ("scheduler.exe" if os.name == "nt" else "scheduler")
            cpp.write_text(harness, encoding="utf-8")
            result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(cpp), "-o", str(executable)],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=60,
            )
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            result = subprocess.run(
                [str(executable)], capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=15,
            )
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
