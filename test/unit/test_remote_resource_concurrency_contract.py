"""Concurrency contracts for remote command and observation commits."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = (ROOT / "include/protocols/RemoteResourceManager.h").read_text(
    encoding="utf-8"
)
SOURCE = (ROOT / "src/protocols/RemoteResourceManager.cpp").read_text(
    encoding="utf-8"
)


def braced_block(source: str, marker: str) -> str:
    marker_at = source.find(marker)
    if marker_at < 0:
        raise AssertionError(f"missing source marker {marker!r}")
    opening = source.find("{", marker_at + len(marker))
    if opening < 0:
        raise AssertionError(f"missing block after {marker!r}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated block after {marker!r}")


class RemoteResourceConcurrencyContractTests(unittest.TestCase):
    def test_retired_channels_cannot_publish_and_weather_save_is_fenced(self):
        self.assertIn("size_t activeExternalCommits_ = 0", HEADER)
        mqtt = braced_block(SOURCE, "void RemoteResourceManager::handleMqtt(")
        self.assertNotIn("publishExact", mqtt)
        self.assertNotIn("client->subscribeExact", mqtt)
        consume = braced_block(SOURCE, "bool RemoteResourceManager::consumeMqtt(")
        self.assertIn("return false", consume)
        self.assertNotIn("upsertObservation", consume)
        save = braced_block(SOURCE, "bool RemoteResourceManager::saveConfig(")
        fence_at = save.index("if (activeExternalCommits_)")
        persist_at = save.index("saveJSONConfig(", fence_at)
        apply_at = save.index("applyConfig(", persist_at)
        self.assertLess(fence_at, persist_at)
        self.assertLess(persist_at, apply_at)

        transaction = braced_block(
            SOURCE,
            "bool RemoteResourceManager::beginProtocolConfigTransaction(",
        )
        self.assertIn("if (activeExternalCommits_)", transaction)
        self.assertIn("xSemaphoreGiveRecursive(mutex_)", transaction)

    def test_observations_use_the_same_generation_and_mapping_fence(self):
        claim = braced_block(
            SOURCE, "bool RemoteResourceManager::beginObservationCommit("
        )
        for token in (
            "generation != generation_",
            'if (!weatherSource) return false',
            'source["enabled"]',
            'source["id"]',
            "RemoteResourceModel::sourceRevision(source)",
            "++activeExternalCommits_",
        ):
            self.assertIn(token, claim)

        weather = braced_block(SOURCE, "void RemoteResourceManager::collectWeather(")
        state_commit = weather.index("s.updatedMs=now")
        weather_claim = weather.index("beginObservationCommit(", state_commit)
        weather_upsert = weather.index("upsertObservation(", weather_claim)
        weather_release = weather.index("endExternalCommit()", weather_upsert)
        self.assertLess(state_commit, weather_claim)
        self.assertLess(weather_claim, weather_upsert)
        self.assertLess(weather_upsert, weather_release)

        self.assertNotIn("upsertObservation", claim)


if __name__ == "__main__":
    unittest.main()
