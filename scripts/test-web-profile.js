'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {
    ASSET_CAPABILITY_REQUIREMENTS,
    MENU_CAPABILITY_REQUIREMENTS,
    capabilityRuleSatisfied,
    capabilitiesFromBuildFlags,
    parseDefines,
    resolveBuildFlagsFromIni,
    resolveRequestGovernorLimits,
    resolveWebBuildContext,
    shouldIncludeAsset,
    shouldIncludeMenu
} = require('./web-profile');

const ROOT_DIR = path.resolve(__dirname, '..');
const PLATFORMIO_TEXT = fs.readFileSync(path.join(ROOT_DIR, 'platformio.ini'), 'utf8');
const FIRMWARE_ENVIRONMENTS = [
    'esp32-F4R0',
    'esp32-F8R0',
    'esp32-F8R8',
    'esp32c3-F4R0',
    'esp32c6-F4R0',
    'esp32c6-F8R0',
    'esp32s3-F8R0',
    'esp32s3-F8R8',
    'esp32s3-F16R8'
];

const sampleIni = `
[platformio]
default_envs = target

[base]
build_flags =
    -DFASTBEE_ENABLE_WEB_STATIC=1
    -DFASTBEE_ENABLE_MODBUS=1
    -DFASTBEE_ENABLE_OTA=1

[env:target]
build_flags =
    \${base.build_flags}
    -DFASTBEE_ENABLE_OTA=0
build_unflags =
    -DFASTBEE_ENABLE_MODBUS=1
`;

const resolved = resolveBuildFlagsFromIni(sampleIni, 'target');
assert.strictEqual(resolved.environment, 'target');
assert.deepStrictEqual(resolved.buildFlags, [
    '-DFASTBEE_ENABLE_WEB_STATIC=1',
    '-DFASTBEE_ENABLE_OTA=1',
    '-DFASTBEE_ENABLE_OTA=0'
]);
const sampleCapabilities = capabilitiesFromBuildFlags(resolved.buildFlags);
assert.strictEqual(sampleCapabilities.webStatic, true);
assert.strictEqual(sampleCapabilities.modbus, false);
assert.strictEqual(sampleCapabilities.ota, false, 'the final macro definition must win');

assert.throws(() => resolveBuildFlagsFromIni(`
[platformio]
default_envs = loop
[loop]
build_flags = \${loop.build_flags}
[env:loop]
build_flags = \${loop.build_flags}
`, 'loop'), /Circular platformio\.ini reference/);

const contexts = Object.fromEntries(FIRMWARE_ENVIRONMENTS.map((environment) => [
    environment,
    resolveWebBuildContext({ platformioText: PLATFORMIO_TEXT, environment, argv: [] })
]));
FIRMWARE_ENVIRONMENTS.forEach((environment) => {
    const context = contexts[environment];
    assert.strictEqual(context.environment, environment);
    assert.strictEqual(context.compiledCapabilities.webServer, true);
    assert.strictEqual(context.compiledCapabilities.webStatic, true);
    assert.strictEqual(context.compiledCapabilities.webApi, true);
    assert.strictEqual(context.compiledCapabilities.deviceControl, true);
    Object.entries(ASSET_CAPABILITY_REQUIREMENTS).forEach(([asset, rule]) => {
        assert.strictEqual(
            shouldIncludeAsset(asset, context),
            capabilityRuleSatisfied(rule, context),
            `${environment}: ${asset} capability decision is inconsistent`
        );
    });
});

const PSRAM_ENVIRONMENTS = new Set(['esp32-F8R8', 'esp32s3-F8R8', 'esp32s3-F16R8']);
FIRMWARE_ENVIRONMENTS.forEach((environment) => {
    const expectedConcurrency = PSRAM_ENVIRONMENTS.has(environment) ? 2 : 1;
    const limits = resolveRequestGovernorLimits(contexts[environment]);
    assert.deepStrictEqual(
        { maxConcurrent: limits.maxConcurrent, maxWeight: limits.maxWeight, maxHeavy: limits.maxHeavy },
        { maxConcurrent: expectedConcurrency, maxWeight: expectedConcurrency, maxHeavy: 1 },
        `${environment}: request governor memory profile`
    );
});

