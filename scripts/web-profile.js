'use strict';

/**
 * Resolve the Web publish set from the firmware capabilities compiled into a
 * PlatformIO environment. Runtime features.json values are intentionally not
 * consulted: a compiled feature keeps its Web resources even when a user
 * disables it at runtime.
 */

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const PLATFORMIO_INI = path.join(ROOT_DIR, 'platformio.ini');
const RESOURCE_MANIFEST_NAME = 'web-resource-manifest.json';

const CAPABILITY_MACROS = Object.freeze({
    webServer: 'FASTBEE_ENABLE_WEB_SERVER',
    webStatic: 'FASTBEE_ENABLE_WEB_STATIC',
    webApi: 'FASTBEE_ENABLE_WEB_API',
    auth: 'FASTBEE_ENABLE_AUTH',
    session: 'FASTBEE_ENABLE_SESSION',
    mqtt: 'FASTBEE_ENABLE_MQTT',
    mdns: 'FASTBEE_ENABLE_MDNS',
    apMode: 'FASTBEE_ENABLE_AP_MODE',
    periphExec: 'FASTBEE_ENABLE_PERIPH_EXEC',
    gpio: 'FASTBEE_ENABLE_GPIO',
    commandScript: 'FASTBEE_ENABLE_COMMAND_SCRIPT',
    sensorDriver: 'FASTBEE_ENABLE_SENSOR_DRIVER',
    ds18b20: 'FASTBEE_ENABLE_DS18B20',
    modbus: 'FASTBEE_ENABLE_MODBUS',
    lcd: 'FASTBEE_ENABLE_LCD',
    lcdGb2312: 'FASTBEE_ENABLE_LCD_GB2312',
    neopixel: 'FASTBEE_ENABLE_NEOPIXEL',
    sevenSegment: 'FASTBEE_ENABLE_SEVEN_SEGMENT',
    rfid: 'FASTBEE_ENABLE_RFID',
    i2cSensors: 'FASTBEE_ENABLE_I2C_SENSORS',
    ioExpander: 'FASTBEE_ENABLE_IO_EXPANDER',
    irRemote: 'FASTBEE_ENABLE_IR_REMOTE',
    rfLearn: 'FASTBEE_ENABLE_RF_LEARN',
    ds1302: 'FASTBEE_ENABLE_DS1302',
    lcd1602: 'FASTBEE_ENABLE_LCD1602',
    ruleScript: 'FASTBEE_ENABLE_RULE_SCRIPT',
    deviceControl: 'FASTBEE_ENABLE_DEVICE_CONTROL',
    screenPanel: 'FASTBEE_ENABLE_SCREEN_PANEL',
    remoteResources: 'FASTBEE_ENABLE_REMOTE_RESOURCES',
    ota: 'FASTBEE_ENABLE_OTA',
    otaFs: 'FASTBEE_ENABLE_OTA_FS',
    logViewer: 'FASTBEE_ENABLE_LOG_VIEWER',
    fileLogging: 'FASTBEE_ENABLE_FILE_LOGGING',
    userAdmin: 'FASTBEE_ENABLE_USER_ADMIN',
    configTransfer: 'FASTBEE_ENABLE_CONFIG_TRANSFER',
    ble: 'FASTBEE_ENABLE_BLE',
    ethernet: 'FASTBEE_ENABLE_ETHERNET',
    cellular: 'FASTBEE_ENABLE_CELLULAR',
    i18n: 'FASTBEE_ENABLE_I18N'
});

// Independently loadable optional resources are listed here. Array rules mean
// "all capabilities" for backward compatibility; object rules can also express
// shared surfaces such as the protocol page, which exists for MQTT or Modbus.
const ASSET_CAPABILITY_REQUIREMENTS = Object.freeze({
    'js/modules/dashboard-fullscreen.js': ['deviceControl'],
    'js/modules/device-control.js': ['deviceControl'],
    'js/modules/device-control-view.js': ['deviceControl'],
    'js/modules/device-control-modbus.js': ['deviceControl', 'modbus'],
    'pages/fullscreen.html': ['deviceControl'],

    'js/modules/device-ota.js': ['ota'],
    'js/modules/device-config-transfer.js': ['configTransfer'],
    'pages/fragments/device-ota.html': ['ota'],

    'js/modules/protocol-modbus-rtu.js': ['modbus'],
    'js/modules/protocol-modbus-control.js': ['modbus'],
    'js/modules/protocol-full-config.js': ['modbus'],
    'pages/fragments/protocol-modbus-rtu.html': ['modbus'],

    'js/modules/protocol.js': { anyOf: ['mqtt', 'modbus'] },
    'pages/protocol.html': { anyOf: ['mqtt', 'modbus'] },
    'pages/fragments/protocol-mqtt.html': ['mqtt'],

    'js/modules/screen-panel-model.js': ['screenPanel'],
    'js/modules/screen-panel.js': ['screenPanel'],
    'pages/screen-panel.html': ['screenPanel'],
    'css/screen-panel.css': ['screenPanel'],

    'js/modules/periph-exec.js': ['periphExec'],
    'js/modules/periph-exec-form.js': ['periphExec'],
    'js/modules/periph-exec-templates.js': ['periphExec'],
    'js/modules/periph-exec-sensor.js': ['periphExec'],
    'js/modules/periph-exec-effects.js': ['periphExec'],
    'js/modules/periph-exec-modbus.js': { allOf: ['periphExec', 'modbus'] },

    'js/modules/rule-script.js': ['ruleScript'],
    'pages/rule-script.html': ['ruleScript'],

    'js/modules/logs.js': ['logViewer'],
    'pages/logs.html': ['logViewer'],

    'js/modules/users.js': ['userAdmin'],
    'pages/admin.html': ['userAdmin'],

    'js/modules/ir-remote.js': ['irRemote'],
    'js/modules/rf-remote.js': ['rfLearn']
});

