"""Exercise inherited PlatformIO flags against the production Node validator."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BuildMatrixRegexContractTests(unittest.TestCase):
    def test_zero_define_is_rejected_and_explicit_undef_is_inherited(self):
        probe = r'''
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { validateAsyncWebServerRegexDisabled: validate, extractPlatformioEnvNames: envs } =
    require('./scripts/validate-build-matrix');
function check(source) { const issues=[]; validate(source,envs(source),issues); return issues; }
const fixture = (base, local='') => `
[base]
build_flags =
    ${base}
[shared]
build_flags =
    \${base.build_flags}
[env:esp32-F4R0]
build_flags =
    \${shared.build_flags}
    ${local}
[env:esp32c6-F4R0]
build_flags =
    \${shared.build_flags}
[env:native]
build_flags = -DUNIT_TEST
`;
assert.deepEqual(check(fixture('-UASYNCWEBSERVER_REGEX')),[]);
assert.deepEqual(check(fixture('-U ASYNCWEBSERVER_REGEX')),[]);
assert.deepEqual(check(fixture('-UASYNCWEBSERVER_REGEX -DASYNCWEBSERVER_REGEX_OTHER=1')),[]);
for (const define of ['-DASYNCWEBSERVER_REGEX=0','-DASYNCWEBSERVER_REGEX=1',
                      '-DASYNCWEBSERVER_REGEX','-D ASYNCWEBSERVER_REGEX=0']) {
    const issues=check(fixture(define));
    assert.equal(issues.filter(issue=>issue.includes('must not define')).length,2);
    assert.equal(issues.filter(issue=>issue.includes('missing -U')).length,2);
}
// A trailing undef must not hide a forbidden inherited definition.
assert.ok(check(fixture('-DASYNCWEBSERVER_REGEX=0','-UASYNCWEBSERVER_REGEX'))
    .some(issue=>issue.includes('[env:esp32-F4R0]') && issue.includes('must not define')));
const direct=check(fixture('-UASYNCWEBSERVER_REGEX','-DASYNCWEBSERVER_REGEX=0'));
assert.equal(direct.length,1); assert.match(direct[0],/esp32-F4R0/);
assert.equal(check(fixture('-DUNRELATED=1')).length,2);
const actual=fs.readFileSync('platformio.ini','utf8');
assert.equal(envs(actual).filter(env=>env!=='native').length,9);
assert.deepEqual(check(actual),[]);
console.log('AsyncWebServer regex flag regression passed: inherited, direct, zero-valued and missing undef cases');
'''
        result = subprocess.run(["node", "-e", probe], cwd=ROOT, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=30)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
