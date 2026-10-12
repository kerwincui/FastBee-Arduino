"""Release tables must not recommend retired or undeclared environments."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert = require('assert/strict'), fs = require('fs');
const {extractPlatformioEnvNames, validateDocumentedEnvironmentRows: validate} = require('./scripts/validate-build-matrix');
const envs = extractPlatformioEnvNames(fs.readFileSync('platformio.ini', 'utf8')).filter(n => n !== 'native');
const tests = {
  current() { const errors = []; validate(envs.map(n => '| `' + n + '` | current |').join('\n'), 'fixture', envs, errors); assert.deepEqual(errors, []); },
  retired() { for (const env of ['esp32-F8R4','esp32s3-F8R4','esp32c3-F8R8','esp32c6-F16R8']) {
    const errors = []; validate('| Environment | Description |\n| `' + env + '` | old |', 'fixture', envs, errors);
    assert.equal(errors.length, 1); assert(errors[0].includes('fixture:2:')); assert(errors[0].includes(env)); } },
  plain() { const errors=[];validate('| esp32-F8R4 | old |','fixture',envs,errors);assert.equal(errors.length,1); },
  narrative() { const errors=[];validate('Historical esp32-F8R4 is not a supported target.','fixture',envs,errors);assert.deepEqual(errors,[]); },
  readmes() { for(const file of ['README.md','README.en.md','scripts/README.md']) {const errors=[];validate(fs.readFileSync(file,'utf8'),file,envs,errors);assert.deepEqual(errors,[]);} }
};
tests[process.argv[1]]();
'''

class BuildMatrixReadmeTests(unittest.TestCase):
    def run_case(self, name):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run([node, '-e', SCRIPT, name], cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

for _case in ('current', 'retired', 'plain', 'narrative', 'readmes'):
    setattr(BuildMatrixReadmeTests, 'test_' + _case, lambda self, name=_case: self.run_case(name))

if __name__ == '__main__':
    unittest.main()
