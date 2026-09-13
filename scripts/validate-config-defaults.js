/*
 * Validate default peripheral and periph-exec configuration.
 *
 * The factory config is intentionally conservative: peripheral templates may
 * exist, but execution rules must not auto-run until the user verifies wiring.
 */

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const ROOT_DIR = path.resolve(__dirname, '..');
const DATA_CONFIG_DIR = path.join(ROOT_DIR, 'data', 'config');
const SYSTEM_CONSTANTS_FILE = path.join(ROOT_DIR, 'include', 'core', 'SystemConstants.h');
const WIFI_MANAGER_HEADER = path.join(ROOT_DIR, 'include', 'network', 'WiFiManager.h');
const SYSTEM_ROUTE_FILE = path.join(ROOT_DIR, 'src', 'network', 'handlers', 'SystemRouteHandler.cpp');
const README_FILE = path.join(ROOT_DIR, 'README.md');
const README_EN_FILE = path.join(ROOT_DIR, 'README.en.md');
const PROFILES = ['lite', 'standard', 'full', 'unified'];
const DISABLED_RULE_TEMPLATE_PREFIXES = [
    'exec_stepper_',
    'exec_adc_voltage_',
    'exec_ws2812b_',
    'exec_uart_debug_'
];
const NON_PERIPHERAL_TARGET_ACTIONS = new Set([
    6, 7, 8, 9, 15, 18, 21, 22, 23
]);
const PROFILE_DEFAULT_BUDGETS = {
    maxPeripherals: 32,
    maxRules: 32,
    maxEnabledRules: 0
};
const MAX_TRIGGERS_PER_RULE = 3;
const MAX_ACTIONS_PER_RULE = 4;

function readJson(filePath) {
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function validateDefaultApPassword(issues) {
    const constantsText = readText(SYSTEM_CONSTANTS_FILE);
    const constantMatch = constantsText.match(/DEFAULT_AP_PASSWORD\s*=\s*"([^"]+)"/);
    if (!constantMatch) {
        issues.push('SystemConstants.h: DEFAULT_AP_PASSWORD is missing');
        return;
    }

    const password = constantMatch[1];
    if (password.length < 8 || password.length > 63) {
        issues.push('SystemConstants.h: DEFAULT_AP_PASSWORD must be a valid WPA2 password (8-63 characters)');
    }

    const networkDoc = readJson(path.join(DATA_CONFIG_DIR, 'network.json'));
    if (networkDoc.apPassword !== password) {
        issues.push('data/config/network.json: apPassword must match NetConst::DEFAULT_AP_PASSWORD');
    }

    const wifiHeader = readText(WIFI_MANAGER_HEADER);
    if (!wifiHeader.includes('String apPassword = NetConst::DEFAULT_AP_PASSWORD')) {
        issues.push('WiFiManager.h: missing-config fallback must use NetConst::DEFAULT_AP_PASSWORD');
    }

    const resetRoute = readText(SYSTEM_ROUTE_FILE);
    if (!resetRoute.includes(`\\"apPassword\\":\\"${password}\\"`)) {
        issues.push('SystemRouteHandler.cpp: factory-reset AP password must match NetConst::DEFAULT_AP_PASSWORD');
    }

    if (!readText(README_FILE).includes(`密码均为 \`${password}\``)) {
        issues.push('README.md: documented AP password must match NetConst::DEFAULT_AP_PASSWORD');
    }
    if (!readText(README_EN_FILE).includes(`default AP password is \`${password}\``)) {
        issues.push('README.en.md: documented AP password must match NetConst::DEFAULT_AP_PASSWORD');
    }
}

