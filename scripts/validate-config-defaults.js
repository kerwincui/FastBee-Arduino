/*
 * Validate default peripheral and periph-exec configuration.
 *
 * The factory config is intentionally conservative: peripheral templates may
 * exist, but execution rules must not auto-run until the user verifies wiring.
 */

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const DATA_CONFIG_DIR = path.join(ROOT_DIR, 'data', 'config');
const PROFILES = ['lite', 'standard', 'full'];
const COMPACT_TEMPLATE_PERIPHERALS = new Set(['stepper', 'adc', 'ws2812b', 'uart_debug']);
const DISABLED_RULE_TEMPLATE_PREFIXES = [
    'exec_stepper_',
    'exec_adc_voltage_',
    'exec_ws2812b_',
    'exec_uart_debug_'
];
const NON_PERIPHERAL_TARGET_ACTIONS = new Set([
    6,  // system restart
    7,  // factory reset
    8,  // NTP sync
    9,  // OTA
    15, // command script
    18, // Modbus poll
    21, // trigger event
    22, // enable rule
    23  // disable rule
]);

function readJson(filePath) {
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
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

function shouldKeepProfilePeripheral(item, profile) {
    const id = String((item && item.id) || '');
    if (id === 'modbus_tcp') return false;
    if (profile === 'lite' && (id === 'modbus_rtu' || id === 'ethernet')) return false;
    if (COMPACT_TEMPLATE_PERIPHERALS.has(id)) return true;
    return item && item.enabled !== false;
}

function getProfilePeripheralIds(peripheralsDoc, profile) {
    const peripherals = getPeripherals(peripheralsDoc);
    const selected = profile === 'full'
        ? peripherals
        : peripherals.filter((item) => shouldKeepProfilePeripheral(item, profile));
    return new Set(selected.map((item) => String(item.id || '')).filter(Boolean));
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

function transformRulesForProfile(execDoc, profile, profilePeripheralIds) {
    const rules = getRules(execDoc);
    if (profile === 'full') return rules;

    const unsupportedActions = new Set([9, 20]);
    const unsupportedProtocols = new Set([2, 3, 4, 5]);
    if (profile === 'lite') {
        [16, 17, 18].forEach((actionType) => unsupportedActions.add(actionType));
        unsupportedProtocols.add(1);
    }

    return rules
        .map((rule) => {
            if (rule && rule.enabled === false && !isDisabledRuleTemplate(rule)) return null;
            if (unsupportedProtocols.has(Number(rule && rule.protocolType))) return null;

            const actions = Array.isArray(rule && rule.actions)
                ? rule.actions.filter((action) => {
                    if (unsupportedActions.has(Number(action.actionType))) return false;
                    if (profile === 'lite' && String(action.targetPeriphId || '').indexOf('modbus') === 0) return false;
                    if (Number(action.actionType) === 15) {
                        const refs = getScriptPeripheralRefs(action.actionValue);
                        if (refs.some((id) => !profilePeripheralIds.has(id))) return false;
                    }
                    if (actionTargetRequiresPeripheral(action) &&
                        !profilePeripheralIds.has(String(action.targetPeriphId || ''))) {
                        return false;
                    }
                    return true;
                })
                : [];
            if (actions.length === 0) return null;
            return { ...rule, actions };
        })
        .filter(Boolean);
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

function validateStagingRoot(stageRoot, latestOnly, issues) {
    const stageDirs = getStageDirs(stageRoot, latestOnly);
    const summaries = [];
    stageDirs.forEach((item) => {
        const peripheralsDoc = readJson(path.join(item.fullPath, 'config', 'peripherals.json'));
        const execDoc = readJson(path.join(item.fullPath, 'config', 'periph_exec.json'));
        const peripheralIds = new Set(getPeripherals(peripheralsDoc).map((periph) => String(periph.id || '')).filter(Boolean));
        const rules = getRules(execDoc);
        rules.forEach((rule) => validateRuleReferences(rule, peripheralIds, `staging:${item.name}`, issues));
        summaries.push({
            name: item.name,
            peripherals: peripheralIds.size,
            rules: rules.length,
            enabledRules: rules.filter((rule) => rule.enabled !== false).length
        });
    });
    return summaries;
}

function main() {
    const peripheralsDoc = readJson(path.join(DATA_CONFIG_DIR, 'peripherals.json'));
    const execDoc = readJson(path.join(DATA_CONFIG_DIR, 'periph_exec.json'));
    const rules = getRules(execDoc);
    const issues = [];
    const stageRootArg = readArgValue('--staging-root');
    const latestOnly = process.argv.includes('--latest-only');

    rules.forEach((rule) => {
        if (rule && rule.enabled !== false) {
            issues.push(`source rule '${rule.id || '(missing-id)'}' must default to enabled=false`);
        }
    });

    const sourcePeripheralIds = getProfilePeripheralIds(peripheralsDoc, 'full');
    rules.filter((rule) => rule && rule.enabled === true)
        .forEach((rule) => validateRuleReferences(rule, sourcePeripheralIds, 'source', issues));

    const profileSummaries = PROFILES.map((profile) => {
        const profilePeripheralIds = getProfilePeripheralIds(peripheralsDoc, profile);
        const profileRules = transformRulesForProfile(execDoc, profile, profilePeripheralIds);
        profileRules.forEach((rule) => validateRuleReferences(rule, profilePeripheralIds, `profile:${profile}`, issues));
        return {
            profile,
            peripherals: profilePeripheralIds.size,
            rules: profileRules.length,
            enabledRules: profileRules.filter((rule) => rule.enabled !== false).length
        };
    });
    const stagingSummaries = stageRootArg
        ? validateStagingRoot(path.resolve(ROOT_DIR, stageRootArg), latestOnly, issues)
        : [];

    if (issues.length > 0) {
        console.error('FastBee config default validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }

    const enabledCount = rules.filter((rule) => rule && rule.enabled !== false).length;
    console.log(`FastBee config defaults OK: source rules=${rules.length}, enabled=${enabledCount}, peripherals=${sourcePeripheralIds.size}`);
    profileSummaries.forEach((item) => {
        console.log(`  ${item.profile}: peripherals=${item.peripherals}, rules=${item.rules}, enabled=${item.enabledRules}`);
    });
    if (stageRootArg) {
        console.log(`FastBee staging configs OK: checked=${stagingSummaries.length}, latestOnly=${latestOnly}`);
        stagingSummaries.forEach((item) => {
            console.log(`  ${item.name}: peripherals=${item.peripherals}, rules=${item.rules}, enabled=${item.enabledRules}`);
        });
    }
}

main();
