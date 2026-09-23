"""Offline behavior checks for the private-file-preserving LittleFS builder."""

import contextlib
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path, PurePosixPath
import tempfile
import unittest

from littlefs import LittleFS


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/build-preserved-device-fs.py"
SPEC = importlib.util.spec_from_file_location("build_preserved_device_fs", SCRIPT)
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


def digest(content):
    return hashlib.sha256(content).hexdigest()


class PreservedDeviceFsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            prefix="fastbee-preserved-device-fs-"
        )
        self.root = Path(self.temporary.name)
        self.extracted = self.root / "device-extracted"
        self.web = self.root / "published-www"
        self.output = self.root / "artifacts" / "littlefs-preserved.bin"
        self.snapshot = self.root / "device-full-flash.bin"
        self.partition_size = 32 * 4096

        self.private_files = {
            "config/network.json": b'{"ssid":"fixture-network","password":"fixture-only"}',
            "config/device.json": b'{"deviceId":"fixture-device","product":100}',
            "config/protocol.json": b'{"mqtt":{"host":"fixture.invalid"}}',
            "data/point_history.json": b'{"series":[{"value":12.5}]}',
            "runtime/calibration.bin": bytes(range(32)),
        }
        self.old_web_files = {
            "www/index.html.gz": gzip.compress(b"old index"),
            "www/web-resource-manifest.json": b'{"version":"old"}',
            "www/obsolete.js.gz": gzip.compress(b"obsolete"),
        }
        self.new_web_files = {
            "index.html.gz": gzip.compress(b"new index"),
            "web-resource-manifest.json": b'{"version":"new"}',
            "assets/app.js.gz": gzip.compress(b"console.log('fixture')"),
        }

        all_extracted = {**self.private_files, **self.old_web_files}
        for relative, content in all_extracted.items():
            target = self.extracted / Path(*PurePosixPath(relative).parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        for relative, content in self.new_web_files.items():
            target = self.web / Path(*PurePosixPath(relative).parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)

        snapshot_content = b"offline fixture full-flash evidence\0" * 8
        self.snapshot.write_bytes(snapshot_content)
        self.manifest_path = self.extracted.parent / (
            self.extracted.name + "-manifest.json"
        )
        manifest = {
            "snapshot": str(self.snapshot.resolve()),
            "sha256": digest(snapshot_content),
            "flashBytes": len(snapshot_content),
            "partitions": [
                {
                    "type": 1,
                    "subtype": 130,
                    "offset": 0x710000,
                    "size": self.partition_size,
                    "name": "spiffs",
                    "flags": 0,
                }
            ],
            "files": [
                {"path": name, "bytes": len(content), "sha256": digest(content)}
                for name, content in sorted(all_extracted.items())
            ],
        }
        self.manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )

    def tearDown(self):
        self.temporary.cleanup()

    def build(self):
        with contextlib.redirect_stdout(io.StringIO()):
            BUILDER.build(self.extracted, self.web, self.output)

    def read_image_files(self):
        image = self.output.read_bytes()
        filesystem = LittleFS(
            block_size=4096, block_count=len(image) // 4096, mount=False
        )
        filesystem.context.buffer = bytearray(image)
        filesystem.mount()
        files = {}
        try:
            for root, _, names in filesystem.walk("/"):
                for name in names:
                    relative = str(PurePosixPath(root, name)).lstrip("/")
                    with filesystem.open("/" + relative, "rb") as stream:
                        files[relative] = stream.read()
        finally:
            filesystem.unmount()
        return image, files

    def assert_no_output(self):
        self.assertFalse(self.output.exists())
        self.assertFalse(self.output.with_suffix(".json").exists())

    def test_preserves_private_files_and_replaces_entire_www_tree(self):
        self.build()
        _, files = self.read_image_files()

        for name, expected in self.private_files.items():
            self.assertEqual(files[name], expected, name)
        for name, expected in self.new_web_files.items():
            self.assertEqual(files["www/" + name], expected, name)
        self.assertNotIn("www/obsolete.js.gz", files)
        self.assertEqual(
            set(files),
            set(self.private_files)
            | {"www/" + name for name in self.new_web_files},
        )

    def test_report_hashes_match_remounted_image_and_files(self):
        self.build()
        image, files = self.read_image_files()
        report = json.loads(
            self.output.with_suffix(".json").read_text(encoding="utf-8")
        )

        self.assertEqual(len(image), self.partition_size)
        self.assertEqual(report["filesystemBytes"], self.partition_size)
        self.assertEqual(report["sha256"], digest(image))
        self.assertEqual(report["backupSha256"], digest(self.snapshot.read_bytes()))
        reported = {entry["path"]: entry for entry in report["files"]}
        self.assertEqual(set(reported), set(files))
        for name, content in files.items():
            self.assertEqual(reported[name]["bytes"], len(content), name)
            self.assertEqual(reported[name]["sha256"], digest(content), name)
        self.assertEqual(report["preservedFiles"], sorted(self.private_files))

    def test_rejects_changed_full_flash_backup(self):
        self.snapshot.write_bytes(self.snapshot.read_bytes() + b"tampered")

        with self.assertRaisesRegex(ValueError, "backup no longer matches"):
            self.build()

        self.assert_no_output()

    def test_rejects_changed_extracted_file(self):
        changed = self.extracted / "config" / "device.json"
        changed.write_bytes(b'{"deviceId":"changed-after-extraction"}')

        with self.assertRaisesRegex(ValueError, "Extracted file changed"):
            self.build()

        self.assert_no_output()

    def test_refuses_to_overwrite_existing_image_or_report(self):
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.output.write_bytes(b"existing-image-sentinel")
        with self.assertRaisesRegex(ValueError, "Refusing to overwrite"):
            self.build()
        self.assertEqual(self.output.read_bytes(), b"existing-image-sentinel")

        self.output.unlink()
        report = self.output.with_suffix(".json")
        report.write_text("existing-report-sentinel", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Refusing to overwrite"):
            self.build()
        self.assertFalse(self.output.exists())
        self.assertEqual(
            report.read_text(encoding="utf-8"), "existing-report-sentinel"
        )


if __name__ == "__main__":
    unittest.main()
