'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const MATRIX_PATH = path.join(ROOT_DIR, 'scripts', 'device-api-test-matrix.json');
const SMOKE_SCRIPT_PATH = path.join(ROOT_DIR, 'scripts', 'smoke-test-device.ps1');
const SOAK_SCRIPT_PATH = path.join(ROOT_DIR, 'scripts', 'soak-test-device.ps1');
const HANDLER_DIR = path.join(ROOT_DIR, 'src', 'network', 'handlers');
const PROFILES = new Set(['lite', 'standard', 'full']);
const METHODS = new Set(['GET', 'POST']);
const TYPES = new Set(['', 'multi-session', 'bearer-over-cookie', 'expect-unavailable', 'expect-error']);
const SEMANTIC_ONLY_CHECKS = new Set([
    'config-transfer-export',
    'config-transfer-import'
]);

const REQUIRED_ALL_PROFILE_CHECKS = [
    'auth-session',
    'auth-multi-session',
    'auth-bearer-over-cookie',
    'system-health',
    'system-health-alias',
    'system-info',
    'system-status',
    'system-web-runtime',
    'system-capabilities',
    'system-metrics',
    'device-config',
    'device-info',
    'device-time',
    'network-status',
    'network-config',
    'mqtt-status',
    'protocol-config',
    'protocol-mqtt-config',
    'peripherals',
    'peripheral-types',
    'peripheral-status-missing-id',
    'periph-exec-rules',
    'periph-exec-controls',
    'periph-exec-trigger-types',
    'periph-exec-static-events',
    'periph-exec-dynamic-events',
    'periph-exec-events',
    'config-transfer-list',
    'batch-basic'
];

const REQUIRED_PROFILE_CHECKS = [
    { name: 'protocol-periph-exec-config', profiles: ['standard', 'full'] },
    { name: 'modbus-status', profiles: ['standard', 'full'] },
    { name: 'modbus-unavailable', profiles: ['lite'] },
    { name: 'filesystem', profiles: ['full'] },
    { name: 'filesystem-unavailable', profiles: ['lite', 'standard'] },
    { name: 'files-root', profiles: ['full'] },
    { name: 'files-unavailable', profiles: ['lite', 'standard'] },
    { name: 'logs-list', profiles: ['full'] },
    { name: 'logs-tail', profiles: ['full'] },
    { name: 'logs-info', profiles: ['full'] },
    { name: 'logs-unavailable', profiles: ['lite', 'standard'] },
    { name: 'ota-status', profiles: ['full'] },
    { name: 'ota-unavailable', profiles: ['lite', 'standard'] },
    { name: 'rule-scripts', profiles: ['full'] },
    { name: 'rule-scripts-unavailable', profiles: ['lite', 'standard'] },
    { name: 'users', profiles: ['full'] },
    { name: 'users-unavailable', profiles: ['lite', 'standard'] }
];

