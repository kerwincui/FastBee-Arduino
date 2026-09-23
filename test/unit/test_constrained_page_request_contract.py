"""Contracts that keep first-screen requests small on no-PSRAM devices."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PERIPH_EXEC = (ROOT / "web-src/modules/runtime/periph-exec.js").read_text(
    encoding="utf-8"
)
PERIPHERALS = (ROOT / "web-src/modules/runtime/peripherals.js").read_text(
    encoding="utf-8"
)


class ConstrainedPageRequestContractTests(unittest.TestCase):
    def test_type_catalog_is_not_loaded_with_the_first_screen_list(self):
        setup_start = PERIPHERALS.index("setupPeripheralsEvents()")
        setup_end = PERIPHERALS.index("_bindPeripheralModalEvents()", setup_start)
        setup = PERIPHERALS[setup_start:setup_end]
        self.assertNotIn("_loadPeripheralHardwareCapabilities()", setup)

        modal_start = PERIPHERALS.index("async openPeripheralModal")
        modal_end = PERIPHERALS.index("closePeripheralModal()", modal_start)
        modal = PERIPHERALS[modal_start:modal_end]
        self.assertIn("this._loadPeripheralHardwareCapabilities()", modal)

    def test_rule_list_does_not_prefetch_filter_peripherals(self):
        load_start = PERIPH_EXEC.index("loadPeriphExecPage()")
        load_end = PERIPH_EXEC.index("_renderPeriphExecPagination", load_start)
        loader = PERIPH_EXEC[load_start:load_end]

        self.assertIn("apiGet(apiUrl)", loader)
        self.assertNotIn("_populatePeriphExecFilter", loader)

    def test_filter_options_are_lazy_and_deduplicated(self):
        setup_start = PERIPH_EXEC.index("setupPeriphExecEvents()")
        setup_end = PERIPH_EXEC.index("_bindPeriphExecModalEvents()", setup_start)
        setup = PERIPH_EXEC[setup_start:setup_end]
        self.assertIn("peFilter.addEventListener('focus', loadFilterOptions)", setup)
        self.assertIn("peFilter.addEventListener('pointerdown', loadFilterOptions)", setup)

        populate_start = PERIPH_EXEC.index("_populatePeriphExecFilter() {")
        populate_end = PERIPH_EXEC.index("\n        }\n    });", populate_start)
        populate = PERIPH_EXEC[populate_start:populate_end]
        self.assertIn("if (sel._populatePromise) return sel._populatePromise", populate)
        self.assertIn("sel._populatePromise = apiGet('/api/peripherals'", populate)
        self.assertIn("sel._populatePromise = null", populate)


if __name__ == "__main__":
    unittest.main()
