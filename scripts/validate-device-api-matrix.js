'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const MATRIX_PATH = path.join(ROOT_DIR, 'scripts', 'device-api-test-matrix.json');
const SMOKE_SCRIPT_PATH = path.join(ROOT_DIR, 'scripts', 'smoke-test-device.ps1');
const SOAK_SCRIPT_PATH = path.join(ROOT_DIR, 'scripts', 'soak-test-device.ps1');
const HANDLER_DIR = path.join(ROOT_DIR, 'src', 'network', 'handlers');
const CAPABILITIES = new Set(['modbus', 'webServer', 'logViewer', 'ota', 'ruleScript', 'auth']);
const METHODS = new Set(['GET', 'POST']);
const TYPES = new Set(['', 'multi-session', 'bearer-over-cookie', 'expect-unavailable', 'expect-error', 'capability-optional', 'mqtt-ntp-sync', 'batch-stress']);
const SEMANTIC_ONLY_CHECKS = new Set([
    'config-transfer-export',
    'config-transfer-import'
]);

const REQUIRED_COMMON_CHECKS = [
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
    'protocol-periph-exec-config',
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

const REQUIRED_CAPABILITY_CHECKS = [
    {
        "name": "modbus-status",
        "capability": "modbus",
        "capabilityEnabled": true
    },
    {
        "name": "modbus-unavailable",
        "capability": "modbus",
        "capabilityEnabled": false
    },
    {
        "name": "filesystem",
        "capability": "webServer",
        "capabilityEnabled": true
    },
    {
        "name": "filesystem-unavailable",
        "capability": "webServer",
        "capabilityEnabled": false
    },
    {
        "name": "files-root",
        "capability": "webServer",
        "capabilityEnabled": true
    },
    {
        "name": "files-unavailable",
        "capability": "webServer",
        "capabilityEnabled": false
    },
    {
        "name": "logs-list",
        "capability": "logViewer",
        "capabilityEnabled": true
    },
    {
        "name": "logs-tail",
        "capability": "logViewer",
        "capabilityEnabled": true
    },
    {
        "name": "logs-info",
        "capability": "logViewer",
        "capabilityEnabled": true
    },
    {
        "name": "logs-unavailable",
        "capability": "logViewer",
        "capabilityEnabled": false
    },
    {
        "name": "ota-status",
        "capability": "ota",
        "capabilityEnabled": true
    },
    {
        "name": "ota-unavailable",
        "capability": "ota",
        "capabilityEnabled": false
    },
    {
        "name": "rule-scripts",
        "capability": "ruleScript",
        "capabilityEnabled": true
    },
    {
        "name": "rule-scripts-unavailable",
        "capability": "ruleScript",
        "capabilityEnabled": false
    },
    {
        "name": "users",
        "capability": "auth",
        "capabilityEnabled": true
    },
    {
        "name": "users-unavailable",
        "capability": "auth",
        "capabilityEnabled": false
    }
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

function validateCheck(check, index, issues, registered, names) {
    const label = `checks[${index}]`;
    const name = String(check && check.name || '');
    const method = String(check && check.method || '');
    const type = String(check && check.type || '');
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

    if (Object.prototype.hasOwnProperty.call(check, 'profiles')) {
        issues.push(`${label}: firmware editions are not supported; use capability conditions`);
    }
    if (Object.prototype.hasOwnProperty.call(check, 'capability')) {
        if (!CAPABILITIES.has(check.capability) || typeof check.capabilityEnabled !== 'boolean') {
            issues.push(`${label}: invalid capability condition`);
        }
    }

    const allowedStatuses = Array.isArray(check && check.allowedStatuses) ? check.allowedStatuses : [];
    const hasAllowedMessages = check && Object.prototype.hasOwnProperty.call(check, 'allowedMessages');
    const allowedMessages = Array.isArray(check && check.allowedMessages) ? check.allowedMessages : [];
    if (type === 'expect-unavailable' || type === 'expect-error' || type === 'capability-optional') {
        if (allowedStatuses.length === 0) {
            issues.push(`${label} (${name}): ${type} requires allowedStatuses`);
        }
        allowedStatuses.forEach((status) => {
            if (!Number.isInteger(status) || status < 400 || status > 599) {
                issues.push(`${label} (${name}): invalid allowed status '${status}'`);
            }
        });
    } else if (allowedStatuses.length > 0) {
        issues.push(`${label} (${name}): allowedStatuses is only valid for expect-* or capability-optional checks`);
    }
    if (hasAllowedMessages) {
        if (type !== 'capability-optional') {
            issues.push(`${label} (${name}): allowedMessages is only valid for capability-optional checks`);
        }
        if (!Array.isArray(check.allowedMessages) || allowedMessages.length === 0) {
            issues.push(`${label} (${name}): allowedMessages must be a non-empty array`);
        }
        allowedMessages.forEach((message) => {
            if (typeof message !== 'string' || message.trim() === '') {
                issues.push(`${label} (${name}): allowedMessages entries must be non-empty strings`);
            }
        });
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

function validateRequiredChecks(checksByName, issues) {
    REQUIRED_COMMON_CHECKS.forEach((name) => {
        const check = checksByName.get(name);
        if (!check || check.capability) issues.push(`matrix: missing unconditional check '${name}'`);
    });
    REQUIRED_CAPABILITY_CHECKS.forEach(({name, capability, capabilityEnabled}) => {
        const check = checksByName.get(name);
        if (!check || check.capability !== capability || check.capabilityEnabled !== capabilityEnabled) {
            issues.push(`matrix: '${name}' must follow ${capability}=${capabilityEnabled}`);
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

    if (!Number.isInteger(matrix.version) || matrix.version < 1) {
        issues.push('matrix: version must be a positive integer');
    }
    if (checks.length === 0) {
        issues.push('matrix: checks must be a non-empty array');
    }

    checks.forEach((check, index) => {
        validateCheck(check, index, issues, registered, names);
        if (check && check.name) checksByName.set(String(check.name), check);
    });
    validateRequiredChecks(checksByName, issues);
    const semanticCount = validateSemanticParity(checksByName, issues);

    if (issues.length > 0) {
        console.error('FastBee device API matrix validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }

    console.log(`FastBee device API matrix OK: checks=${checks.length}, semantics=${semanticCount}, capabilityConditions=${REQUIRED_CAPABILITY_CHECKS.length}`);
}

main();
