import json
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "web-src" / "modules" / "runtime" / "periph-exec.js"


NODE_PROBE = r"""
const fs = require('fs');
const vm = require('vm');

const sourcePath = process.argv[1];
const source = fs.readFileSync(sourcePath, 'utf8');
let registeredModule = null;

const sandbox = {
  AppState: {
    registerModule: function (_name, module) {
      // The browser's state registry merges the module into AppState; the
      // retry classifier is added by a later Object.assign(AppState, ...).
      Object.assign(this, module);
      registeredModule = this;
    },
  },
  console,
  document: { getElementById: () => null },
  setTimeout,
  clearTimeout,
};
vm.runInNewContext(source, sandbox, { filename: sourcePath });

if (!registeredModule || typeof registeredModule._isPeriphExecEditRetryable !== 'function') {
  throw new Error('periph-exec retry classifier was not registered');
}

const genericAbort = new Error('signal is aborted without reason');
genericAbort.name = 'AbortError';
const timeoutAbort = new Error('Request timed out after 15000ms');
timeoutAbort.name = 'AbortError';
timeoutAbort._requestTimeout = true;

process.stdout.write(JSON.stringify({
  genericAbort: registeredModule._isPeriphExecEditRetryable(genericAbort),
  timeoutAbort: registeredModule._isPeriphExecEditRetryable(timeoutAbort),
}));
"""


class PeriphExecAbortClassificationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = MODULE.read_text(encoding="utf-8")

    def run_classifier(self):
        result = subprocess.run(
            ["node", "-e", NODE_PROBE, str(MODULE)],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        return json.loads(result.stdout)

    def test_generic_abort_error_is_not_retryable(self):
        result = self.run_classifier()
        self.assertFalse(
            result["genericAbort"],
            "browser navigation/unmount AbortError must not start another edit-data retry",
        )

    def test_timeout_abort_error_remains_retryable(self):
        result = self.run_classifier()
        self.assertTrue(
            result["timeoutAbort"],
            "request-governor timeout AbortError must retain the recovery retry",
        )


if __name__ == "__main__":
    unittest.main()