// The measured classic ESP32, C3 and C6 4 MB app slots cannot contain the optional
// graphical panel and remote-resource packages. The C3 additionally omits recent
// point history because it is otherwise the only code keeping the shared screen
// route handler linked. Keep these exceptions local to the measured targets;
// missing -D flags are unsafe because FeatureFlags.h defaults them to 1.
const CONSTRAINED_4MB_ENVIRONMENTS = new Set(['esp32-F4R0', 'esp32c3-F4R0', 'esp32c6-F4R0']);
const FOUR_MB_OPTIONAL_MACROS = [
    'FASTBEE_ENABLE_SCREEN_PANEL',
    'FASTBEE_ENABLE_SCREEN_UI',
    'FASTBEE_ENABLE_REMOTE_RESOURCES'
];
function assertFourMbCompileBoundary(iniText) {
    FIRMWARE_ENVIRONMENTS.forEach((environment) => {
        const resolvedFlags = resolveBuildFlagsFromIni(iniText, environment).buildFlags;
        const definitions = parseDefines(resolvedFlags);
        const constrained = CONSTRAINED_4MB_ENVIRONMENTS.has(environment);
        FOUR_MB_OPTIONAL_MACROS.forEach((macro) => {
            const matching = resolvedFlags.filter((flag) => flag === `-D${macro}` || flag.startsWith(`-D${macro}=`));
            const enabled = macro === 'FASTBEE_ENABLE_REMOTE_RESOURCES'
                ? !constrained : environment.startsWith('esp32s3-');
            assert.deepStrictEqual(matching, [`-D${macro}=${enabled ? 1 : 0}`],
                `${environment}: ${macro} must have one explicit definition after build_unflags`);
        });
        const pointHistoryEnabled = environment !== 'esp32c3-F4R0';
        const pointHistoryMacro = 'FASTBEE_ENABLE_POINT_HISTORY';
        const pointHistoryFlags = resolvedFlags.filter((flag) =>
            flag === `-D${pointHistoryMacro}` || flag.startsWith(`-D${pointHistoryMacro}=`));
        assert.deepStrictEqual(pointHistoryFlags, [
            `-D${pointHistoryMacro}=${pointHistoryEnabled ? 1 : 0}`
        ], `${environment}: ${pointHistoryMacro} must have one explicit definition after build_unflags`);
        [
            'I2C_SENSORS', 'IO_EXPANDER', 'SENSOR_DRIVER',
            'DS18B20', 'GPIO', 'PERIPH_EXEC', 'MODBUS', 'LCD', 'LCD1602',
            'MQTT', 'MDNS', 'AP_MODE', 'WEB_SERVER', 'WEB_STATIC', 'WEB_API', 'AUTH', 'SESSION'
        ].forEach((name) => {
            assert.strictEqual(definitions[`FASTBEE_ENABLE_${name}`], '1',
                `${environment}: preserve FASTBEE_ENABLE_${name} when trimming optional packages`);
        });
        const context = { compiledCapabilities: capabilitiesFromBuildFlags(resolvedFlags) };
        const screenSupported = environment.startsWith('esp32s3-');
        assert.strictEqual(context.compiledCapabilities.screenPanel, screenSupported, `${environment}: screenPanel`);
        assert.strictEqual(context.compiledCapabilities.remoteResources, !constrained, `${environment}: remoteResources`);
        [
            'pages/screen-panel.html', 'css/screen-panel.css',
            'js/modules/screen-panel.js', 'js/modules/screen-panel-model.js',
            'pages/fragments/protocol-remote-resources.html'
        ].forEach((asset) => {
            const expected = asset.includes('protocol-remote-resources') ? false : screenSupported;
            assert.strictEqual(shouldIncludeAsset(asset, context), expected,
                `${environment}: optional publish resource ${asset}`);
        });
        assert.strictEqual(shouldIncludeAsset('pages/protocol.html', context), true,
            `${environment}: MQTT/Modbus protocol page must remain available`);
        assert.strictEqual(shouldIncludeMenu('protocol', context), true,
            `${environment}: MQTT/Modbus protocol menu must remain available`);
    });
}
assertFourMbCompileBoundary(PLATFORMIO_TEXT);

