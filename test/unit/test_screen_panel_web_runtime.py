"""Exercise the real lazy loader and screen-panel config wire conversion."""

import json
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
NODE_PROBE = r"""
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { minifyJS } = require('./scripts/minify-js');

async function probe(minified) {
  const requests = [];
  let context;
  const sandbox = { console, setTimeout, clearTimeout, AppState: {},
    document: {
      querySelector() { return null; },
      createElement() { return { dataset: {}, remove() {} }; },
      head: { appendChild(script) {
        requests.push(script.dataset.module);
        queueMicrotask(() => {
          try {
            const file = path.join('web-src/modules/runtime', script.dataset.module + '.js');
            const source = fs.readFileSync(file, 'utf8');
            vm.runInContext(minified ? minifyJS(source) : source, context, { filename: file });
            script.onload();
          } catch (error) { throw error; }
        });
      } }
    }
  };
  sandbox.window = sandbox;
  context = vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync('web-src/js/module-loader.js', 'utf8'), context);
  sandbox.AppState.registerModule = (name, methods) => sandbox.ModuleLoader.registerModule(name, methods);
  const beforeNavigation = [...requests];
  const load = () => new Promise(resolve => sandbox.ModuleLoader.loadModule('screen-panel', resolve));
  await Promise.all([load(), load()]);
  await load();
  const app = sandbox.AppState;
  const helpers = app._screenPanelModelHelpers;
  const input = {
    schemaVersion: 1, displayRef: 'lcd', token: 'do-not-save',
    settings: { homePageId: 'home', background: { presetId: 'soft-gradient' }, password: 'secret' },
    pages: [{ id: 'home', title: 'Home', layoutId: 'detail', logicalWidth: 320, logicalHeight: 240,
      runtime: { value: 99 }, cards: [
        { id: 'trend', kind: 'chart', visualType: 'line', sourceRef: 'sensor:temp', cachedValue: 18 },
        { id: 'link', kind: 'page-link', pageId: 'home' },
        { id: 'restart', kind: 'system', requireConfirmation: true },
        { id: 'switch', kind: 'control', visualType: 'switch', controlRef: 'relay:1' }
      ] }]
  };
  const original = JSON.stringify(input);
  const normalized = helpers.normalizeConfig(input);
  const wire = helpers.sanitizeConfig(normalized, [{ ref: 'relay:1', type: 'switch' }]);
  const template = helpers.normalizeConfig({ pages: [{ id: 'home', layoutId: 'detail',
    logicalWidth: 320, logicalHeight: 240, cards: [{ id: 'clock', kind: 'clock' }] }] });
  const validLayout = app._spValidateConfig(template);
  const invalid = helpers.normalizeConfig({ pages: [{ id: 'home', layoutMode: 'canvas',
    logicalWidth: 320, logicalHeight: 240, cards: [
      { id: 'relay', kind: 'control', controlRef: 'missing', rect: { x: 0, y: 0, w: 32, h: 32 } },
      { id: 'restart', kind: 'system', confirm: false, rect: { x: 20, y: 20, w: 60, h: 60 } }
    ] }] });
  const invalidLayout = app._spValidateConfig(invalid);
  const templateChoices = [
    { ref: 'weather:city', kind: 'weather', available: true },
    { ref: 'switch:lamp', kind: 'control', type: 'switch', available: true },
    { ref: 'exec:scene', kind: 'control', type: 'action', available: true, requiresConfirmation: true },
    { ref: 'exec:hidden', kind: 'control', type: 'action', available: false },
    { ref: 'exec:denied', kind: 'control', type: 'action', available: true, allowPanelControl: false }
  ];
  const draft = helpers.defaultConfig();
  const originalDraft = JSON.stringify(draft);
  const executePage = helpers.buildPageTemplate('execute', draft, templateChoices, { width: 480, height: 480 }, 0, 'quick');
  const weatherPage = helpers.buildPageTemplate('weather', draft, [], { width: 480, height: 480 }, 0, 'weather');
  const controlPage = helpers.buildPageTemplate('control', draft, [], { width: 480, height: 480 }, 0, 'control');
  const pendingLayout = app._spValidateConfig(helpers.normalizeConfig({ pages: [weatherPage, controlPage] }));
  const originalModelPage = app._spCurrentPage().id;
  const editor = app;
  editor._spModel = null;
  const editorModel = editor._spEnsureModel();
  editorModel.capabilities = { limits: { maxPages: 8, maxCards: 24, maxCardsPerPage: 12 } };
  editorModel.choices = templateChoices;
  editor._spMutate = fn => fn(editorModel.config);
  const messages = [];
  editor._spSetMessage = (message, tone) => messages.push({ message, tone });
  sandbox.prompt = (message, suggested) => suggested;
  let pagePreset = 'blank';
  sandbox.document.getElementById = id => id === 'sp-page-preset' ? { value: pagePreset } : null;
  const preservedHome = JSON.stringify(editorModel.config.pages[0]);
  for (const preset of ['weather', 'control', 'execute', 'blank', 'blank', 'blank', 'blank']) {
    pagePreset = preset;
    editor._spAddPage();
  }
  const eightPages = JSON.stringify(editorModel.config.pages);
  editor._spAddPage();
  const pageLimitPreserved = JSON.stringify(editorModel.config.pages) === eightPages;
  editorModel.config.pages.pop();
  editorModel.capabilities.limits.maxCards = 1;
  pagePreset = 'weather';
  const beforeCardLimit = JSON.stringify(editorModel.config);
  editor._spAddPage();
  const cardLimitPreserved = beforeCardLimit === JSON.stringify(editorModel.config);
  editorModel.capabilities.limits.maxCards = 24;
  editorModel.selectedPageId = editorModel.config.pages[editorModel.config.pages.length - 1].id;
  editor._spAddCard('execute');
  const executeCard = editor._spCurrentCard();
  const interactionConfig = helpers.normalizeConfig({ settings: { brightnessPercent: 0, touchBeepEnabled: false, controlConfirmIdleSeconds: 15 } });
  const systemPage = helpers.buildPageTemplate('system', interactionConfig, [], { width: 480, height: 480 }, 0, 'system');
  interactionConfig.pages = [systemPage]; interactionConfig.settings.homePageId = 'system';
  const systemWire = helpers.sanitizeConfig(interactionConfig, []);
  const systemValidation = editor._spValidateConfig(interactionConfig);
  const invalidIntervals = [0,14,601,30.5].map(seconds => {
    const test = helpers.normalizeConfig(interactionConfig); test.settings.controlConfirmIdleSeconds = seconds;
    return editor._spValidateConfig(test).errors.some(message => message.includes('15 到 600'));
  });
  const motorConfirmation = helpers.sanitizeConfig({ pages: [{ id: 'motor', cards: [{ id:'m', kind:'control', visualType:'motor', requireConfirmation:false }] }] }).pages[0].cards[0].requireConfirmation;
  return { requests, beforeNavigation, inputUnchanged: JSON.stringify(input) === original,
    interaction: { defaults: helpers.defaultConfig().settings, systemWire, systemValidation, invalidIntervals, motorConfirmation },
    templates: { executePage, weatherPage, controlPage, pendingLayout,
      inputUnchanged: JSON.stringify(draft) === originalDraft,
      addedPages: JSON.parse(eightPages), pageLimitPreserved,
      existingPagePreserved: preservedHome === JSON.stringify(editorModel.config.pages[0]),
      cardLimitPreserved, executeCard, messages },
    normalized, wire, modelPage: originalModelPage,
    validLayout, templateRect: template.pages[0].cards[0].rect, invalidLayout,
    helpersRegistered: sandbox.ModuleLoader.isLoaded('screen-panel-model'),
    editorRegistered: sandbox.ModuleLoader.isLoaded('screen-panel') };
}
(async () => {
  process.stdout.write(JSON.stringify({ source: await probe(false), minified: await probe(true) }));
})().catch(error => { console.error(error); process.exit(1); });
"""


class ScreenPanelWebRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run(
            ["node", "-e", NODE_PROBE], cwd=ROOT, check=True,
            capture_output=True, text=True, encoding="utf-8", timeout=15,
        )
        cls.results = json.loads(result.stdout)

    def test_touch_settings_and_system_templates_roundtrip_without_enabling_restart(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                interaction = result['interaction']
                self.assertTrue(interaction['defaults']['touchBeepEnabled'])
                self.assertEqual(60, interaction['defaults']['controlConfirmIdleSeconds'])
                self.assertFalse(interaction['defaults']['allowSystemRestart'])
                settings = interaction['systemWire']['settings']
                self.assertEqual(0, settings['brightnessPercent'])
                self.assertFalse(settings['touchBeepEnabled'])
                self.assertEqual(15, settings['controlConfirmIdleSeconds'])
                cards = interaction['systemWire']['pages'][0]['cards']
                self.assertEqual(['brightness','sleep-time','touch-beep','confirm-idle','sleep'], [card['systemActionId'] for card in cards])
                self.assertEqual(['slider','select','switch','select','action'], [card['visualType'] for card in cards])
                self.assertEqual([], interaction['systemValidation']['errors'])
                self.assertTrue(all(interaction['invalidIntervals']))
                self.assertTrue(interaction['motorConfirmation'])

    def test_navigation_loads_model_before_editor_once_for_concurrent_callers(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                self.assertEqual([], result["beforeNavigation"])
                self.assertEqual(["screen-panel-model", "screen-panel"], result["requests"])
                self.assertTrue(result["helpersRegistered"])
                self.assertTrue(result["editorRegistered"])
                self.assertEqual("home", result["modelPage"])

    def test_wire_aliases_round_trip_through_the_extracted_helpers(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                self.assertTrue(result["inputUnchanged"])
                page = result["normalized"]["pages"][0]
                self.assertEqual("data-detail", page["layoutId"])
                self.assertEqual({"width": 320, "height": 240}, page["canvas"])
                self.assertEqual("gradient", result["normalized"]["settings"]["background"]["presetId"])
                wire_page = result["wire"]["pages"][0]
                self.assertEqual("detail", wire_page["layoutId"])
                self.assertEqual(320, wire_page["logicalWidth"])
                self.assertEqual("recent10", wire_page["cards"][0]["historyMode"])
                self.assertEqual("home", wire_page["cards"][1]["pageId"])
                self.assertEqual("restart", wire_page["cards"][2]["systemActionId"])
                self.assertTrue(wire_page["cards"][2]["requireConfirmation"])
                self.assertEqual("switch", wire_page["cards"][3]["visualType"])

    def test_persisted_config_excludes_credentials_and_runtime_values(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                wire = result["wire"]
                self.assertEqual({"schemaVersion", "displayRef", "settings", "pages"}, set(wire))
                self.assertNotIn("password", wire["settings"])
                self.assertNotIn("runtime", wire["pages"][0])
                self.assertNotIn("cachedValue", wire["pages"][0]["cards"][0])

    def test_layout_validation_preserves_geometry_references_and_restart_guards(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                self.assertEqual({"errors": [], "cardCount": 1}, result["validLayout"])
                rect = result["templateRect"]
                self.assertGreater(rect["w"], 0)
                self.assertGreater(rect["h"], 0)
                self.assertLessEqual(rect["x"] + rect["w"], 320)
                self.assertLessEqual(rect["y"] + rect["h"], 240)
                self.assertEqual(2, result["invalidLayout"]["cardCount"])
                errors = "\n".join(result["invalidLayout"]["errors"])
                for reason in ("超出画布或尺寸过小", "控制项已不可用", "重叠", "重启许可", "二次确认"):
                    self.assertIn(reason, errors)

    def test_page_templates_bind_only_available_authorized_actions_and_allow_pending_cards(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                templates = result["templates"]
                self.assertTrue(templates["inputUnchanged"])
                cards = templates["executePage"]["cards"]
                self.assertEqual(["exec:scene"], [card["controlRef"] for card in cards])
                self.assertEqual("action", cards[0]["display"])
                self.assertTrue(cards[0]["confirm"])
                self.assertEqual("control", templates["executeCard"]["kind"])
                self.assertEqual("action", templates["executeCard"]["display"])
                self.assertEqual("", templates["executeCard"]["controlRef"])
                self.assertTrue(templates["executeCard"]["confirm"])
                self.assertFalse(any(card["kind"] == "weather" for card in templates["weatherPage"]["cards"]))
                self.assertTrue(all(card["controlRef"] == "" for card in templates["controlPage"]["cards"]))
                self.assertEqual([], templates["pendingLayout"]["errors"])

    def test_add_page_preserves_existing_pages_and_uses_device_limits_not_three_pages(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                templates = result["templates"]
                self.assertEqual(8, len(templates["addedPages"]))
                self.assertTrue(templates["existingPagePreserved"])
                self.assertTrue(templates["pageLimitPreserved"])
                self.assertTrue(templates["cardLimitPreserved"])
                pages = templates["addedPages"]
                ids = [card["id"] for page in pages for card in page["cards"]]
                self.assertEqual(len(ids), len(set(ids)))
                self.assertEqual("error", templates["messages"][-2]["tone"])


if __name__ == "__main__":
    unittest.main()
