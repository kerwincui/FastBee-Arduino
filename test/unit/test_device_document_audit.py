import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
AUDITOR = ROOT / "scripts" / "audit-device-docs.mjs"


class DeviceDocumentAuditTests(unittest.TestCase):
    def run_audit(self, documents, reviews=None, extra=None):
        with tempfile.TemporaryDirectory(prefix="fastbee-doc-audit-") as directory:
            base = Path(directory)
            root = base / "src" / "device"
            root.mkdir(parents=True)
            for name, content in {**documents, **(extra or {})}.items():
                file = root / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(content if isinstance(content, bytes) else content.encode("utf-8"))
            report = base / "report.json"
            command = ["node", str(AUDITOR), "--docs-root", str(root), "--output", str(report)]
            if reviews is not None:
                review_dir = base / "reviews"
                review_dir.mkdir()
                (review_dir / "fixture-review.json").write_text(json.dumps({"files": reviews}), encoding="utf-8")
                command += ["--review-dir", str(review_dir)]
            process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
            self.assertTrue(report.exists(), process.stderr)
            return process.returncode, json.loads(report.read_text(encoding="utf-8"))

    def test_vuepress_html_links_images_and_encoded_paths_resolve(self):
        code, result = self.run_audit({"README.md": '[guide](guide.html#section)\n![image](images/a%20b.png)\n<img src="images/a b.png">', "guide.md": "# Guide"}, extra={"images/a b.png": b"image"})
        self.assertEqual(0, code, result["issues"])
        self.assertEqual(2, result["documents"])
        self.assertEqual(3, result["localLinks"])

    def test_broken_local_links_and_invalid_json_fail(self):
        code, result = self.run_audit({"README.md": '[missing](gone.html)\n```json\n{"a":1,}\n```'})
        self.assertEqual(1, code)
        self.assertEqual(2, len(result["issues"]))
        self.assertEqual(1, result["jsonBlocks"])

    def test_fences_ignore_links_and_nested_short_fences(self):
        code, result = self.run_audit({"README.md": '````text\n[example](not-a-link.md)\n```json\n{broken}\n```\n````\n~~~json\n{"中文":0}\n~~~'})
        self.assertEqual(0, code, result["issues"])
        self.assertEqual(1, result["jsonBlocks"])
        self.assertEqual(0, result["localLinks"])

    def test_invalid_utf8_and_unclosed_fences_fail(self):
        code, result = self.run_audit({"bad.md": b"\xff", "README.md": "```text\nunfinished"})
        self.assertEqual(1, code)
        self.assertEqual(2, len(result["issues"]))

    def test_every_document_requires_unique_review_and_real_source(self):
        record = {"path": "README.md", "sources": ["platformio.ini"], "findings": ["release environments checked"], "changed": False}
        code, result = self.run_audit({"README.md": "# Device", "guide.md": "# Guide"}, [record, record])
        self.assertEqual(1, code)
        self.assertTrue(any("duplicate review" in issue for issue in result["issues"]))
        self.assertTrue(any("not reviewed: guide.md" in issue for issue in result["issues"]))

    def test_review_hashes_inputs_and_rejects_missing_or_escaping_source(self):
        good = {"path": "README.md", "sources": ["platformio.ini"], "findings": "partitions verified", "changed": False}
        code, result = self.run_audit({"README.md": "# Device"}, [good])
        self.assertEqual(0, code, result["issues"])
        self.assertEqual(64, len(result["files"][0]["sourceSha256"]["platformio.ini"]))
        good["sources"] = ["../outside", "no-such-source.cpp"]
        code, result = self.run_audit({"README.md": "# Device"}, [good])
        self.assertEqual(1, code)
        self.assertEqual(2, len(result["issues"]))


if __name__ == "__main__":
    unittest.main()
