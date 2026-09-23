"""Execute Web idle recovery with deterministic cross-task clock races."""

from __future__ import annotations

import pathlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "network" / "WebConfigManager.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "include" / "network" / "WebConfigManager.h").read_text(encoding="utf-8")


class WebTcpWatchdogContractTests(unittest.TestCase):
    def test_production_request_age_and_watchdog_interleavings(self):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        self.assertTrue(pathlib.Path(compiler).exists(), "C++ compiler required")
        signature = "unsigned long WebConfigManager::getLastWebRequestAgeMs() const"
        marker = "if (lastRequestSeenMs > 0 && isRunning)"
        atomic_member = re.search(
            r"std::atomic<uint32_t> lastRequestSeenMs\{0\};", HEADER
        )
        self.assertIsNotNone(atomic_member, "Request timestamp crosses task boundaries")
        with tempfile.TemporaryDirectory(prefix="fastbee-web-idle-") as directory:
            temporary = pathlib.Path(directory)
            (temporary / "production_request_age.inc").write_text(
                signature + " {" + braced_block(SOURCE, signature) + "}\n",
                encoding="utf-8",
            )
            (temporary / "production_request_watchdog.inc").write_text(
                marker + " {" + braced_block(SOURCE, marker) + "}\n",
                encoding="utf-8",
            )
            (temporary / "production_request_diag.inc").write_text(
                braced_block(HEADER, "void getTcpDiag("), encoding="utf-8"
            )
            (temporary / "production_request_timestamp.inc").write_text(
                atomic_member.group(0), encoding="utf-8"
            )
            executable = temporary / "watchdog.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       f"-I{temporary}",
                       str(ROOT / "test/unit/cpp/web_tcp_watchdog_harness.cpp"),
                       "-o", str(executable)]
            build = subprocess.run(command, capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", timeout=60,
                                   env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True,
                                    text=True, timeout=10, env=environment)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_ota_upload_chunks_refresh_request_activity(self):
        route = (ROOT / "src/network/handlers/OTARouteHandler.cpp").read_text(encoding="utf-8")
        upload = braced_block(route, "void OTARouteHandler::handleOtaUpload(")
        self.assertLess(upload.index("ctx->requireAuth(request)"),
                        upload.index("ctx->otaManager->handleOTAUpload("))
        context = (ROOT / "src/network/WebHandlerContext.cpp").read_text(encoding="utf-8")
        authenticated = braced_block(context, "bool WebHandlerContext::requireAuth(")
        self.assertIn("noteWebRequestActivity();", authenticated)
        activity = braced_block(context, "void WebHandlerContext::noteWebRequestActivity(")
        self.assertIn("webConfigManager->trackWebRequest();", activity)
        track = braced_block(SOURCE, "void WebConfigManager::trackWebRequest()")
        self.assertIn("lastRequestSeenMs = now", track)

    def test_no_request_reaper_counts_only_port_80(self):
        start = SOURCE.index("NO_REQUEST_WATCHDOG_MS")
        end = SOURCE.index("// 8BIT", start)
        block = SOURCE[start:end]
        self.assertIn("countTcpConnectionsOnLocalPort(80, &timeWaitCount)", block)
        self.assertNotIn("countTcpConnections(&timeWaitCount)", block)

    def test_port_specific_counter_filters_both_pcb_lists(self):
        self.assertIn("countTcpConnectionsOnLocalPort", HEADER)
        start = SOURCE.index("uint16_t WebConfigManager::countTcpConnectionsOnLocalPort")
        end = SOURCE.index("uint8_t WebConfigManager::pruneTimeWaitPcbs", start)
        block = SOURCE[start:end]
        self.assertGreaterEqual(block.count("pcb->local_port != port"), 2)


if __name__ == "__main__":
    unittest.main()
