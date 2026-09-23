import json
import pathlib
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = ROOT / "scripts" / "config-staging-journal.ps1"
DEPLOY = ROOT / "scripts" / "deploy-config-bundle.ps1"
POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")

HARNESS = r"""
param(
    [Parameter(Mandatory = $true)][string]$Helper,
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][string]$Mode,
    [string]$SignalPath = ""
)
$ErrorActionPreference = "Stop"
. $Helper
$configDir = Join-Path $ProjectRoot "data\config"

function Set-StagedState {
    foreach ($file in @(Get-ChildItem -LiteralPath $configDir -File -Force)) {
        Remove-Item -LiteralPath $file.FullName -Force
    }
    [System.IO.File]::WriteAllBytes(
        (Join-Path $configDir "staged.json"),
        [System.Text.Encoding]::UTF8.GetBytes('{"staged":true}')
    )
}

switch ($Mode) {
    "normal" {
        $journal = New-FastBeeConfigStagingJournal `
            -ProjectRoot $ProjectRoot -ConfigDir $configDir -FirmwareLockToken "normal-lock"
        Set-StagedState
        Restore-FastBeeConfigStagingJournal `
            -ProjectRoot $ProjectRoot -ConfigDir $configDir `
            -JournalDirectory $journal.JournalDirectory `
            -ExpectedStageToken $journal.StageToken -RemoveOnSuccess | Out-Null
    }
    "abandon" {
        $journal = New-FastBeeConfigStagingJournal `
            -ProjectRoot $ProjectRoot -ConfigDir $configDir -FirmwareLockToken "abandoned-lock"
        Set-StagedState
        if (-not [string]::IsNullOrWhiteSpace($SignalPath)) {
            [System.IO.File]::WriteAllText($SignalPath, $journal.StageToken)
        }
        [System.Environment]::Exit(0)
    }
    "hold" {
        $journal = New-FastBeeConfigStagingJournal `
            -ProjectRoot $ProjectRoot -ConfigDir $configDir -FirmwareLockToken "active-lock"
        Set-StagedState
        [System.IO.File]::WriteAllText($SignalPath, $journal.StageToken)
        Start-Sleep -Seconds 60
    }
    "recover" {
        $count = Recover-FastBeeAbandonedConfigStaging `
            -ProjectRoot $ProjectRoot -ConfigDir $configDir `
            -CurrentFirmwareLockToken "recovery-lock"
        Write-Output "RECOVERED=$count"
    }
    default { throw "Unknown harness mode: $Mode" }
}
"""


