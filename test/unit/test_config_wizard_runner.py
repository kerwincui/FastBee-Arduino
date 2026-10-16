"""Run the actual serial runner against process-boundary failure fixtures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ConfigWizardRunnerTests(unittest.TestCase):
    def test_failed_and_missing_report_do_not_hide_independent_suites(self):
        scripts = ["test-periph-exec-template-browser.cjs", "test-config-migration-preview-browser.cjs",
                   "test-rtc-quality-browser.cjs", "test-sd-quality-browser.cjs", "test-ads1115-form.cjs",
                   "test-motor-protocol-editor.cjs"]
        with tempfile.TemporaryDirectory(prefix="fastbee-wizard-runner-") as temporary:
            fixture = Path(temporary)
            (fixture / "scripts").mkdir()
            shutil.copy2(ROOT / "scripts/run-config-wizard-tests.js", fixture / "scripts")
            for index, name in enumerate(scripts):
                source = "const fs=require('fs'),path=require('path'),out=process.argv[2];\n"
                source += "fs.mkdirSync(out);\n"
                # A nonzero child and a zero-exit child with missing evidence
                # must both fail without skipping the remaining four suites.
                if index != 1:
                    source += "fs.writeFileSync(path.join(out,'report.json'),JSON.stringify({status:'PASS',cases:[{status:'PASS'}]}));\n"
                source += "process.exitCode=" + ("2" if index == 0 else "0") + ";\n"
                (fixture / "scripts" / name).write_text(source, encoding="utf-8")
            for setting in (None, "relative evidence", str(fixture / "absolute evidence")):
                with self.subTest(artifact_root=setting):
                    environment = os.environ.copy()
                    environment.pop("FASTBEE_TEST_ARTIFACT_ROOT", None)
                    if setting is not None:
                        environment["FASTBEE_TEST_ARTIFACT_ROOT"] = setting
                    expected_root = (fixture / (setting or "artifacts")).resolve()
                    preserved = {}
                    for _ in range(2):
                        result = subprocess.run(["node", "scripts/run-config-wizard-tests.js"], cwd=fixture,
                                                env=environment, capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                        for path, original in preserved.items():
                            self.assertEqual(path.read_bytes(), original)
                        report_path, = [path for path in expected_root.glob("config-wizards-*/report.json")
                                        if path not in preserved]
                        report = json.loads(report_path.read_text(encoding="utf-8"))
                        self.assertEqual(report["status"], "FAIL")
                        self.assertFalse(report["devicesAccessed"])
                        self.assertEqual([row["status"] for row in report["stages"]], ["FAIL", "FAIL", "PASS", "PASS", "PASS", "PASS"])
                        self.assertEqual([row["exitCode"] for row in report["stages"]], [2, 0, 0, 0, 0, 0])
                        self.assertTrue(all(Path(row["log"]).is_file() and row["logSha256"] for row in report["stages"]))
                        self.assertTrue(all(Path(row["evidence"]).is_dir() for row in report["stages"]))
                        self.assertTrue(all(Path(row["evidence"]).parent == expected_root and
                                            Path(row["log"]).parent == report_path.parent for row in report["stages"]))
                        for directory in [report_path.parent, *(Path(row["evidence"]) for row in report["stages"])]:
                            preserved.update((path, path.read_bytes()) for path in directory.iterdir() if path.is_file())


if __name__ == "__main__":
    unittest.main()
