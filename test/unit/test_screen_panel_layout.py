"""Regression checks for template screen layout collision and footer bounds."""

import json
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]


NODE_PROBE = r"""
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const sandbox = { console, AppState: {} };
sandbox.window = sandbox;
const context = vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path.join('web-src/modules/runtime/screen-panel-model.js'), 'utf8'), context);
const helpers = sandbox.AppState._screenPanelModelHelpers;
const size = { width: 480, height: 480 };
const layoutContext = { _spReferenceSize: () => size };
const overlap = (a, b) => a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
const check = (page) => {
  const rects = page.cards.map(card => card.rect);
  return {
    withinContent: rects.every(rect => rect.x >= 0 && rect.y >= 0 && rect.w > 0 && rect.h > 0 &&
      rect.x + rect.w <= size.width && rect.y + rect.h <= size.height - 72),
    nonOverlapping: rects.every((rect, index) => rects.slice(index + 1).every(other => !overlap(rect, other))),
    rects
  };
};

const dense = helpers.normalizeConfig({ pages: [{ id: 'home', layoutMode: 'template', layoutId: 'mixed-grid',
  logicalWidth: 480, logicalHeight: 480,
  cards: Array.from({ length: 12 }, (_, index) => ({ id: 'card-' + index, kind: 'data' })) }] });
helpers.layoutTemplatePage(dense.pages[0], dense, layoutContext);

const stale = helpers.normalizeConfig({ pages: [{ id: 'home', layoutMode: 'template', layoutId: 'mixed-grid',
  logicalWidth: 480, logicalHeight: 480,
  cards: [
    { id: 'a', kind: 'data', rect: { x: 10, y: 10, w: 240, h: 160 } },
    { id: 'b', kind: 'data', rect: { x: 20, y: 20, w: 240, h: 160 } }
  ] }] });
const staleContext = {
  _spReferenceSize: () => size,
  _spLayoutTemplatePage: (page, config) => helpers.layoutTemplatePage(page, config, staleContext)
};
helpers.ensurePageRects(stale.pages[0], stale, staleContext);

const detail = helpers.normalizeConfig({ pages: [{ id: 'system', layoutMode: 'template', layoutId: 'data-detail',
  logicalWidth: 480, logicalHeight: 480,
  cards: ['brightness', 'sleep-time', 'touch-beep', 'confirm-idle', 'sleep'].map((kind, index) => ({ id: 'system-' + index, kind })) }] });
helpers.layoutTemplatePage(detail.pages[0], detail, layoutContext);

process.stdout.write(JSON.stringify({ dense: check(dense.pages[0]), stale: check(stale.pages[0]), detail: check(detail.pages[0]) }));
"""


class ScreenPanelLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run(
            ["node", "-e", NODE_PROBE], cwd=ROOT, check=True,
            capture_output=True, text=True, encoding="utf-8", timeout=15,
        )
        cls.results = json.loads(result.stdout)

    def test_dense_template_cards_fit_above_navigation_footer_without_overlap(self):
        self.assertTrue(self.results["dense"]["withinContent"])
        self.assertTrue(self.results["dense"]["nonOverlapping"])

    def test_existing_overlapping_template_rects_are_rebuilt(self):
        self.assertTrue(self.results["stale"]["withinContent"])
        self.assertTrue(self.results["stale"]["nonOverlapping"])

    def test_data_detail_template_does_not_put_last_card_under_footer(self):
        self.assertTrue(self.results["detail"]["withinContent"])
        self.assertTrue(self.results["detail"]["nonOverlapping"])


if __name__ == "__main__":
    unittest.main()
