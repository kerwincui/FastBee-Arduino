"""Compile the panel status text mapping header and verify every mapping."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "include/screen/PanelStatusText.h"


class ScreenPanelStatusTextTests(unittest.TestCase):
    def test_async_exec_error_codes_map_to_chinese(self):
        run = self._compile_and_run()
        self.assertIn("async-exec-codes ok", run.stdout)

    def test_network_status_values_map_to_chinese(self):
        run = self._compile_and_run()
        self.assertIn("network-status-values ok", run.stdout)

    def test_unknown_machine_codes_fall_back_to_readable_text(self):
        run = self._compile_and_run()
        self.assertIn("unknown-fallbacks ok", run.stdout)

    def test_human_readable_text_passthrough(self):
        run = self._compile_and_run()
        self.assertIn("passthrough ok", run.stdout)

    def test_renderer_and_default_page_use_the_mapping(self):
        renderer = (ROOT / "src/screen/StripeRenderer.h").read_text(encoding="utf-8")
        self.assertIn('#include "screen/PanelStatusText.h"', renderer)
        for marker in ("panelErrorText(card.error)",
                       'strcmp(card.sourceRef, "system.network")',
                       "panelNetworkStatusText(card.value)"):
            self.assertIn(marker, renderer)
        manager = (ROOT / "src/core/ScreenPanelManager.cpp").read_text(encoding="utf-8")
        self.assertIn('page["title"] = "首页";', manager)
        header = HEADER.read_text(encoding="utf-8")
        self.assertIn("namespace FastBee { namespace Screen {", header)
        self.assertIn('inline const char* panelErrorText(const char* error)', header)
        self.assertIn('inline const char* panelNetworkStatusText(const char* status)', header)

    def _compile_and_run(self):
        compiler = shutil.which("g++")
        fallback = Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        self.assertTrue(compiler, "Real C++17 compiler required")
        with tempfile.TemporaryDirectory(prefix="fastbee-panel-status-text-") as directory:
            temp = Path(directory)
            source = temp / "panel-status-text.cpp"
            source.write_text(HARNESS, encoding="utf-8")
            exe = temp / ("panel-status-text.exe" if os.name == "nt" else "panel-status-text")
            build = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Os",
                 "-I" + str(ROOT / "include"), str(source), "-o", str(exe)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            return run


HARNESS = r'''
#include <cstdio>
#include <cstring>
#include "screen/PanelStatusText.h"

using FastBee::Screen::panelErrorText;
using FastBee::Screen::panelNetworkStatusText;

int failures = 0;
void expect(const char* actual, const char* wanted, const char* label) {
    if (std::strcmp(actual, wanted) != 0) {
        std::fprintf(stderr, "%s: expected [%s] got [%s]\n", label, wanted, actual);
        ++failures;
    }
}

int main() {
    // Async execution error codes reaching card.error become short phrases.
    expect(panelErrorText("low_memory"), "内存不足", "low_memory");
    expect(panelErrorText("deadline_exceeded"), "超时", "deadline_exceeded");
    expect(panelErrorText("action_failed"), "动作失败", "action_failed");
    expect(panelErrorText("incomplete_actions"), "部分动作未完成", "incomplete_actions");
    expect(panelErrorText("cancelled_by_system"), "被系统取消", "cancelled_by_system");
    expect(panelErrorText("worker_queue_full"), "队列已满", "worker_queue_full");
    expect(panelErrorText("uart_source_unconfirmed"), "UART 未确认", "uart_source_unconfirmed");
    expect(panelErrorText("rule_revision_unconfirmed"), "规则版本未确认", "rule_revision_unconfirmed");
    expect(panelErrorText("binding_unconfigured"), "未绑定数据源", "binding_unconfigured");
    std::printf("async-exec-codes ok\n");

    // system.network status values become short phrases.
    expect(panelNetworkStatusText("connected"), "已连接", "connected");
    expect(panelNetworkStatusText("disconnected"), "未连接", "disconnected");
    expect(panelNetworkStatusText("connecting"), "连接中", "connecting");
    expect(panelNetworkStatusText("ap_mode"), "热点模式", "ap_mode");
    expect(panelNetworkStatusText("failed"), "连接失败", "failed");
    expect(panelNetworkStatusText("unknown"), "未知状态", "unknown");
    std::printf("network-status-values ok\n");

    // Unknown machine codes get a readable fallback instead of raw English.
    expect(panelErrorText("future_unknown_code"), "未知错误", "unknown code");
    expect(panelErrorText("SCREEN_TOUCH_INIT_FAILED"), "未知错误", "uppercase code");
    expect(panelErrorText("TRAVEL_RANGE_EXCEEDED"), "未知错误",
           "motor code mapped before the safety branch uses it");
    expect(panelErrorText("none"), "", "none means no error");
    expect(panelErrorText(""), "", "empty");
    expect(panelErrorText(nullptr), "", "null");
    expect(panelNetworkStatusText("associating"), "未知状态", "unknown network status");
    expect(panelNetworkStatusText(""), "", "empty network status");
    expect(panelNetworkStatusText(nullptr), "", "null network status");
    std::printf("unknown-fallbacks ok\n");

    // Already readable text must survive untouched.
    expect(panelErrorText("电机通信失败，请检查控制器电源与接线"),
           "电机通信失败，请检查控制器电源与接线", "chinese passthrough");
    expect(panelErrorText("已到软件限位"), "已到软件限位", "short chinese passthrough");
    expect(panelNetworkStatusText("已连接"), "已连接", "chinese network passthrough");
    std::printf("passthrough ok\n");

    if (failures) return 2;
    std::printf("panel status text mapping ok\n");
    return 0;
}
'''


if __name__ == "__main__":
    unittest.main()