@unittest.skipUnless(POWERSHELL, "PowerShell is required for staging journal tests")
class ConfigStagingJournalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fastbee-config-journal-")
        self.project = pathlib.Path(self.temp.name) / "project"
        self.config = self.project / "data" / "config"
        self.config.mkdir(parents=True)
        self.harness = pathlib.Path(self.temp.name) / "journal-harness.ps1"
        self.harness.write_text(HARNESS, encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def command(self, mode, signal=None):
        args = [
            POWERSHELL,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(self.harness),
            "-Helper",
            str(HELPER),
            "-ProjectRoot",
            str(self.project),
            "-Mode",
            mode,
        ]
        if signal is not None:
            args.extend(["-SignalPath", str(signal)])
        return args

    def run_mode(self, mode, signal=None, expected=0, timeout=20):
        result = subprocess.run(
            self.command(mode, signal),
            cwd=self.project,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
        self.assertEqual(
            result.returncode,
            expected,
            f"mode={mode} stdout={result.stdout!r} stderr={result.stderr!r}",
        )
        return result

    def journal_directories(self):
        root = self.project / ".pio" / "config-backups"
        return [] if not root.exists() else [item for item in root.iterdir() if item.is_dir()]

    def assert_config(self, expected):
        actual = {path.name: path.read_bytes() for path in self.config.iterdir() if path.is_file()}
        self.assertEqual(actual, expected)

    def test_normal_restore_is_byte_exact_and_cleans_new_files(self):
        original = {
            "alpha.json": b' {"alpha":1}\r\n',
            "binary.bin": bytes([0, 255, 13, 10, 42, 128]),
        }
        for name, content in original.items():
            (self.config / name).write_bytes(content)

        self.run_mode("normal")

        self.assert_config(original)
        self.assertEqual(self.journal_directories(), [])

    def test_abrupt_exit_is_recovered_by_next_owner(self):
        original = {"original.json": b'{"before":"crash"}\n'}
        (self.config / "original.json").write_bytes(original["original.json"])
        signal = pathlib.Path(self.temp.name) / "abandoned.signal"

        self.run_mode("abandon", signal=signal)
        self.assertTrue(signal.exists())
        self.assert_config({"staged.json": b'{"staged":true}'})
        self.assertEqual(len(self.journal_directories()), 1)

        result = self.run_mode("recover")

        self.assertIn("RECOVERED=1", result.stdout)
        self.assert_config(original)
        self.assertEqual(self.journal_directories(), [])

    def test_live_owner_is_refused_then_dead_owner_can_recover(self):
        original = {"active.json": b"active-owner-original"}
        (self.config / "active.json").write_bytes(original["active.json"])
        signal = pathlib.Path(self.temp.name) / "active.signal"
        holder = subprocess.Popen(
            self.command("hold", signal),
            cwd=self.project,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        try:
            deadline = time.monotonic() + 10
            while not signal.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertTrue(signal.exists(), "active staging owner did not publish its journal")

            contender = subprocess.run(
                self.command("recover"),
                cwd=self.project,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=10,
            )
            self.assertNotEqual(contender.returncode, 0)
            self.assertIn("owned by active pid", contender.stdout + contender.stderr)
            self.assert_config({"staged.json": b'{"staged":true}'})
        finally:
            holder.kill()
            holder.communicate(timeout=10)

        self.run_mode("recover")
        self.assert_config(original)

    def test_empty_original_directory_removes_staged_files_on_recovery(self):
        self.run_mode("abandon")
        self.assert_config({"staged.json": b'{"staged":true}'})

        self.run_mode("recover")

        self.assert_config({})
        self.assertEqual(self.journal_directories(), [])

    def test_project_and_backup_path_tampering_are_rejected(self):
        (self.config / "guard.json").write_bytes(b"guard-original")
        self.run_mode("abandon")
        journal = self.journal_directories()[0]
        manifest_path = journal / "journal.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["projectRoot"] = str(pathlib.Path(self.temp.name) / "other-project")
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

        project_mismatch = subprocess.run(
            self.command("recover"),
            cwd=self.project,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
        self.assertNotEqual(project_mismatch.returncode, 0)
        self.assertIn("different project root", project_mismatch.stdout + project_mismatch.stderr)
        self.assert_config({"staged.json": b'{"staged":true}'})

        manifest["projectRoot"] = str(self.project.resolve())
        manifest["firmwareLockToken"] = "mixed-token"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        token_mismatch = subprocess.run(
            self.command("recover"),
            cwd=self.project,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
        self.assertNotEqual(token_mismatch.returncode, 0)
        self.assertIn("metadata mismatch", token_mismatch.stdout + token_mismatch.stderr)
        self.assert_config({"staged.json": b'{"staged":true}'})

        manifest["firmwareLockToken"] = "abandoned-lock"
        manifest["files"][0]["backupFile"] = "../outside.bin"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        path_escape = subprocess.run(
            self.command("recover"),
            cwd=self.project,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
        self.assertNotEqual(path_escape.returncode, 0)
        self.assertIn("Unexpected backup path", path_escape.stdout + path_escape.stderr)
        self.assert_config({"staged.json": b'{"staged":true}'})

    def test_multiple_ready_tokens_are_refused_as_ambiguous(self):
        (self.config / "guard.json").write_bytes(b"guard-original")
        self.run_mode("abandon")
        first = self.journal_directories()[0]
        second_token = "b" * 32 if first.name != "b" * 32 else "c" * 32
        second = first.parent / second_token
        shutil.copytree(first, second)

        owner_path = second / "owner.json"
        owner = json.loads(owner_path.read_text(encoding="utf-8"))
        owner["stageToken"] = second_token
        owner_path.write_text(json.dumps(owner), encoding="utf-8")
        manifest_path = second / "journal.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["stageToken"] = second_token
        manifest["owner"]["stageToken"] = second_token
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

        result = subprocess.run(
            self.command("recover"),
            cwd=self.project,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Multiple recoverable", result.stdout + result.stderr)
        self.assert_config({"staged.json": b'{"staged":true}'})

    def test_config_subdirectory_is_rejected_before_snapshot_or_mutation(self):
        original = {"flat.json": b"flat-original"}
        (self.config / "flat.json").write_bytes(original["flat.json"])
        nested = self.config / "nested"
        nested.mkdir()
        (nested / "secret.json").write_bytes(b"must-not-be-packaged")

        result = subprocess.run(
            self.command("normal"),
            cwd=self.project,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("flat file set", result.stdout + result.stderr)
        self.assertEqual((self.config / "flat.json").read_bytes(), original["flat.json"])
        self.assertEqual((nested / "secret.json").read_bytes(), b"must-not-be-packaged")
        self.assertEqual(self.journal_directories(), [])

    def test_reparse_backup_and_journal_root_are_rejected_when_supported(self):
        (self.config / "guard.json").write_bytes(b"guard-original")
        self.run_mode("abandon")
        journal = self.journal_directories()[0]
        backup = journal / "files" / "0000.bin"
        outside_file = pathlib.Path(self.temp.name) / "outside.bin"
        outside_file.write_bytes(backup.read_bytes())
        backup.unlink()
        try:
            backup.symlink_to(outside_file)
        except OSError as error:
            self.skipTest(f"file symlinks are unavailable: {error}")

        backup_result = subprocess.run(
            self.command("recover"),
            cwd=self.project,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
        self.assertNotEqual(backup_result.returncode, 0)
        self.assertIn("backup must not be a reparse point", backup_result.stdout + backup_result.stderr)
        self.assertTrue(journal.exists(), "unsafe journal must remain available for manual recovery")
        self.assert_config({"staged.json": b'{"staged":true}'})

        # Use a separate project because the intentionally unsafe journal above
        # must remain untouched.
        root_project = pathlib.Path(self.temp.name) / "root-reparse-project"
        root_config = root_project / "data" / "config"
        root_config.mkdir(parents=True)
        (root_config / "root.json").write_bytes(b"root-original")
        outside_directory = pathlib.Path(self.temp.name) / "outside-journals"
        outside_directory.mkdir()
        journal_root = root_project / ".pio" / "config-backups"
        journal_root.parent.mkdir(parents=True)
        try:
            journal_root.symlink_to(outside_directory, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"directory symlinks are unavailable: {error}")
        result = subprocess.run(
            [
                POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", str(self.harness), "-Helper", str(HELPER),
                "-ProjectRoot", str(root_project), "-Mode", "normal",
            ],
            cwd=root_project,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("staging root must not be a reparse point", result.stdout + result.stderr)
        self.assertEqual((root_config / "root.json").read_bytes(), b"root-original")

    def test_deploy_orders_recovery_and_journal_inside_firmware_lock(self):
        source = DEPLOY.read_text(encoding="utf-8")
        lock = source.index("$firmwareLock = Enter-FastBeeFirmwareBuildLock")
        recover = source.index("Recover-FastBeeAbandonedConfigStaging", lock)
        journal = source.index("New-FastBeeConfigStagingJournal", recover)
        clear = source.index("Clear-StagedConfiguration -ExpectedNames", journal)
        restore = source.index("Restore-FastBeeConfigStagingJournal", clear)
        unlock = source.index("Exit-FastBeeFirmwareBuildLock", restore)
        self.assertLess(lock, recover)
        self.assertLess(recover, journal)
        self.assertLess(journal, clear)
        self.assertLess(clear, restore)
        self.assertLess(restore, unlock)


if __name__ == "__main__":
    unittest.main()
