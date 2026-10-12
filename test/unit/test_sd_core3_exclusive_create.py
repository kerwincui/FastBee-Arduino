"""Compile actual installed Core3 FS/VFS methods with explicit filesystem seams.

This verifies SDK mode/create forwarding and existence-error behavior. It does
not execute device FAT; the installed target libc/FAT disassembly is separately
recorded in the preparation artifact, not replaced by assumptions about host IO.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ["FASTBEE_TEST_ROOT"]) if os.environ.get("FASTBEE_TEST_ROOT") else next(
    parent for parent in HERE.parents if (parent / "platformio.ini").is_file())
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))
sys.path.insert(0, str(SUPPORT / "test/unit"))
from test_periph_exec_fixed_json import definition


class SdCore3ExclusiveCreateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages"
        sdk = Path(os.environ.get("FASTBEE_TEST_ARDUINO_FS_SRC",
                                  packages / "framework-arduinoespressif32/libraries/FS/src"))
        fs_path, vfs_path = sdk / "FS.cpp", sdk / "vfs_api.cpp"
        harness_path = HERE / "cpp/sd_core3_exclusive_create_harness.cpp.in"
        if not fs_path.is_file() or not vfs_path.is_file():
            raise RuntimeError("Registered Core3 FS sources required for the real SDK extraction")
        cls.inputs = (Path(__file__), fs_path, vfs_path, harness_path,
                      SUPPORT / "test/unit/test_periph_exec_fixed_json.py")
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        fs_source, vfs_source = fs_path.read_text(encoding="utf-8"), vfs_path.read_text(encoding="utf-8")
        methods = [definition(fs_source, signature) for signature in
                   ("File FS::open(const String &path,", "File FS::open(const char *path,")]
        methods += [definition(vfs_source, signature) for signature in
                    ("FileImplPtr VFSImpl::open(", "bool VFSImpl::exists(",
                     "VFSFileImpl::VFSFileImpl(", "VFSFileImpl::~VFSFileImpl()",
                     "void VFSFileImpl::close()", "VFSFileImpl::operator bool()")]
        program = harness_path.read_text(encoding="utf-8")
        if program.count("@SDK_METHODS@") != 1:
            raise AssertionError("Expected one SDK methods marker")
        program = program.replace("@SDK_METHODS@", "\n\n".join(methods))
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-sd-core3-exclusive-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        cpp = directory / "core3.cpp"
        cpp.write_text(program, encoding="utf-8")
        cls.binary = directory / ("core3.exe" if os.name == "nt" else "core3")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                 str(cpp), "-o", str(cls.binary)], text=True, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=90, env=cls.environment)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}:
            raise AssertionError("Core3 SDK/fixture extraction inputs drifted during execution")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], text=True, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=15, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode, result.stdout)


for _mode in ("new", "exists-error", "create-race"):
    setattr(SdCore3ExclusiveCreateTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
