'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.resolve(__dirname, '..');
const candidates = [
    process.env.FASTBEE_PYTHON,
    path.join(os.homedir(), '.platformio', 'penv', 'Scripts', 'python.exe'),
    path.join(os.homedir(), '.platformio', 'penv', 'bin', 'python'),
    'python3',
    'python'
].filter(Boolean);

const python = candidates.find((candidate) => {
    if (path.isAbsolute(candidate)) return fs.existsSync(candidate);
    return true;
});

if (!python) {
    console.error('No Python 3 interpreter found. Set FASTBEE_PYTHON to a Python 3 executable.');
    process.exit(1);
}

// Reject Python 2 or an outdated Python 3 before running the unittest suite, so a
// stale interpreter earlier on PATH fails with a clear message instead of obscure
// syntax/runtime errors deep inside the tests.
const versionCheck = spawnSync(python, ['--version'], { encoding: 'utf8' });
const versionText = `${versionCheck.stdout || ''}${versionCheck.stderr || ''}`.trim();
const versionMatch = versionText.match(/Python\s+(\d+)\.(\d+)(?:\.(\d+))?/);
if (versionCheck.status !== 0 || !versionMatch) {
    console.error(`Unable to determine the Python version of '${python}' (got: ${versionText || 'no output'}). Set FASTBEE_PYTHON to a Python 3 executable.`);
    process.exit(1);
}
const major = Number(versionMatch[1]);
const minor = Number(versionMatch[2]);
if (major !== 3 || minor < 8) {
    console.error(`Python 3.8 or newer is required, but '${python}' reports '${versionText}'. Set FASTBEE_PYTHON to a Python 3 executable.`);
    process.exit(1);
}

// Host C++ harness tests compile with g++ and run the resulting binaries. When an
// older libstdc++/libgcc (e.g. a bundled Git distribution) appears earlier on PATH,
// Windows loads those DLLs and the binaries fail with 0xC0000135/0xC0000139. Pin the
// compiler's own directory first so the matching runtime is always found.
function compilerRuntimeDir() {
    const finder = process.platform === 'win32' ? 'where' : 'which';
    const found = spawnSync(finder, ['g++'], { encoding: 'utf8' });
    if (found.status !== 0) return null;
    const first = (found.stdout || '').split(/\r?\n/).map((line) => line.trim()).find(Boolean);
    return first ? path.dirname(first) : null;
}

const runtimeDir = compilerRuntimeDir();
const env = { ...process.env, PYTHONDONTWRITEBYTECODE: '1' };
if (runtimeDir) {
    env.PATH = runtimeDir + path.delimiter + (env.PATH || '');
}

const result = spawnSync(python, ['-m', 'unittest', 'discover', '-s', 'test/unit', '-p', 'test_*.py', '-v'], {
    cwd: root,
    env,
    stdio: 'inherit',
    windowsHide: true
});

if (result.error) {
    console.error(`Unable to start Python interpreter '${python}': ${result.error.message}`);
    process.exit(1);
}
process.exit(result.status === null ? 1 : result.status);
