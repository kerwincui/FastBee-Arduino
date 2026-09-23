"""Source contracts that preserve the last committed configuration file."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
FILE_UTILS_CPP = (ROOT / "src/utils/FileUtils.cpp").read_text(encoding="utf-8")
NATIVE_UTILS_CPP = (ROOT / "test/native_utils_impl.cpp").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    match = re.search(signature + r"[^;{]*\{", source, re.S)
    if match is None:
        raise AssertionError(f"missing C++ function matching {signature!r}")
    opening = source.find("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated C++ function matching {signature!r}")


def code_only(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


class AtomicConfigCommitContractTests(unittest.TestCase):
    def assert_target_is_never_removed_or_directly_overwritten(
        self, body: str, label: str
    ) -> None:
        code = code_only(body)
        self.assertNotRegex(
            code,
            r"LittleFS\s*\.\s*remove\s*\(\s*path\s*\)",
            f"{label} must preserve the last committed target when rename fails",
        )
        self.assertNotRegex(
            code,
            r"LittleFS\s*\.\s*open\s*\(\s*path\s*,\s*\"w\"\s*\)",
            f"{label} must not fall back to a non-atomic direct overwrite",
        )

    def test_production_commit_is_rename_first_and_failure_safe(self):
        body = function_body(
            FILE_UTILS_CPP, r"\bFileUtils::commitAtomicWrite\s*\("
        )
        code = code_only(body)
        self.assertIn("LittleFS.exists(tmpPath)", code)
        self.assertEqual(
            len(re.findall(r"LittleFS\s*\.\s*rename\s*\(\s*tmpPath\s*,\s*path\s*\)", code)),
            1,
            "commit must make one atomic replacement attempt without delete-and-retry",
        )
        self.assertIn("LittleFS.remove(tmpPath)", code)
        self.assert_target_is_never_removed_or_directly_overwritten(body, "production commit")

    def test_production_string_atomic_write_has_no_destructive_fallback(self):
        body = function_body(FILE_UTILS_CPP, r"\bFileUtils::atomicWriteFile\s*\(")
        code = code_only(body)
        self.assertIn("tmpPath", code)
        self.assertRegex(
            code,
            r"(?:commitAtomicWrite\s*\(\s*tmpPath\s*,\s*path\s*\)|"
            r"LittleFS\s*\.\s*rename\s*\(\s*tmpPath\s*,\s*path\s*\))",
        )
        self.assert_target_is_never_removed_or_directly_overwritten(
            body, "production atomicWriteFile"
        )

    def test_native_commit_matches_failure_safe_production_semantics(self):
        body = function_body(
            NATIVE_UTILS_CPP, r"\bFileUtils::commitAtomicWrite\s*\("
        )
        code = code_only(body)
        self.assertIn("LittleFS.exists(tmpPath)", code)
        self.assertEqual(
            len(re.findall(r"LittleFS\s*\.\s*rename\s*\(\s*tmpPath\s*,\s*path\s*\)", code)),
            1,
            "native commit must not hide delete-and-retry behavior from tests",
        )
        self.assertIn("LittleFS.remove(tmpPath)", code)
        self.assert_target_is_never_removed_or_directly_overwritten(body, "native commit")

    def test_native_string_atomic_write_preserves_committed_target(self):
        body = function_body(NATIVE_UTILS_CPP, r"\bFileUtils::atomicWriteFile\s*\(")
        self.assert_target_is_never_removed_or_directly_overwritten(
            body, "native atomicWriteFile"
        )


if __name__ == "__main__":
    unittest.main()
