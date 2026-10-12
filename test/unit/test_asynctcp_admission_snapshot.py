"""Exercise the actual dependency transformation and optional native provider."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_libdeps_patch_contract import PATCH_MODULE

ROOT = Path(__file__).resolve().parents[2]
HEADER = """#include <stdint.h>
class AsyncClient {
public:
  static void setAccepted(uint32_t value) { _fastbee_accepted_count = value; }
private:
  static uint32_t _fastbee_accepted_count;
};
"""
SOURCE = """#include "AsyncTCP.h"
uint32_t AsyncClient::_fastbee_accepted_count = 3U;
"""


class AsyncTcpAdmissionSnapshotTests(unittest.TestCase):
    def patched(self):
        return PATCH_MODULE._patch_async_tcp_admission_snapshot(HEADER, SOURCE)

    def test_existing_source_is_preserved_and_patch_is_idempotent(self):
        header, source, changed = self.patched()
        self.assertTrue(changed)
        self.assertEqual(header.replace("  static uint32_t fastbeeAcceptedClientSnapshot();\n", "", 1), HEADER)
        self.assertTrue(source.startswith(SOURCE.rstrip()))
        self.assertEqual(
            PATCH_MODULE._patch_async_tcp_admission_snapshot(header, source),
            (header, source, False))

    def test_partial_duplicate_and_changed_providers_are_rejected(self):
        header, source, _ = self.patched()
        declaration = "  static uint32_t fastbeeAcceptedClientSnapshot();\n"
        implementation = source[len(SOURCE.rstrip()):].strip()
        variants = (
            ("partial_header", header, SOURCE),
            ("partial_source", HEADER, source),
            ("missing_counter", HEADER, SOURCE.replace("_fastbee_accepted_count", "removed_counter")),
            ("missing_class", HEADER.replace("class AsyncClient {", "class RemovedClass {"), SOURCE),
            ("duplicate_declaration", header.replace(declaration, declaration + declaration), source),
            ("duplicate_provider", header, source + "\n" + implementation + "\n"),
            ("changed_cap", header, source.replace("*limit = 4U;", "*limit = 99U;")),
            ("changed_provider_result", header, source.replace("  return true;", "  return false;")),
        )
        for name, candidate_header, candidate_source in variants:
            with self.subTest(name=name), self.assertRaises(PATCH_MODULE.PatchError):
                PATCH_MODULE._patch_async_tcp_admission_snapshot(candidate_header, candidate_source)

    def compile_and_run(self, program, limit=None, provider=True):
        # CXX denotes one executable path; it is never parsed as a shell command.
        compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.fail("A C++17 compiler is required (CXX, g++, or clang++)")
        with tempfile.TemporaryDirectory(prefix="fastbee-tcp-admission-") as directory:
            temporary = Path(directory)
            inputs = []
            if provider:
                header, source, _ = self.patched()
                (temporary / "AsyncTCP.h").write_text(header, encoding="utf-8")
                provider_path = temporary / "provider.cpp"
                provider_path.write_text(source, encoding="utf-8")
                inputs.append(str(provider_path))
            main = temporary / "main.cpp"
            main.write_text(program, encoding="utf-8")
            inputs.append(str(main))
            executable = temporary / ("snapshot.exe" if os.name == "nt" else "snapshot")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Os",
                       f"-I{temporary}", f"-I{ROOT / 'include'}"]
            if limit is not None:
                command.append(f"-DCONFIG_ASYNC_TCP_MAX_CONNECTIONS={limit}")
            result = subprocess.run(command + inputs + ["-o", str(executable)],
                                    capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True,
                                    text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_real_provider_reads_current_count_caps_and_rejects_null_arguments(self):
        program = """#include "AsyncTCP.h"
#include "network/AsyncTCPAdmissionSnapshot.h"
int main() {
  const auto first = FastBee::captureAsyncTCPAdmissionSnapshot();
  if (!first.available || first.accepted != 3U || first.limit != CONFIG_ASYNC_TCP_MAX_CONNECTIONS) return 1;
  AsyncClient::setAccepted(7U);
  const auto second = FastBee::captureAsyncTCPAdmissionSnapshot();
  if (!second.available || second.accepted != 7U || second.limit != CONFIG_ASYNC_TCP_MAX_CONNECTIONS) return 2;
  uint32_t count = 17U, cap = 19U;
  if (fastbee_async_tcp_admission_snapshot(nullptr, &cap) || cap != 19U) return 3;
  if (fastbee_async_tcp_admission_snapshot(&count, nullptr) || count != 17U) return 4;
  return 0;
}
"""
        for limit in (4, 6, 8):
            with self.subTest(limit=limit):
                self.compile_and_run(program, limit=limit)

    def test_missing_weak_provider_reports_unavailable(self):
        program = """#include "network/AsyncTCPAdmissionSnapshot.h"
int main() {
  const auto value = FastBee::captureAsyncTCPAdmissionSnapshot();
  return value.available || value.accepted || value.limit;
}
"""
        self.compile_and_run(program, provider=False)


if __name__ == "__main__":
    unittest.main()
