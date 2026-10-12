import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PLATFORMIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")
README_ZH = (ROOT / "README.md").read_text(encoding="utf-8")
README_EN = (ROOT / "README.en.md").read_text(encoding="utf-8")
DOCS_INDEX = (ROOT / "docs" / "README.md").read_text(encoding="utf-8")
PACKAGE = json.loads((ROOT / "package.json").read_text(encoding="utf-8"))


def release_environments():
    return [
        name
        for name in re.findall(r"^\[env:([^\]]+)\]\s*$", PLATFORMIO, re.MULTILINE)
        if name != "native"
    ]


class DocumentationContractTests(unittest.TestCase):
    def test_readmes_list_every_release_environment(self):
        environments = release_environments()
        self.assertEqual(9, len(environments))
        for environment in environments:
            with self.subTest(environment=environment, language="zh"):
                self.assertIn(f"`{environment}`", README_ZH)
            with self.subTest(environment=environment, language="en"):
                self.assertIn(f"`{environment}`", README_EN)

    def test_readmes_describe_the_effective_feature_gate(self):
        gate = re.compile(
            r"compiled(?:（[^\n]+）)?\s+AND\s+userEnabled(?:（[^\n]+）)?\s+"
            r"AND\s+budgetPassed(?:（[^\n]+）)?\s+AND\s+NOT\s+runtimeDisabled"
        )
        self.assertRegex(README_ZH, gate)
        self.assertRegex(README_EN, gate)

    def test_readmes_keep_release_protocol_scope_explicit(self):
        for readme in (README_ZH, README_EN):
            self.assertRegex(readme, r"MQTT/MQTTS.+Modbus RTU")
            for unsupported in ("Modbus TCP", "CoAP"):
                self.assertIn(unsupported, readme)

    def test_docs_index_keeps_six_distinct_public_device_examples(self):
        expected = {
            "docs/device-test-configs/com4-esp32c3-f4r0/config",
            "docs/c6-rgb-led-f4r0",
            "docs/device-test-configs/com16-esp32-f4r0/config",
            "docs/device-test-configs/com15-esp32s3-f16r8/config",
            "docs/waveshare-esp32s3-relay6ch",
            "docs/device-test-configs/com8-com9-esp32s3-touch/config",
        }
        normalized = DOCS_INDEX.replace("\\", "/")
        for source in expected:
            with self.subTest(source=source):
                self.assertIn(source.removeprefix("docs/"), normalized)

    def test_documented_default_test_command_matches_package_entry(self):
        self.assertEqual("npm run test:host", PACKAGE["scripts"]["test"])
        self.assertIn("npm test", README_ZH)
        self.assertIn("npm test", README_EN)

    def test_readmes_document_the_regression_layers(self):
        expected = (
            "npm test",
            "npm run test:regression",
            "npm run test:firmware -- <",
            "npm run test:e2e:collect",
        )
        for readme in (README_ZH, README_EN):
            for command in expected:
                with self.subTest(command=command):
                    self.assertIn(command, readme)

    def test_docs_index_keeps_the_five_device_port_map_without_pinning_dhcp(self):
        expected = {
            "esp32s3-F16R8": "COM15",
            "esp32c3-F4R0": "COM4",
            "esp32c6-F4R0": "COM11",
            "esp32-F4R0": "COM16",
            "esp32s3-F8R0": "COM7",
        }
        for environment, port in expected.items():
            with self.subTest(environment=environment):
                self.assertIn(f"`{environment}`", DOCS_INDEX)
                self.assertIn(f"`{port}`", DOCS_INDEX)
        self.assertIn("DHCP 变化后以串口或路由器实际分配结果为准", DOCS_INDEX)


if __name__ == "__main__":
    unittest.main()
