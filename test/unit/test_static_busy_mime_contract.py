"""Regression checks for MIME-safe low-memory static responses."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTEXT = (ROOT / "src/network/WebHandlerContext.cpp").read_text(encoding="utf-8")


class StaticBusyMimeContractTests(unittest.TestCase):
    def test_js_and_css_busy_responses_keep_resource_mime(self):
        self.assertIn("sendStaticBusyResponse", CONTEXT)
        self.assertIn('contentType.startsWith("application/javascript")', CONTEXT)
        self.assertIn('busyType = "application/javascript; charset=utf-8"', CONTEXT)
        self.assertIn('contentType.startsWith("text/css")', CONTEXT)
        self.assertIn('busyType = "text/css; charset=utf-8"', CONTEXT)

    def test_all_non_core_static_rejection_paths_use_mime_safe_helper(self):
        self.assertGreaterEqual(
            CONTEXT.count("sendStaticBusyResponse(request, contentType);"),
            3,
        )
        self.assertNotIn('contentType == "application/javascript"', CONTEXT)
        self.assertNotIn('contentType == "text/css"', CONTEXT)


if __name__ == "__main__":
    unittest.main()