function withEnvironmentSection(iniText, environment, transform) {
    const start = iniText.indexOf(`[env:${environment}]`);
    assert.ok(start >= 0);
    const next = iniText.indexOf('\n[', start + 1);
    const end = next < 0 ? iniText.length : next;
    return iniText.slice(0, start) + transform(iniText.slice(start, end)) + iniText.slice(end);
}
assert.throws(() => assertFourMbCompileBoundary(withEnvironmentSection(
    PLATFORMIO_TEXT, 'esp32-F4R0', (section) => section.replace('-DFASTBEE_ENABLE_SCREEN_PANEL=0', '')
)), /esp32-F4R0: FASTBEE_ENABLE_SCREEN_PANEL/,
'removing an enable flag without an explicit zero must fail: firmware defaults remain enabled');
assert.throws(() => assertFourMbCompileBoundary(withEnvironmentSection(
    PLATFORMIO_TEXT, 'esp32-F4R0', (section) => section.replace('-DFASTBEE_ENABLE_SCREEN_PANEL=1', '')
)), /esp32-F4R0: FASTBEE_ENABLE_SCREEN_PANEL/,
'missing build_unflags must fail rather than accept contradictory inherited definitions');
assert.throws(() => assertFourMbCompileBoundary(withEnvironmentSection(
    PLATFORMIO_TEXT, 'esp32c3-F4R0', (section) => section.replace('-DFASTBEE_ENABLE_POINT_HISTORY=0', '')
)), /esp32c3-F4R0: FASTBEE_ENABLE_POINT_HISTORY/,
'C3 point history must stay explicitly disabled to keep its history route out of the image');
assert.throws(() => assertFourMbCompileBoundary(withEnvironmentSection(
    PLATFORMIO_TEXT, 'esp32c3-F4R0', (section) => section.replace('-DFASTBEE_ENABLE_POINT_HISTORY=1', '')
)), /esp32c3-F4R0: FASTBEE_ENABLE_POINT_HISTORY/,
'C3 must remove the inherited point-history enable before applying its zero override');
assert.throws(() => assertFourMbCompileBoundary(withEnvironmentSection(
    PLATFORMIO_TEXT, 'esp32-F8R0', (section) => section.replace('-DFASTBEE_ENABLE_SCREEN_PANEL=0', '')
)), /esp32-F8R0: FASTBEE_ENABLE_SCREEN_PANEL/,
'a larger classic ESP32 target must not silently inherit the S3 screen capability');
assert.throws(() => assertFourMbCompileBoundary(
    PLATFORMIO_TEXT.replace('-DFASTBEE_ENABLE_I2C_SENSORS=1', '-DFASTBEE_ENABLE_I2C_SENSORS=0')
), /preserve FASTBEE_ENABLE_I2C_SENSORS/,
'generic I2C support must not be removed to make the optional packages fit');

const menuOpeningTags = fs.readFileSync(path.join(ROOT_DIR, 'web-src', 'index.html'), 'utf8')
    .match(/<a\b[^>]*>/gi) || [];
