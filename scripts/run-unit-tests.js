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

const result = spawnSync(python, ['-m', 'unittest', 'discover', '-s', 'test/unit', '-p', 'test_*.py', '-v'], {
    cwd: root,
    env: { ...process.env, PYTHONDONTWRITEBYTECODE: '1' },
    stdio: 'inherit',
    windowsHide: true
});

if (result.error) {
    console.error(`Unable to start Python interpreter '${python}': ${result.error.message}`);
    process.exit(1);
}
process.exit(result.status === null ? 1 : result.status);