// Every sidebar item must be classified here. Keeping the base pages in the
// same table makes a newly added menu fail tests until its compile rule is
// deliberately chosen, instead of silently shipping a link to a missing page.
const MENU_CAPABILITY_REQUIREMENTS = Object.freeze({
    dashboard: [],
    device: [],
    network: [],
    peripheral: [],
    'periph-exec': ['periphExec'],
    protocol: { anyOf: ['mqtt', 'modbus'] },
    'screen-panel': ['screenPanel'],
    'device-control': ['deviceControl'],
    'rule-script': ['ruleScript'],
    logs: ['logViewer'],
    users: ['userAdmin'],
    features: []
});

// Some source files intentionally contain several independently mounted page
// roots or modal templates. The build removes only the unavailable roots.
const PAGE_ROOT_CAPABILITY_REQUIREMENTS = Object.freeze({
    'periph-exec-page': ['periphExec'],
    'device-control-page': ['deviceControl']
});

const MODAL_CAPABILITY_REQUIREMENTS = Object.freeze({
    'periph-exec-modal': ['periphExec']
});

const PROTOCOL_TAB_CAPABILITY_REQUIREMENTS = Object.freeze({
    mqtt: ['mqtt'],
    'modbus-rtu': ['modbus'],
});

function stripIniComment(value) {
    return String(value || '').replace(/\s[;#].*$/, '').trim();
}

function parseIni(text) {
    const sections = new Map();
    let section = '';
    let activeKey = '';

    String(text || '').split(/\r?\n/).forEach((rawLine) => {
        const header = rawLine.trim().match(/^\[([^\]]+)]$/);
        if (header) {
            section = header[1].trim();
            activeKey = '';
            if (!sections.has(section)) sections.set(section, new Map());
            return;
        }
        if (!section || /^\s*[;#]/.test(rawLine) || !rawLine.trim()) return;

        // PlatformIO keys start at column zero. Indented -DMACRO=value lines
        // are list continuations and must not be mistaken for assignments.
        const assignment = rawLine.match(/^([A-Za-z0-9_.-]+)\s*=\s*(.*)$/);
        if (assignment) {
            activeKey = assignment[1];
            const values = [];
            const first = stripIniComment(assignment[2]);
            if (first) values.push(first);
            sections.get(section).set(activeKey, values);
            return;
        }
        if (!activeKey || !/^\s+/.test(rawLine)) return;
        const value = stripIniComment(rawLine);
        if (value) sections.get(section).get(activeKey).push(value);
    });
    return sections;
}

function splitIniTokens(lines) {
    const tokens = [];
    (lines || []).forEach((line) => {
        String(line).split(/[\s,]+/).filter(Boolean).forEach((token) => tokens.push(token));
    });
    return tokens;
}

function resolveIniList(sections, sectionName, key, stack = []) {
    const refKey = `${sectionName}.${key}`;
    if (stack.includes(refKey)) {
        throw new Error(`Circular platformio.ini reference: ${stack.concat(refKey).join(' -> ')}`);
    }
    const section = sections.get(sectionName);
    if (!section) throw new Error(`Unknown platformio.ini section: ${sectionName}`);
    const tokens = splitIniTokens(section.get(key));
    const resolved = [];
    tokens.forEach((token) => {
        const reference = token.match(/^\$\{([^}.]+)\.([^}]+)}$/);
        if (reference) {
            resolved.push(...resolveIniList(sections, reference[1], reference[2], stack.concat(refKey)));
        } else {
            resolved.push(token);
        }
    });
    return resolved;
}

function defaultEnvironmentFromSections(sections) {
    const platformio = sections.get('platformio');
    const values = platformio ? splitIniTokens(platformio.get('default_envs')) : [];
    return values[0] || '';
}

function resolveBuildFlagsFromIni(text, environment) {
    const sections = parseIni(text);
    const selected = environment || defaultEnvironmentFromSections(sections);
    if (!selected) throw new Error('platformio.ini has no default_envs');
    const sectionName = `env:${selected}`;
    if (!sections.has(sectionName)) throw new Error(`Unknown PlatformIO environment: ${selected}`);

    const flags = resolveIniList(sections, sectionName, 'build_flags');
    const unflags = new Set(resolveIniList(sections, sectionName, 'build_unflags'));
    return {
        environment: selected,
        buildFlags: flags.filter((flag) => !unflags.has(flag))
    };
}

function parseDefines(buildFlags) {
    const definitions = {};
    (buildFlags || []).forEach((rawFlag) => {
        const flag = String(rawFlag || '').trim();
        const define = flag.match(/^-D([^=\s]+)(?:=(.*))?$/);
        if (define) {
            definitions[define[1]] = define[2] === undefined ? '1' : define[2];
            return;
        }
        const undefine = flag.match(/^-U([^\s]+)$/);
        if (undefine) definitions[undefine[1]] = '0';
    });
    return definitions;
}

function defineEnabled(value) {
    if (value === undefined || value === null) return false;
    return !/^(?:0|false|off|no|disabled)?$/i.test(String(value).trim());
}

function capabilitiesFromBuildFlags(buildFlags) {
    const definitions = parseDefines(buildFlags);
    return Object.fromEntries(Object.entries(CAPABILITY_MACROS).map(([capability, macro]) => [
        capability,
        // The legacy OTA_FS flag describes partition/release compatibility.
        // There is no U_FS/U_SPIFFS writer; an asserted macro is not support.
        capability !== 'otaFs' && defineEnabled(definitions[macro])
    ]));
}

function readArgValue(argv, names) {
    for (let index = 0; index < argv.length; index += 1) {
        const argument = argv[index];
        for (const name of names) {
            if (argument === name && argv[index + 1]) return argv[index + 1];
            if (argument.startsWith(`${name}=`)) return argument.slice(name.length + 1);
        }
    }
    return '';
}

function readCapabilityInput(filePath) {
    const resolved = path.resolve(filePath);
    const value = JSON.parse(fs.readFileSync(resolved, 'utf8').replace(/^\uFEFF/, ''));
    if (!value || !Array.isArray(value.buildFlags) || !value.environment) {
        throw new Error(`Invalid Web capability input: ${resolved}`);
    }
    return {
        environment: String(value.environment),
        buildFlags: value.buildFlags.map(String),
        source: 'platformio-resolved-build-flags'
    };
}

function resolveWebBuildContext(options = {}) {
    const argv = options.argv || process.argv.slice(2);
    const inputPath = options.capabilityInput ||
        readArgValue(argv, ['--capability-input']) ||
        process.env.FASTBEE_WEB_CAPABILITY_INPUT || '';
    const requestedEnvironment = options.environment ||
        readArgValue(argv, ['--env', '--environment', '-e']) ||
        process.env.PIOENV || '';
    let resolved;

    if (Array.isArray(options.buildFlags)) {
        resolved = {
            environment: options.environment || 'test',
            buildFlags: options.buildFlags.map(String),
            source: options.source || 'provided-build-flags'
        };
    } else if (inputPath) {
        resolved = readCapabilityInput(inputPath);
        if (requestedEnvironment && requestedEnvironment !== resolved.environment) {
            throw new Error(
                `Web capability environment mismatch: requested ${requestedEnvironment}, input has ${resolved.environment}`
            );
        }
    } else {
        const iniText = options.platformioText === undefined
            ? fs.readFileSync(options.platformioPath || PLATFORMIO_INI, 'utf8')
            : options.platformioText;
        resolved = {
            ...resolveBuildFlagsFromIni(iniText, requestedEnvironment),
            source: 'platformio.ini'
        };
    }

    const definitions = parseDefines(resolved.buildFlags);
    const compiledCapabilities = capabilitiesFromBuildFlags(resolved.buildFlags);
    const fingerprintInput = JSON.stringify({
        environment: resolved.environment,
        definitions: Object.fromEntries(Object.entries(definitions).sort(([left], [right]) => left.localeCompare(right)))
    });
    return Object.freeze({
        environment: resolved.environment,
        source: resolved.source,
        buildFlags: Object.freeze(resolved.buildFlags.slice()),
        definitions: Object.freeze(definitions),
        compiledCapabilities: Object.freeze(compiledCapabilities),
        capabilityFingerprint: crypto.createHash('sha256').update(fingerprintInput).digest('hex')
    });
}

function hasCapability(name, context = resolveWebBuildContext()) {
    return context.compiledCapabilities[name] === true;
}

function capabilityRuleSatisfied(rule, context = resolveWebBuildContext()) {
    const normalized = Array.isArray(rule) ? { allOf: rule } : (rule || {});
    const allOf = Array.isArray(normalized.allOf) ? normalized.allOf : [];
    const anyOf = Array.isArray(normalized.anyOf) ? normalized.anyOf : [];
    return allOf.every((name) => hasCapability(name, context)) &&
        (anyOf.length === 0 || anyOf.some((name) => hasCapability(name, context)));
}

function shouldIncludeAsset(relPath, context = resolveWebBuildContext()) {
    const normalized = String(relPath || '').replace(/\\/g, '/').replace(/^\.\//, '');
    if (normalized === 'pages/fragments/protocol-remote-resources.html' || normalized === 'js/modules/protocol/remote-resources.js') return false;
    const requirements = ASSET_CAPABILITY_REQUIREMENTS[normalized] || [];
    return capabilityRuleSatisfied(requirements, context);
}

function shouldIncludeMenu(pageName, context = resolveWebBuildContext()) {
    if (!Object.prototype.hasOwnProperty.call(MENU_CAPABILITY_REQUIREMENTS, pageName)) return false;
    return capabilityRuleSatisfied(MENU_CAPABILITY_REQUIREMENTS[pageName], context);
}

function hasDeviceControl(context = resolveWebBuildContext()) {
    return hasCapability('deviceControl', context);
}

function resolveRequestGovernorLimits(context = resolveWebBuildContext()) {
    const definitions = context && context.definitions ? context.definitions : {};
    const psramEnabled = defineEnabled(definitions.FASTBEE_USE_PSRAM);
    const maxConcurrent = psramEnabled ? 2 : 1;
    return Object.freeze({
        maxConcurrent,
        maxWeight: maxConcurrent,
        maxHeavy: 1,
        memoryProfile: psramEnabled ? 'psram' : 'internal-dram'
    });
}

// Compatibility helpers retained for scripts that still print a profile label.
// The selection itself is driven by compiled hardware capabilities.
function getWebProfile(context = resolveWebBuildContext()) {
    return `compiled:${context.environment}`;
}

function walkFiles(directory, files = []) {
    if (!fs.existsSync(directory)) return files;
    fs.readdirSync(directory, { withFileTypes: true }).forEach((entry) => {
        const filePath = path.join(directory, entry.name);
        if (entry.isDirectory()) walkFiles(filePath, files);
        else if (entry.isFile()) files.push(filePath);
    });
    return files;
}

function writeWebResourceManifest(wwwDir, context = resolveWebBuildContext()) {
    const manifestPath = path.join(wwwDir, RESOURCE_MANIFEST_NAME);
    const resources = walkFiles(wwwDir)
        .filter((filePath) => filePath !== manifestPath && !filePath.endsWith('.gz'))
        .map((filePath) => path.relative(wwwDir, filePath).replace(/\\/g, '/'))
        .filter((relPath) => shouldIncludeAsset(relPath, context))
        .sort();
    const manifest = {
        schemaVersion: 1,
        environment: context.environment,
        capabilitySource: context.source,
        capabilityFingerprint: context.capabilityFingerprint,
        runtimeConfigurationAffectsResources: false,
        compiledCapabilities: context.compiledCapabilities,
        requestGovernor: resolveRequestGovernorLimits(context),
        resources
    };
    // This JSON ships uncompressed on every device. Omit formatting bytes;
    // fields, values and resource order remain identical for release checks.
    fs.writeFileSync(manifestPath, `${JSON.stringify(manifest)}\n`, 'utf8');
    return { path: manifestPath, manifest };
}

function readWebResourceManifest(wwwDir) {
    const manifestPath = path.join(wwwDir, RESOURCE_MANIFEST_NAME);
    return JSON.parse(fs.readFileSync(manifestPath, 'utf8').replace(/^\uFEFF/, ''));
}

module.exports = {
    ASSET_CAPABILITY_REQUIREMENTS,
    CAPABILITY_MACROS,
    MENU_CAPABILITY_REQUIREMENTS,
    MODAL_CAPABILITY_REQUIREMENTS,
    PAGE_ROOT_CAPABILITY_REQUIREMENTS,
    PLATFORMIO_INI,
    PROTOCOL_TAB_CAPABILITY_REQUIREMENTS,
    RESOURCE_MANIFEST_NAME,
    capabilityRuleSatisfied,
    capabilitiesFromBuildFlags,
    getWebProfile,
    hasCapability,
    hasDeviceControl,
    parseDefines,
    parseIni,
    readWebResourceManifest,
    resolveBuildFlagsFromIni,
    resolveRequestGovernorLimits,
    resolveWebBuildContext,
    shouldIncludeAsset,
    shouldIncludeMenu,
    writeWebResourceManifest
};