function validateDefaultAdmin(issues) {
    const constantsText = readText(SYSTEM_CONSTANTS_FILE);
    const usernameMatch = constantsText.match(/DEFAULT_ADMIN_USERNAME\s*=\s*"([^"]+)"/);
    const passwordMatch = constantsText.match(/DEFAULT_ADMIN_PASSWORD\s*=\s*"([^"]+)"/);
    if (!usernameMatch || !passwordMatch) {
        issues.push('SystemConstants.h: default administrator credentials are missing');
        return;
    }

    const username = usernameMatch[1];
    const password = passwordMatch[1];
    const usersDoc = readJson(path.join(DATA_CONFIG_DIR, 'users.json'));
    const users = Array.isArray(usersDoc.users) ? usersDoc.users : [];
    if (users.length !== 1 || users[0].username !== username || users[0].enabled !== true) {
        issues.push('data/config/users.json: factory image must contain only the enabled default administrator');
        return;
    }

    const salt = String(users[0].salt || '');
    const passwordHash = String(users[0].passwordHash || '');
    const expectedHash = crypto.createHash('sha256')
        .update(`${salt}${password}${salt}`, 'utf8')
        .digest('base64');
    if (!salt || passwordHash !== expectedHash) {
        issues.push('data/config/users.json: default administrator hash does not match SystemConstants.h');
    }

    const resetRoute = readText(SYSTEM_ROUTE_FILE);
    for (const [field, value] of [['username', username], ['passwordHash', passwordHash], ['salt', salt]]) {
        if (!value || !resetRoute.includes(`\\"${field}\\":\\"${value}\\"`)) {
            issues.push(`SystemRouteHandler.cpp: factory-reset ${field} must match data/config/users.json`);
        }
    }

    if (!readText(README_FILE).includes(`\`${username} / ${password}\``)) {
        issues.push('README.md: documented default login must match SystemConstants.h');
    }
    if (!readText(README_EN_FILE).includes(`\`${username} / ${password}\``)) {
        issues.push('README.en.md: documented default login must match SystemConstants.h');
    }
}

function validateFeatureDefaults(issues) {
    const featuresDoc = readJson(path.join(DATA_CONFIG_DIR, 'features.json'));
    const features = featuresDoc && featuresDoc.features;
    if (featuresDoc.version !== 1) {
        issues.push('data/config/features.json: version must be 1');
    }
    if (!features || typeof features !== 'object' || Array.isArray(features)) {
        issues.push('data/config/features.json: features must be an object');
        return;
    }

    const requiredEnabled = new Set(['webServer', 'mdns']);
    for (const key of requiredEnabled) {
        if (!features[key] || features[key].enabled !== true) {
            issues.push(`data/config/features.json: ${key} must default to enabled=true`);
        }
    }

    Object.entries(features).forEach(([key, entry]) => {
        if (!entry || typeof entry !== 'object' || typeof entry.enabled !== 'boolean') {
            issues.push(`data/config/features.json: ${key}.enabled must be boolean`);
            return;
        }
        if (!requiredEnabled.has(key) && entry.enabled) {
            issues.push(`data/config/features.json: optional feature ${key} must default to disabled`);
        }
    });

    const networkDoc = readJson(path.join(DATA_CONFIG_DIR, 'network.json'));
    if (networkDoc.enableMDNS !== features.mdns?.enabled) {
        issues.push('data/config/network.json: enableMDNS must match features.json mdns default');
    }
}

function readArgValue(prefix) {
    const direct = process.argv.find((arg) => arg.startsWith(`${prefix}=`));
    if (direct) return direct.slice(prefix.length + 1);
    const index = process.argv.indexOf(prefix);
    if (index >= 0 && index + 1 < process.argv.length) return process.argv[index + 1];
    return '';
}

function getRules(doc) {
    return Array.isArray(doc && doc.rules) ? doc.rules : [];
}

function getPeripherals(doc) {
    return Array.isArray(doc && doc.peripherals) ? doc.peripherals : [];
}

function getRuleActions(rule) {
    return Array.isArray(rule && rule.actions) ? rule.actions : [];
}

function getRuleTriggers(rule) {
    return Array.isArray(rule && rule.triggers) ? rule.triggers : [];
}

function getProfilePeripheralIds(peripheralsDoc) {
    return new Set(getPeripherals(peripheralsDoc)
        .map((item) => String(item && item.id || ''))
        .filter(Boolean));
}

