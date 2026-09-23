import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TEST_MAIN = (ROOT / "test" / "test_main.cpp").read_text(encoding="utf-8")
MOCK_PERIPHERAL = (ROOT / "test" / "mocks" / "MockPeripheral.h").read_text(
    encoding="utf-8"
)


class NativeHarnessContractTests(unittest.TestCase):
    def test_mock_peripheral_types_are_namespaced(self):
        """Prevent mock and production PeripheralConfig ODR collisions."""
        self.assertIn("namespace FastBeeTest {", MOCK_PERIPHERAL)
        self.assertIn("}  // namespace FastBeeTest", MOCK_PERIPHERAL)

    def test_per_test_delay_is_excluded_from_native_build(self):
        """Keep the 100 ms device settling delay out of the 2k+ host tests."""
        tear_down = re.search(
            r"void\s+tearDown\s*\(\s*\)\s*\{(?P<body>.*?)\n\}",
            TEST_MAIN,
            re.S,
        )
        self.assertIsNotNone(tear_down)
        body = tear_down.group("body")
        self.assertRegex(
            body,
            r"#ifndef\s+UNIT_TEST\s+delay\(100\);\s*//[^\n]*\s+#endif",
        )


if __name__ == "__main__":
    unittest.main()
