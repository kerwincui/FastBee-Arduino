"""SEC-P0-02 network field validation, compiled from the production unit."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SUPPORT = Path(os.environ.get("FASTBEE_TEST_MODBUS_SOURCE", ROOT))
HERE = Path(__file__).resolve().parent


class NetworkFieldValidationTests(unittest.TestCase):
    def test_production_network_utils_field_validators(self):
        source = (SUPPORT / "src/utils/NetworkUtils.cpp").read_text(encoding="utf8")
        methods = []
        for signature in (
            "bool NetworkUtils::isValidIP(",
            "bool NetworkUtils::isValidSSID(",
            "bool NetworkUtils::isValidHostname(",
            "bool NetworkUtils::isValidOptionalIP(",
        ):
            start = source.index(signature)
            methods.append(source[start:source.index("\n}", start) + 2])
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        env = dict(os.environ)
        env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="fastbee-network-field-") as temp:
            temp = Path(temp)
            (temp / "network_field_validators_production.inc").write_text(
                "\n".join(methods), encoding="utf8"
            )
            binary = temp / ("fields.exe" if os.name == "nt" else "fields")
            built = subprocess.run(
                [
                    compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(temp),
                    "-I" + str(SUPPORT / "include"),
                    "-I" + str(SUPPORT / "test/mocks"),
                    str(HERE / "cpp/network_field_validation_harness.cpp"),
                    "-o", str(binary),
                ],
                capture_output=True, text=True, encoding="utf8", env=env, timeout=120,
            )
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_update_config_rejects_invalid_fields_before_mutation(self):
        source = (SUPPORT / "src/network/NetworkManager.cpp").read_text(encoding="utf8")
        start = source.index(
            "bool FBNetworkManager::updateConfig(const WiFiConfig& newConfig")
        body = source[start:source.index("\n}", start)]
        # 校验必须在任何配置落盘/生效之前完成（资源锁与 validateNetworkResourceConfig 之前）。
        validate_at = body.index("validateNetworkFields")
        lock_at = body.index("RecursiveMutexGuard resourceLock")
        mutate_at = body.index("wifiConfig = effectiveConfig;")
        self.assertLess(validate_at, lock_at)
        self.assertLess(validate_at, mutate_at)
        for field in (
            "staSSID", "apSSID", "customDomain", "apIP",
            "staticIP", "gateway", "subnet", "dns1", "dns2",
        ):
            self.assertIn(field, body[validate_at:lock_at])
        self.assertIn("NETWORK_FIELD_INVALID", body[validate_at:lock_at])
        self.assertIn("isValidSSID", body)
        self.assertIn("isValidHostname", body)
        self.assertIn("isValidOptionalIP", body)


if __name__ == "__main__":
    unittest.main()