function isDisabledRuleTemplate(rule) {
    const id = String((rule && rule.id) || '');
    return DISABLED_RULE_TEMPLATE_PREFIXES.some((prefix) => id.startsWith(prefix));
}

function actionTargetRequiresPeripheral(action) {
    if (!action || !action.targetPeriphId) return false;
    const targetId = String(action.targetPeriphId || '');
    if (!targetId || targetId.startsWith('modbus-task:') || targetId.startsWith('modbus:')) return false;
    return !NON_PERIPHERAL_TARGET_ACTIONS.has(Number(action.actionType));
}

function getScriptPeripheralRefs(actionValue) {
    const refs = [];
    String(actionValue || '').split(/\r?\n/).forEach((line) => {
        const match = line.match(/^\s*PERIPH\s+([^\s#]+)/i);
        if (match && match[1]) refs.push(match[1]);
    });
    return refs;
}

function getJsonActionPeripheralRef(action) {
    if (Number(action && action.actionType) !== 19) return '';
    try {
        const payload = JSON.parse(String(action.actionValue || '{}'));
        return String(payload.periphId || '');
    } catch (_) {
        return '';
    }
}

function getTemplatePeripheralRefs(actionValue) {
    const refs = [];
    const text = String(actionValue || '');
    const pattern = /\$\{([A-Za-z0-9_-]+)\.[^}]+}/g;
    let match = pattern.exec(text);
    while (match) {
        if (match[1]) refs.push(match[1]);
        match = pattern.exec(text);
    }
    return refs;
}

function validateRuleReferences(rule, peripheralIds, scope, issues) {
    const actions = Array.isArray(rule && rule.actions) ? rule.actions : [];
    actions.forEach((action, actionIndex) => {
        const actionLabel = `${scope} rule=${rule.id || '(missing-id)'} action=${actionIndex}`;
        if (actionTargetRequiresPeripheral(action)) {
            const targetId = String(action.targetPeriphId || '');
            if (!peripheralIds.has(targetId)) {
                issues.push(`${actionLabel}: targetPeriphId '${targetId}' is not present in peripherals.json`);
            }
        }

        if (Number(action.actionType) === 15) {
            getScriptPeripheralRefs(action.actionValue).forEach((id) => {
                if (!peripheralIds.has(id)) {
                    issues.push(`${actionLabel}: script PERIPH '${id}' is not present in peripherals.json`);
                }
            });
        }

        const sensorId = getJsonActionPeripheralRef(action);
        if (sensorId && !peripheralIds.has(sensorId)) {
            issues.push(`${actionLabel}: sensor action periphId '${sensorId}' is not present in peripherals.json`);
        }

        getTemplatePeripheralRefs(action.actionValue).forEach((id) => {
            if (!peripheralIds.has(id)) {
                issues.push(`${actionLabel}: template reference '${id}' is not present in peripherals.json`);
            }
        });
    });
}

function validateProfileFootprint(profile, peripheralIds, profileRules, scope, issues) {
    const budget = PROFILE_DEFAULT_BUDGETS[profile];
    if (!budget) return;

    if (peripheralIds.size > budget.maxPeripherals) {
        issues.push(`${scope}: ${profile} default peripherals ${peripheralIds.size} exceeds budget ${budget.maxPeripherals}`);
    }
    if (profileRules.length > budget.maxRules) {
        issues.push(`${scope}: ${profile} default rules ${profileRules.length} exceeds budget ${budget.maxRules}`);
    }

    const enabledRules = profileRules.filter((rule) => rule && rule.enabled !== false);
    if (enabledRules.length > budget.maxEnabledRules) {
        issues.push(`${scope}: ${profile} default enabled rules ${enabledRules.length} exceeds budget ${budget.maxEnabledRules}`);
    }

    profileRules.forEach((rule) => {
        const ruleLabel = `${scope} ${profile} rule '${rule && rule.id ? rule.id : '(missing-id)'}'`;
        const triggers = getRuleTriggers(rule);
        const actions = getRuleActions(rule);
        if (triggers.length > MAX_TRIGGERS_PER_RULE) {
            issues.push(`${ruleLabel}: triggers ${triggers.length} exceeds ${MAX_TRIGGERS_PER_RULE}`);
        }
        if (actions.length > MAX_ACTIONS_PER_RULE) {
            issues.push(`${ruleLabel}: actions ${actions.length} exceeds ${MAX_ACTIONS_PER_RULE}`);
        }
        if (budget.unsupportedProtocols.has(Number(rule && rule.protocolType))) {
            issues.push(`${ruleLabel}: protocolType ${rule.protocolType} is not allowed in ${profile} defaults`);
        }
        actions.forEach((action, actionIndex) => {
            const actionType = Number(action && action.actionType);
            if (budget.unsupportedActionTypes.has(actionType)) {
                issues.push(`${ruleLabel} action=${actionIndex}: actionType ${actionType} is not allowed in ${profile} defaults`);
            }
        });
    });
}

function parseStageDirName(dirName) {
    const parts = String(dirName || '').split('-');
    const profile = parts[0] || '';
    if (!PROFILES.includes(profile) || parts.length < 4) return null;
    const env = parts.slice(1, -2).join('-');
    return env ? { profile, env } : null;
}

function getStageDirs(stageRoot, latestOnly) {
    if (!stageRoot || !fs.existsSync(stageRoot)) return [];
    const dirs = fs.readdirSync(stageRoot)
        .map((name) => {
            const fullPath = path.join(stageRoot, name);
            if (!fs.statSync(fullPath).isDirectory()) return null;
            const parsed = parseStageDirName(name);
            if (!parsed) return null;
            const peripheralsPath = path.join(fullPath, 'config', 'peripherals.json');
            const execPath = path.join(fullPath, 'config', 'periph_exec.json');
            if (!fs.existsSync(peripheralsPath) || !fs.existsSync(execPath)) return null;
            return {
                ...parsed,
                name,
                fullPath,
                mtimeMs: fs.statSync(fullPath).mtimeMs
            };
        })
        .filter(Boolean);

    if (!latestOnly) return dirs;

    const latestByTarget = new Map();
    dirs.forEach((item) => {
        const key = `${item.profile}:${item.env}`;
        const prev = latestByTarget.get(key);
        if (!prev || item.mtimeMs > prev.mtimeMs) latestByTarget.set(key, item);
    });
    return Array.from(latestByTarget.values()).sort((a, b) => a.name.localeCompare(b.name));
}

function main() {
    const peripheralsDoc = readJson(path.join(DATA_CONFIG_DIR, 'peripherals.json'));
    const execDoc = readJson(path.join(DATA_CONFIG_DIR, 'periph_exec.json'));
    const rules = getRules(execDoc);

    const issues = [];
    validateDefaultApPassword(issues);
    validateDefaultAdmin(issues);
    validateFeatureDefaults(issues);
    const stageRootArg = null; // staging validation disabled - only single version now
    const latestOnly = false;

    rules.forEach((rule) => {
        if (rule && rule.enabled !== false) {
            issues.push(`source rule '${rule.id || '(missing-id)'}' must default to enabled=false`);
        }
    });

    const sourcePeripheralIds = getProfilePeripheralIds(peripheralsDoc);
    rules.filter((rule) => rule && rule.enabled === true)
        .forEach((rule) => validateRuleReferences(rule, sourcePeripheralIds, 'source', issues));

    const profileSummaries = [];
    const stagingSummaries = stageRootArg
        ? validateStagingRoot(path.resolve(ROOT_DIR, stageRootArg), latestOnly, issues)
        : [];

    if (issues.length > 0) {
        console.error('FastBee config default validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }

    const enabledCount = rules.filter((rule) => rule && rule.enabled !== false).length;
    console.log(`FastBee config defaults OK: rules=${rules.length}, enabled=${enabledCount}, peripherals=${sourcePeripheralIds.size}`);
}

main();