const sourceMenuPages = menuOpeningTags
    .filter((tag) => /\bclass\s*=\s*['"][^'"]*\bmenu-item\b/i.test(tag))
    .map((tag) => (tag.match(/\bdata-page\s*=\s*['"]([^'"]+)['"]/i) || [])[1])
    .filter(Boolean)
    .sort();
assert.deepStrictEqual(
    Object.keys(MENU_CAPABILITY_REQUIREMENTS).sort(),
    sourceMenuPages,
    'every sidebar menu must declare an explicit compile-capability rule'
);

const mqttOnlyContext = resolveWebBuildContext({
    environment: 'mqtt-only',
    buildFlags: ['-DFASTBEE_ENABLE_MQTT=1'],
    argv: []
});
const modbusOnlyContext = resolveWebBuildContext({
    environment: 'modbus-only',
    buildFlags: ['-DFASTBEE_ENABLE_MODBUS=1'],
    argv: []
});
const remoteResourcesOnlyContext = resolveWebBuildContext({
    environment: 'remote-resources-only',
    buildFlags: ['-DFASTBEE_ENABLE_REMOTE_RESOURCES=1'],
    argv: []
});
const noProtocolContext = resolveWebBuildContext({
    environment: 'no-protocol',
    buildFlags: ['-DFASTBEE_ENABLE_WEB_STATIC=1'],
    argv: []
});
assert.strictEqual(shouldIncludeMenu('protocol', mqttOnlyContext), true);
assert.strictEqual(shouldIncludeMenu('protocol', modbusOnlyContext), true);
assert.strictEqual(shouldIncludeMenu('protocol', remoteResourcesOnlyContext), false);
assert.strictEqual(shouldIncludeMenu('protocol', noProtocolContext), false);
assert.strictEqual(shouldIncludeAsset('pages/fragments/protocol-mqtt.html', modbusOnlyContext), false);
assert.strictEqual(shouldIncludeAsset('pages/fragments/protocol-modbus-rtu.html', mqttOnlyContext), false);
assert.strictEqual(shouldIncludeAsset('pages/fragments/protocol-remote-resources.html', remoteResourcesOnlyContext), false);
assert.strictEqual(shouldIncludeAsset('pages/screen-panel.html', remoteResourcesOnlyContext), false,
    'remote resources must not pull in the optional screen panel');
assert.strictEqual(shouldIncludeAsset('js/modules/screen-panel-model.js', remoteResourcesOnlyContext), false,
    'the screen panel model follows the same compiled capability as the editor');

['esp32-F4R0', 'esp32c3-F4R0', 'esp32c6-F4R0'].forEach((environment) => {
    assert.strictEqual(contexts[environment].compiledCapabilities.ota, false, `${environment}: OTA`);
    assert.strictEqual(contexts[environment].compiledCapabilities.ethernet, false, `${environment}: Ethernet`);
    assert.strictEqual(contexts[environment].compiledCapabilities.cellular, false, `${environment}: cellular`);
});
assert.strictEqual(contexts['esp32s3-F8R8'].compiledCapabilities.irRemote, false);
assert.strictEqual(contexts['esp32s3-F8R8'].compiledCapabilities.rfLearn, false);
['ota', 'modbus', 'deviceControl', 'irRemote', 'rfLearn', 'ethernet', 'cellular'].forEach((name) => {
    assert.strictEqual(contexts['esp32s3-F16R8'].compiledCapabilities[name], true, `F16R8: ${name}`);
});

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-web-profile-'));
try {
    const inputPath = path.join(tempDir, 'capabilities.json');
    fs.writeFileSync(inputPath, JSON.stringify({
        environment: 'input-env',
        buildFlags: [
            '-DFASTBEE_ENABLE_WEB_STATIC=1',
            '-DFASTBEE_ENABLE_DEVICE_CONTROL=0',
            '-DFASTBEE_ENABLE_MODBUS=1'
        ]
    }), 'utf8');

    const previousManualFlag = process.env.FASTBEE_ENABLE_DEVICE_CONTROL;
    process.env.FASTBEE_ENABLE_DEVICE_CONTROL = '1';
    try {
        const inputContext = resolveWebBuildContext({ capabilityInput: inputPath, argv: [] });
        assert.strictEqual(inputContext.source, 'platformio-resolved-build-flags');
        assert.strictEqual(inputContext.compiledCapabilities.deviceControl, false,
            'manual process flags must not override compiled capabilities');
        assert.strictEqual(shouldIncludeAsset('js/modules/device-control.js', inputContext), false);
    } finally {
        if (previousManualFlag === undefined) delete process.env.FASTBEE_ENABLE_DEVICE_CONTROL;
        else process.env.FASTBEE_ENABLE_DEVICE_CONTROL = previousManualFlag;
    }

    assert.throws(() => resolveWebBuildContext({
        capabilityInput: inputPath,
        environment: 'different-env',
        argv: []
    }), /environment mismatch/);
} finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
}

console.log(`Web profile contract OK: ${FIRMWARE_ENVIRONMENTS.length} firmware environments`);
