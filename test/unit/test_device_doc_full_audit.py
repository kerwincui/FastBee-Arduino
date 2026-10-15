"""设备端文档全量审计门禁。

当相邻 FastBee-doc checkout 存在时，重新运行文档审计并要求所有 Markdown
都有唯一 review 记录、源码引用有效、JSON fenced block 和本地链接正确。
缺少外部文档 checkout 的环境跳过该集成门禁；示例契约测试仍可独立运行。
"""
from __future__ import annotations

import json
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DOC_ROOT = Path(r"D:/project/gitlab/FastBee-doc/src/device")
REVIEW_DIR = ROOT / "artifacts/device-doc-audit-20261011"
AUDITOR = ROOT / "scripts/audit-device-docs.mjs"


class DeviceDocFullAuditTests(unittest.TestCase):
    def test_all_device_documents_have_current_review_and_sources(self):
        if not DOC_ROOT.is_dir():
            self.skipTest("FastBee-doc checkout is not available")

        report = ROOT / "artifacts/device-doc-audit-20261011/.test-device-doc-report.json"
        try:
            result = subprocess.run(
                [
                    "node", str(AUDITOR), "--docs-root", str(DOC_ROOT),
                    "--review-dir", str(REVIEW_DIR), "--output", str(report),
                ], cwd=ROOT, check=False, capture_output=True, text=True,
                encoding="utf-8",
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            payload = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(payload["documents"], 187)
            self.assertEqual(payload["reviewedDocuments"], payload["documents"])
            self.assertIs(payload["passed"], True)
            self.assertEqual(payload["issues"], [])
        finally:
            if report.exists():
                report.unlink()
