"""Contracts for allocation-bounded static asset cache revalidation."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTEXT = (ROOT / "src/network/WebHandlerContext.cpp").read_text(encoding="utf-8")


class StaticCacheRevalidationContractTests(unittest.TestCase):
    def test_gzip_crc_etag_is_reused_after_rewinding_the_file(self):
        self.assertIn("char staticEtag[11] = {};", CONTEXT)
        self.assertIn("getStaticGzipEtag(openFile, staticEtag)", CONTEXT)
        self.assertIn("file.size() < 8", CONTEXT)
        self.assertIn("file.size() - 8", CONTEXT)
        self.assertIn("if (!openFile.seek(0) || openFile.position() != 0)", CONTEXT)
        self.assertNotIn("hasStaticEtag && !openFile.seek(0)", CONTEXT)

    def test_matching_if_none_match_returns_304_before_taking_a_stream_slot(self):
        block = CONTEXT[CONTEXT.index("char staticEtag[11] = {};") :]
        validator = 'request->hasHeader("If-None-Match")'
        response = "request->beginResponse(304)"
        lease = "acquireStaticInFlightLease()"
        self.assertIn(validator, block)
        self.assertIn('request->header("If-None-Match") == staticEtag', block)
        self.assertIn(response, block)
        self.assertLess(block.index(response), block.index(lease))

    def test_200_and_304_share_cache_and_connection_headers(self):
        self.assertIn("auto addStaticCacheHeaders", CONTEXT)
        self.assertEqual(CONTEXT.count("addStaticCacheHeaders("), 2)
        self.assertIn('target->addHeader("ETag", staticEtag)', CONTEXT)
        self.assertIn('target->addHeader("Cache-Control", "no-cache")', CONTEXT)
        self.assertIn('target->addHeader("Connection", "close")', CONTEXT)


if __name__ == "__main__":
    unittest.main()
