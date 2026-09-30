"""Keep chart lines within known boot sessions, including restored histories."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROBE = r"""
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const { minifyJS } = require('./scripts/minify-js');
for (const minified of [false, true]) {
 const app = {}, context = { AppState: app, console }; context.window = context;
 vm.createContext(context);
 const source = fs.readFileSync('web-src/modules/runtime/screen-panel-model.js', 'utf8');
 vm.runInContext(minified ? minifyJS(source) : source, context);
 const history = { points: [] }; app._spEnsureModel = () => ({ histories: { temperature: history } });
 const render = () => app._screenPanelSettingsMethods._spLineSvg.call(app, { sourceRef: 'temperature' });
 const lines = () => (render().match(/<polyline /g) || []).length;
 const points = () => Array.from({ length: 4 }, (_, i) => ({ value: 20+i, seq: 1+i*7,
   bootId: 'old-boot', uptimeMs: 1000+i*8000, epochMs: null, restored: true }));
 history.points = points(); assert.equal(lines(), 3, 'same-boot restored samples retain lines');
 history.points[2].bootId = history.points[3].bootId = 'new-boot';
 assert.equal(lines(), 2, 'never bridge boot sessions without a shared clock');
 history.points = points(); history.points.forEach(p => delete p.bootId);
 assert.equal(lines(), 0, 'legacy restored points without boot identity remain disconnected');
 history.points = points(); history.points.forEach((p, i) => { p.restored = false; p.epochMs = 1700000000000 - i*1000; });
 assert.equal(lines(), 3, 'same-boot monotonic time survives wall-clock correction');
 history.points = points(); history.points[2].uptimeMs = 200000; history.points[3].uptimeMs = 208000;
 assert.equal(lines(), 2, 'real sampling gaps remain disconnected');
 history.points = points(); history.points[1].value = null;
 assert.equal(lines(), 1, 'invalid values never bridge neighboring samples');
 history.points = [points()[0]]; assert.equal(lines(), 0);
 assert.equal((render().match(/<circle /g) || []).length, 1);
}
console.log('history continuity passed for source and minified assets');
"""


class ScreenHistoryContinuityTests(unittest.TestCase):
    def test_web_chart_boot_identity_and_clock_boundaries(self):
        result = subprocess.run(['node', '-e', PROBE], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=40)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
