"""Executable behavior tests for ScreenRuntime system actions.

The harness runs the production dispatch, buzzer auto-off, and shutdown-cleanup
statements extracted from ScreenRuntime.cpp. This avoids maintaining a second
test-only implementation of the timing and fail-safe behavior.
"""

from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNTIME_SOURCE = ROOT / "src/screen/ScreenRuntime.cpp"
HARNESS = ROOT / "test/unit/cpp/screen_system_actions_harness.cpp"


def braced_statement(source: str, marker: str, start: int = 0) -> str:
    marker_at = source.find(marker, start)
    if marker_at < 0:
        raise AssertionError(f"missing source marker {marker!r}")
    opening = source.find("{", marker_at + len(marker))
    if opening < 0:
        raise AssertionError(f"missing block after {marker!r}")
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[marker_at : index + 1]
    raise AssertionError(f"unterminated block after {marker!r}")


def action_dispatch_chain(source: str) -> str:
    start_marker = "if (pendingAction == ScreenSystemAction::SET_BRIGHTNESS &&"
    start = source.find(start_marker)
    if start < 0:
        raise AssertionError("missing system-action dispatch chain")
    buzzer_marker = "else if (pendingAction == ScreenSystemAction::BUZZER &&"
    buzzer = source.find(buzzer_marker, start)
    if buzzer < 0:
        raise AssertionError("missing buzzer dispatch branch")
    opening = source.find("{", buzzer + len(buzzer_marker))
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[start : index + 1]
    raise AssertionError("unterminated system-action dispatch chain")


def constant_declaration(source: str, name: str) -> str:
    match = re.search(
        rf"constexpr\s+uint32_t\s+{re.escape(name)}\s*=\s*[^;]+;", source
    )
    if not match:
        raise AssertionError(f"missing constant {name}")
    return match.group(0)


def production_include(source: str) -> str:
    auto_off = braced_statement(source, "if (buzzerNeedsOff &&")
    cleanup = braced_statement(
        source, "for (uint8_t attempt = 0; attempt < 3; ++attempt)"
    )
    dispatch = action_dispatch_chain(source)
    return f"""
{constant_declaration(source, 'BUZZER_PULSE_MS')}
{constant_declaration(source, 'BUZZER_OFF_RETRY_MS')}

void runBuzzerAutoOff(HarnessState& state, uint32_t now) {{
    auto& device = state.device;
    bool& buzzerNeedsOff = state.buzzerNeedsOff;
    uint32_t& buzzerOffAtMs = state.buzzerOffAtMs;
    uint32_t& buzzerOffFailureLoggedAtMs = state.buzzerOffFailureLoggedAtMs;
{auto_off}
}}

bool runSystemAction(HarnessState& state, ScreenSystemAction pendingAction,
                     uint8_t pendingValue, uint32_t now) {{
    auto& device = state.device;
    const DisplayCapabilities caps = device.display()->capabilities();
    bool applied = false;
    bool& sleeping = state.sleeping;
    bool& forceRender = state.forceRender;
    uint8_t& appliedBrightness = state.appliedBrightness;
    bool& brightnessOverridden = state.brightnessOverridden;
    uint32_t& brightnessOverrideRevision = state.brightnessOverrideRevision;
    uint32_t& lastInteractionMs = state.lastInteractionMs;
    bool& buzzerNeedsOff = state.buzzerNeedsOff;
    uint32_t& buzzerOffAtMs = state.buzzerOffAtMs;
    uint32_t& buzzerOffFailureLoggedAtMs = state.buzzerOffFailureLoggedAtMs;
    TouchGesture& gesture = state.gesture;
    auto& controlSession = state.controlSession;
    auto& confirmationCardId = state.confirmationCardId;
    Snapshot* snapshot = &state.snapshot;
    auto& renderer = state.renderer;
    const size_t snapshotBytes = state.snapshotBytes;
    const size_t framebufferBytes = state.framebufferBytes;
    auto updateRuntimeState = [&state](bool running, bool sleepingNow,
                                       size_t allocated) {{
        state.runtimeUpdateCount++;
        state.runtimeRunning = running;
        state.runtimeSleeping = sleepingNow;
        state.runtimeAllocated = allocated;
    }};
{dispatch}
    state.lastApplied = applied;
    return applied;
}}

void runStopCleanup(HarnessState& state) {{
    auto& device = state.device;
{cleanup}
}}
"""


class ScreenSystemActionBehaviorTests(unittest.TestCase):
    def test_production_fragments_remain_extractable(self):
        source = RUNTIME_SOURCE.read_text(encoding="utf-8")
        generated = production_include(source)
        self.assertIn("now + BUZZER_PULSE_MS", generated)
        self.assertIn("now + BUZZER_OFF_RETRY_MS", generated)
        self.assertIn("static_cast<int32_t>(now - buzzerOffAtMs) >= 0", generated)
        self.assertIn("buzzerNeedsOff = true", generated)
        self.assertIn("device.display()->setBuzzer(false);", generated)
        self.assertIn("applied = device.display()->setBuzzer(true);", generated)
        self.assertIn("pendingAction == ScreenSystemAction::SLEEP", generated)
        self.assertIn("attempt < 3", generated)

    def test_extracted_production_behavior(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = pathlib.Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("A C++17 compiler is required")

        source = RUNTIME_SOURCE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="fastbee-screen-actions-") as temporary:
            temporary_path = pathlib.Path(temporary)
            generated = temporary_path / "screen_system_actions_production.inc"
            executable = temporary_path / (
                "screen-system-actions.exe" if os.name == "nt" else "screen-system-actions"
            )
            generated.write_text(production_include(source), encoding="utf-8")
            environment = dict(os.environ)
            environment["PATH"] = (
                str(pathlib.Path(compiler).parent)
                + os.pathsep
                + environment.get("PATH", "")
            )
            compiled = subprocess.run(
                [
                    compiler,
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{temporary_path}",
                    f"-I{ROOT / 'include'}",
                    str(HARNESS),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
                env=environment,
            )
            self.assertEqual(0, compiled.returncode, compiled.stdout + compiled.stderr)
            executed = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=15,
                env=environment,
            )
            self.assertEqual(0, executed.returncode, executed.stdout + executed.stderr)
            self.assertIn("screen-system-actions: ok", executed.stdout)


if __name__ == "__main__":
    unittest.main()