function readJson(filePath) {
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function walk(dir, files) {
    if (!fs.existsSync(dir)) return;
    fs.readdirSync(dir).forEach((entry) => {
        const fullPath = path.join(dir, entry);
        const stat = fs.statSync(fullPath);
        if (stat.isDirectory()) {
            walk(fullPath, files);
        } else if (stat.isFile() && /\.(cpp|h)$/.test(entry)) {
            files.push(fullPath);
        }
    });
}

function normalizeApiPath(value) {
    const raw = String(value || '').trim();
    const pathOnly = raw.split('?')[0];
    if (pathOnly.length > 1 && pathOnly.endsWith('/')) {
        return pathOnly.slice(0, -1);
    }
    return pathOnly;
}

function collectRegisteredApiPaths() {
    const files = [];
    walk(HANDLER_DIR, files);
    const exact = new Set();
    const prefixes = new Set();

    files.forEach((filePath) => {
        const text = fs.readFileSync(filePath, 'utf8');
        const patterns = [
            { regex: /server->on\s*\(\s*"([^"]+)"/g, prefix: false },
            { regex: /server->on\s*\(\s*AsyncURIMatcher::exact\s*\(\s*"([^"]+)"/g, prefix: false },
            { regex: /server->on\s*\(\s*AsyncURIMatcher::prefix\s*\(\s*"([^"]+)"/g, prefix: true },
            { regex: /new\s+AsyncCallbackJsonWebHandler\s*\(\s*"([^"]+)"/g, prefix: false },
            { regex: /AsyncEventSource\s+\w+\s*\(\s*"([^"]+)"/g, prefix: false }
        ];

        patterns.forEach(({ regex, prefix }) => {
            let match = regex.exec(text);
            while (match) {
                const routePath = normalizeApiPath(match[1]);
                if (routePath.startsWith('/api/')) {
                    (prefix ? prefixes : exact).add(routePath);
                }
                match = regex.exec(text);
            }
        });
    });

    return { exact, prefixes };
}

function routeExists(pathValue, registered) {
    const matrixPath = normalizeApiPath(pathValue);
    if (registered.exact.has(matrixPath)) return true;
    for (const prefix of registered.prefixes) {
        if (matrixPath === prefix || matrixPath.startsWith(prefix)) return true;
    }
    return false;
}

function arrayEqualsAsSet(actual, expected) {
    if (actual.length !== expected.length) return false;
    const set = new Set(actual);
    return expected.every((item) => set.has(item));
}

function validateCheck(check, index, issues, registered, names, profileCounts) {
    const label = `checks[${index}]`;
    const name = String(check && check.name || '');
    const method = String(check && check.method || '');
    const type = String(check && check.type || '');
    const profiles = Array.isArray(check && check.profiles) ? check.profiles.map(String) : [];
    const pathValue = String(check && check.path || '');

    if (!/^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(name)) {
        issues.push(`${label}: invalid or missing kebab-case name`);
    } else if (names.has(name)) {
        issues.push(`${label}: duplicate check name '${name}'`);
    } else {
        names.add(name);
    }

    if (!METHODS.has(method)) {
        issues.push(`${label} (${name || 'unnamed'}): method must be GET or POST`);
    }
    if (!TYPES.has(type)) {
        issues.push(`${label} (${name || 'unnamed'}): unsupported type '${type}'`);
    }
    if (!pathValue.startsWith('/api/')) {
        issues.push(`${label} (${name || 'unnamed'}): path must start with /api/`);
    } else if (!routeExists(pathValue, registered)) {
        issues.push(`${label} (${name}): path '${normalizeApiPath(pathValue)}' is not registered by network handlers`);
    }

    if (profiles.length === 0) {
        issues.push(`${label} (${name || 'unnamed'}): profiles must be explicit`);
    }
    const profileSet = new Set();
    profiles.forEach((profile) => {
        if (!PROFILES.has(profile)) {
            issues.push(`${label} (${name || 'unnamed'}): unknown profile '${profile}'`);
        }
        if (profileSet.has(profile)) {
            issues.push(`${label} (${name || 'unnamed'}): duplicate profile '${profile}'`);
        }
        profileSet.add(profile);
        if (PROFILES.has(profile)) profileCounts[profile] += 1;
    });

    const allowedStatuses = Array.isArray(check && check.allowedStatuses) ? check.allowedStatuses : [];
    if (type === 'expect-unavailable' || type === 'expect-error') {
        if (allowedStatuses.length === 0) {
            issues.push(`${label} (${name}): ${type} requires allowedStatuses`);
        }
        allowedStatuses.forEach((status) => {
            if (!Number.isInteger(status) || status < 400 || status > 599) {
                issues.push(`${label} (${name}): invalid allowed status '${status}'`);
            }
        });
    } else if (allowedStatuses.length > 0) {
        issues.push(`${label} (${name}): allowedStatuses is only valid for expect-* checks`);
    }

    if (check && Object.prototype.hasOwnProperty.call(check, 'jsonBody')) {
        if (typeof check.jsonBody !== 'boolean') {
            issues.push(`${label} (${name}): jsonBody must be boolean`);
        }
        if (check.jsonBody === true && method !== 'POST') {
            issues.push(`${label} (${name}): jsonBody=true requires POST`);
        }
        if (check.jsonBody === true && !Object.prototype.hasOwnProperty.call(check, 'body')) {
            issues.push(`${label} (${name}): jsonBody=true requires body`);
        }
    }

    if ((type === 'multi-session' || type === 'bearer-over-cookie') && method !== 'POST' && type === 'multi-session') {
        issues.push(`${label} (${name}): multi-session check must use POST login`);
    }
}

function validateRequiredProfiles(checksByName, issues) {
    REQUIRED_ALL_PROFILE_CHECKS.forEach((name) => {
        const check = checksByName.get(name);
        if (!check) {
            issues.push(`matrix: missing required all-profile check '${name}'`);
            return;
        }
        if (!arrayEqualsAsSet(check.profiles || [], ['lite', 'standard', 'full'])) {
            issues.push(`matrix: '${name}' must cover lite, standard, full`);
        }
    });

    REQUIRED_PROFILE_CHECKS.forEach(({ name, profiles }) => {
        const check = checksByName.get(name);
        if (!check) {
            issues.push(`matrix: missing required profile check '${name}'`);
            return;
        }
        if (!arrayEqualsAsSet(check.profiles || [], profiles)) {
            issues.push(`matrix: '${name}' must cover ${profiles.join(', ')}`);
        }
    });
}

function extractTestCheckSemanticsBody(scriptPath) {
    const text = fs.readFileSync(scriptPath, 'utf8');
    const fnStart = text.indexOf('function Test-CheckSemantics');
    if (fnStart < 0) return '';
    const switchStart = text.indexOf('switch ($Name)', fnStart);
    if (switchStart < 0) return '';
    const openBrace = text.indexOf('{', switchStart);
    if (openBrace < 0) return '';

    let depth = 0;
    for (let i = openBrace; i < text.length; i += 1) {
        const ch = text[i];
        if (ch === '{') depth += 1;
        if (ch === '}') {
            depth -= 1;
            if (depth === 0) {
                return text.slice(openBrace + 1, i);
            }
        }
    }
    return '';
}

function extractSemanticCaseNames(scriptPath) {
    const body = extractTestCheckSemanticsBody(scriptPath);
    const names = new Set();
    const regex = /^\s*"([a-z0-9]+(?:-[a-z0-9]+)*)"\s*\{/gm;
    let match = regex.exec(body);
    while (match) {
        names.add(match[1]);
        match = regex.exec(body);
    }
    return names;
}

function setDifference(left, right) {
    return Array.from(left).filter((item) => !right.has(item)).sort();
}

function validateSemanticParity(checksByName, issues) {
    const smokeCases = extractSemanticCaseNames(SMOKE_SCRIPT_PATH);
    const soakCases = extractSemanticCaseNames(SOAK_SCRIPT_PATH);

    setDifference(smokeCases, soakCases).forEach((name) => {
        issues.push(`semantic checks: smoke-test-device.ps1 has '${name}' but soak-test-device.ps1 does not`);
    });
    setDifference(soakCases, smokeCases).forEach((name) => {
        issues.push(`semantic checks: soak-test-device.ps1 has '${name}' but smoke-test-device.ps1 does not`);
    });

    Array.from(new Set([...smokeCases, ...soakCases])).sort().forEach((name) => {
        if (!checksByName.has(name) && !SEMANTIC_ONLY_CHECKS.has(name)) {
            issues.push(`semantic checks: '${name}' is not present in device-api-test-matrix.json`);
        }
    });

    return smokeCases.size;
}

function main() {
    const matrix = readJson(MATRIX_PATH);
    const checks = Array.isArray(matrix.checks) ? matrix.checks : [];
    const issues = [];
    const registered = collectRegisteredApiPaths();
    const names = new Set();
    const checksByName = new Map();
    const profileCounts = { lite: 0, standard: 0, full: 0 };

    if (!Number.isInteger(matrix.version) || matrix.version < 1) {
        issues.push('matrix: version must be a positive integer');
    }
    if (checks.length === 0) {
        issues.push('matrix: checks must be a non-empty array');
    }

    checks.forEach((check, index) => {
        validateCheck(check, index, issues, registered, names, profileCounts);
        if (check && check.name) checksByName.set(String(check.name), check);
    });
    validateRequiredProfiles(checksByName, issues);
    const semanticCount = validateSemanticParity(checksByName, issues);

    Object.entries(profileCounts).forEach(([profile, count]) => {
        if (count < 20) {
            issues.push(`matrix: ${profile} profile has too few checks (${count})`);
        }
    });

    if (issues.length > 0) {
        console.error('FastBee device API matrix validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }

    console.log(`FastBee device API matrix OK: checks=${checks.length}, semantics=${semanticCount}, lite=${profileCounts.lite}, standard=${profileCounts.standard}, full=${profileCounts.full}`);
}

main();
