import json
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "web-src" / "js" / "request-governor.js"


NODE_PROBE = r"""
const fs = require('fs');
const vm = require('vm');

const sourcePath = process.argv[1];
let source = fs.readFileSync(sourcePath, 'utf8');
// Expose the private request function only inside this VM probe so the test
// can verify that an explicitly supplied bearer header remains supported.
const closeIndex = source.lastIndexOf('})();');
source = source.slice(0, closeIndex)
  + '\nwindow.__testRequest = request;\n'
  + source.slice(closeIndex);
const calls = [];
const storage = {
  getItem: (key) => key === 'auth_token' ? 'stale-legacy-token' : null,
  removeItem: () => {},
  setItem: () => {},
};

const sandbox = {
  console,
  Date,
  URL,
  Promise,
  AbortController,
  setTimeout,
  clearTimeout,
  location: { hostname: 'device', origin: 'http://device' },
  localStorage: storage,
  document: {
    querySelectorAll: () => [],
    getElementById: () => null,
  },
  Notification: {
    warning: () => {},
    error: () => {},
  },
  fetch: (url, options) => {
    calls.push({ url, options });
    return Promise.resolve({
      ok: true,
      status: 200,
      headers: { get: () => '' },
      json: () => Promise.resolve({ success: true }),
    });
  },
};
sandbox.window = sandbox;
vm.runInNewContext(source, sandbox, { filename: sourcePath });

(async () => {
  await sandbox.window.apiGet('/api/health');
  await sandbox.window.__testRequest('GET', '/api/health', {
    headers: { Authorization: 'Bearer explicit-token' },
  });
  process.stdout.write(JSON.stringify(calls));
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
"""


class RequestGovernorAuthTests(unittest.TestCase):
    def run_probe(self):
        result = subprocess.run(
            ["node", "-e", NODE_PROBE, str(MODULE)],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        return json.loads(result.stdout)

    def test_legacy_local_storage_token_is_not_sent(self):
        calls = self.run_probe()
        self.assertGreaterEqual(len(calls), 1)
        options = calls[0]["options"]
        self.assertEqual("include", options["credentials"])
        headers = options.get("headers") or {}
        self.assertNotIn("Authorization", headers)
        self.assertNotIn("authorization", headers)

    def test_explicit_bearer_header_is_preserved(self):
        calls = self.run_probe()
        self.assertGreaterEqual(len(calls), 2)
        headers = calls[1]["options"].get("headers") or {}
        self.assertEqual("Bearer explicit-token", headers.get("Authorization"))

    def test_request_path_does_not_read_legacy_token(self):
        source = MODULE.read_text(encoding="utf-8")
        request_start = source.index("function request(")
        error_handler_start = source.index("function _handleHttpError(", request_start)
        request_body = source[request_start:error_handler_start]
        self.assertNotIn(
            "localStorage.getItem('auth_token')",
            request_body,
            "request() must not implicitly promote a legacy storage token to Authorization",
        )
        self.assertIn("getExplicitBearerToken(headers)", request_body)


if __name__ == "__main__":
    unittest.main()
