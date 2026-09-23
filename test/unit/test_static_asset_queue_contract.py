"""Contracts for browser-wide serialization of ESP32 static assets."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
INDEX = (ROOT / "web-src/index.html").read_text(encoding="utf-8")
MAIN = (ROOT / "web-src/js/main.js").read_text(encoding="utf-8")
PAGE_LOADER = (ROOT / "web-src/js/page-loader.js").read_text(encoding="utf-8")
MODULE_LOADER = (ROOT / "web-src/js/module-loader.js").read_text(encoding="utf-8")


class StaticAssetQueueContractTests(unittest.TestCase):
    def test_index_defines_one_shared_queue_with_disconnect_settle_time(self):
        self.assertIn("window.__fastbeeStaticAssetChain = Promise.resolve()", INDEX)
        self.assertIn("window.__fastbeeQueueStaticAsset = function(task)", INDEX)
        self.assertIn("setTimeout(resolve, 100)", INDEX)

    def test_boot_chunks_and_service_worker_use_shared_queue(self):
        self.assertGreaterEqual(INDEX.count("window.__fastbeeQueueStaticAsset("), 2)
        self.assertIn("window.__fastbeeQueueStaticAsset(function()", INDEX)
        self.assertIn("window.__fastbeeQueueStaticAsset(registerWorker)", INDEX)

    def test_css_pages_and_modules_use_the_same_queue(self):
        self.assertIn("window.__fastbeeQueueStaticAsset(loadStylesheet)", MAIN)
        self.assertIn("window.__fastbeeQueueStaticAsset(op)", PAGE_LOADER)
        self.assertIn("window.__fastbeeQueueStaticAsset(startRequest)", MODULE_LOADER)

    def test_page_load_enqueues_a_callable_instead_of_an_eager_promise(self):
        load_start = PAGE_LOADER.index("async loadPage(pageId, pageMapping, options)")
        load_end = PAGE_LOADER.index("preloadPages(pageIds, pageMapping, options)", load_start)
        load_block = PAGE_LOADER[load_start:load_end]

        self.assertIn("this._enqueueOp(async () => {", load_block)
        self.assertNotIn("this._enqueueOp((async () => {", load_block)
        self.assertNotIn("})());", load_block)

    def test_page_fetch_timeout_cannot_block_shared_queue_forever(self):
        self.assertIn("var PAGE_FETCH_TIMEOUT_MS = 8000", PAGE_LOADER)
        self.assertIn("new AbortController()", PAGE_LOADER)
        self.assertIn("controller.abort()", PAGE_LOADER)
        self.assertIn("clearTimeout(timer)", PAGE_LOADER)


if __name__ == "__main__":
    unittest.main()
