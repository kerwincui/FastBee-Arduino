'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const zlib = require('zlib');
const { spawnSync } = require('child_process');
const {
    ASSET_CAPABILITY_REQUIREMENTS,
    MENU_CAPABILITY_REQUIREMENTS,
    MODAL_CAPABILITY_REQUIREMENTS,
    PAGE_ROOT_CAPABILITY_REQUIREMENTS,
    PROTOCOL_TAB_CAPABILITY_REQUIREMENTS,
    capabilityRuleSatisfied,
    shouldIncludeMenu,
    shouldIncludeAsset
} = require('./web-profile');
const { compiledModuleSequences } = require('./build-web-modules');
const { collectPublishedLogicalResources } = require('./web-quality-gate');

const ROOT_DIR = path.resolve(__dirname, '..');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');
const BUILD_SCRIPT = path.join(ROOT_DIR, 'scripts', 'build-web-assets.js');
const SMOKE_SCRIPT = path.join(ROOT_DIR, 'scripts', 'web-smoke-test.js');
const CASES = [
    {
        environment: 'esp32s3-F16R8',
        expected: { ota: true, irRemote: true, rfLearn: true },
        expectedRequestConcurrency: 2
    },
    {
        environment: 'esp32-F4R0',
        expected: { ota: false, ethernet: false, cellular: false },
        expectedRequestConcurrency: 1
    },
    {
        environment: 'esp32c3-F4R0',
        expected: { ota: false, configTransfer: true },
        expectedRequestConcurrency: 1,
        runSmoke: true
    },
    {
        environment: 'synthetic-mqtt-only',
        expected: {
            mqtt: true,
            modbus: false,
            periphExec: false
        },
        buildFlags: [
            '-DFASTBEE_ENABLE_WEB_SERVER=1',
            '-DFASTBEE_ENABLE_WEB_STATIC=1',
            '-DFASTBEE_ENABLE_WEB_API=1',
            '-DFASTBEE_ENABLE_MQTT=1'
        ],
        expectedRequestConcurrency: 1
    },
    {
        environment: 'synthetic-modbus-only',
        expected: { mqtt: false, modbus: true, periphExec: false },
        buildFlags: [
            '-DFASTBEE_ENABLE_WEB_SERVER=1',
            '-DFASTBEE_ENABLE_WEB_STATIC=1',
            '-DFASTBEE_ENABLE_WEB_API=1',
            '-DFASTBEE_ENABLE_MODBUS=1'
        ],
        expectedRequestConcurrency: 1
    },
    {
        environment: 'synthetic-periph-exec-only',
        expected: { mqtt: false, modbus: false, periphExec: true },
        buildFlags: [
            '-DFASTBEE_ENABLE_WEB_SERVER=1',
            '-DFASTBEE_ENABLE_WEB_STATIC=1',
            '-DFASTBEE_ENABLE_WEB_API=1',
            '-DFASTBEE_ENABLE_PERIPH_EXEC=1'
        ],
        expectedRequestConcurrency: 1
    },
    {
        environment: 'synthetic-periph-exec-modbus',
        expected: { mqtt: false, modbus: true, periphExec: true },
        buildFlags: [
            '-DFASTBEE_ENABLE_WEB_SERVER=1',
            '-DFASTBEE_ENABLE_WEB_STATIC=1',
            '-DFASTBEE_ENABLE_WEB_API=1',
            '-DFASTBEE_ENABLE_PERIPH_EXEC=1',
            '-DFASTBEE_ENABLE_MODBUS=1'
        ],
        expectedRequestConcurrency: 1
    },
    {
        environment: 'synthetic-remote-resources-only',
        expected: { mqtt: false, modbus: false, screenPanel: false, remoteResources: true },
        buildFlags: [
            '-DFASTBEE_ENABLE_WEB_SERVER=1',
            '-DFASTBEE_ENABLE_WEB_STATIC=1',
            '-DFASTBEE_ENABLE_WEB_API=1',
            '-DFASTBEE_ENABLE_REMOTE_RESOURCES=1'
        ],
        expectedRequestConcurrency: 1
    },
    {
        environment: 'synthetic-no-optional-protocol',
        expected: { mqtt: false, modbus: false, periphExec: false },
        buildFlags: [
            '-DFASTBEE_ENABLE_WEB_SERVER=1',
            '-DFASTBEE_ENABLE_WEB_STATIC=1',
            '-DFASTBEE_ENABLE_WEB_API=1'
        ],
        expectedRequestConcurrency: 1
    },
    {
        environment: 'esp32s3-F8R8',
        expected: { ota: true, irRemote: false, rfLearn: false },
        expectedRequestConcurrency: 2
    },
    // Leave data/www in the maximum-capability state for subsequent smoke and
    // browser layout checks.
    {
        environment: 'esp32s3-F16R8',
        expected: { ota: true, irRemote: true, rfLearn: true },
        expectedRequestConcurrency: 2,
        runSmoke: true
    }
];

function build(testCase, capabilityInput = '') {
    const args = [BUILD_SCRIPT];
    if (capabilityInput) args.push(`--capability-input=${capabilityInput}`);
    else args.push(`--env=${testCase.environment}`);
    const result = spawnSync(process.execPath, args, {
        cwd: ROOT_DIR,
        encoding: 'utf8',
        maxBuffer: 16 * 1024 * 1024
    });
    if (result.status !== 0) {
        process.stderr.write(result.stdout || '');
        process.stderr.write(result.stderr || '');
        throw new Error(`Web capability build failed for ${testCase.environment} (exit ${result.status})`);
    }
}

function runSmoke(testCase) {
    const result = spawnSync(process.execPath, [SMOKE_SCRIPT], {
        cwd: ROOT_DIR,
        encoding: 'utf8',
        maxBuffer: 16 * 1024 * 1024
    });
    if (result.status !== 0) {
        process.stderr.write(result.stdout || '');
        process.stderr.write(result.stderr || '');
        throw new Error(`Web smoke test failed for ${testCase.environment} (exit ${result.status})`);
    }
}

function readGzipText(relPath) {
    return zlib.gunzipSync(fs.readFileSync(path.join(WWW_DIR, `${relPath}.gz`))).toString('utf8');
}

function extractMenuPages(html) {
    return (html.match(/<a\b[^>]*>/gi) || [])
        .filter((tag) => /\bclass\s*=\s*['"][^'"]*\bmenu-item\b/i.test(tag))
        .map((tag) => (tag.match(/\bdata-page\s*=\s*['"]([^'"]+)['"]/i) || [])[1])
        .filter(Boolean);
}

function parseBuiltSequenceMap(source) {
    const match = source.match(/_sequenceMap:(\{[\s\S]*?\})\s*,\s*_resolveAssetUrl/);
    assert.ok(match, 'built ModuleLoader sequence map must be discoverable');
    return Function(`"use strict"; return (${match[1]});`)();
}

function parseBuiltPreloadPages(source) {
    const match = source.match(/PageLoader\.preloadPages\((\[[^\]]*\])\s*,\s*self\._pageMapping\s*,\s*\{delayMs:1200\}\)/);
    assert.ok(match, 'built page preload list must be discoverable');
    return JSON.parse(match[1].replace(/'/g, '"'));
}

function verifyCase({ environment, expected, buildFlags, expectedRequestConcurrency }) {
    const manifest = JSON.parse(fs.readFileSync(
        path.join(WWW_DIR, 'web-resource-manifest.json'),
        'utf8'
    ).replace(/^\uFEFF/, ''));
    assert.strictEqual(manifest.environment, environment);
    assert.strictEqual(
        manifest.capabilitySource,
        buildFlags ? 'platformio-resolved-build-flags' : 'platformio.ini'
    );
    assert.strictEqual(manifest.runtimeConfigurationAffectsResources, false);
    Object.entries(expected).forEach(([name, value]) => {
        assert.strictEqual(manifest.compiledCapabilities[name], value, `${environment}: ${name}`);
    });
    assert.deepStrictEqual(manifest.requestGovernor, {
        maxConcurrent: expectedRequestConcurrency,
        maxWeight: expectedRequestConcurrency,
        maxHeavy: 1,
        memoryProfile: expectedRequestConcurrency === 2 ? 'psram' : 'internal-dram'
    }, `${environment}: manifest request governor profile`);

    const chunk1 = readGzipText('js/chunk-1-core.js');
    const builtLimits = chunk1.match(
        /WEB_PROFILE_REQUEST_LIMITS\s*=\s*Object\.freeze\(\{\s*maxConcurrent\s*:\s*(\d+)\s*,\s*maxWeight\s*:\s*(\d+)\s*\}\)/
    );
    assert.ok(builtLimits, `${environment}: built request governor limits must be discoverable`);
    assert.deepStrictEqual(
        builtLimits.slice(1).map(Number),
        [expectedRequestConcurrency, expectedRequestConcurrency],
        `${environment}: built request governor limits`
    );

    const resources = collectPublishedLogicalResources(WWW_DIR);
    assert.deepStrictEqual(resources, manifest.resources,
        `${environment}: manifest and published resource sets must match`);
    const context = { compiledCapabilities: manifest.compiledCapabilities };
    Object.keys(ASSET_CAPABILITY_REQUIREMENTS).forEach((relPath) => {
        assert.strictEqual(
            manifest.resources.includes(relPath),
            shouldIncludeAsset(relPath, context),
            `${environment}: ${relPath}`
        );
    });

    const indexHtml = readGzipText('index.html');
    const expectedMenus = Object.keys(MENU_CAPABILITY_REQUIREMENTS)
        .filter((pageName) => shouldIncludeMenu(pageName, context));
    assert.deepStrictEqual(extractMenuPages(indexHtml), expectedMenus,
        `${environment}: sidebar menus must match compiled capabilities`);

    const peripheralHtml = readGzipText('pages/peripheral.html');
    Object.entries(PAGE_ROOT_CAPABILITY_REQUIREMENTS).forEach(([rootId, rule]) => {
        assert.strictEqual(
            peripheralHtml.includes(`id="${rootId}"`),
            capabilityRuleSatisfied(rule, context),
            `${environment}: #${rootId} page root`
        );
    });
    const peripheralsCore = readGzipText('js/modules/peripherals.js');
    const peripheralSave = readGzipText('js/modules/peripheral-save.js');
    const peripheralIo = readGzipText('js/modules/peripheral-io.js');
    ['peripheral-board-resources', 'peripheral-automation', 'peripheral-experiments'].forEach((removedId) => {
        assert.ok(!peripheralHtml.includes(removedId),
            `${environment}: peripheral workspace must not expose #${removedId}`);
    });
    ['js/modules/peripheral-board.js', 'js/modules/peripheral-experiments.js'].forEach((removedAsset) => {
        assert.ok(!manifest.resources.includes(removedAsset),
            `${environment}: removed peripheral workspace asset must not be published: ${removedAsset}`);
    });
    assert.ok(!peripheralsCore.includes('/api/boards/profiles') && !peripheralsCore.includes('/api/peripherals/resources'),
        `${environment}: board/resource APIs must stay out of the peripherals core`);
    assert.ok(/ModuleLoader\.loadModule\(["']peripheral-save["'],[A-Za-z_$][\w$]*\)/.test(peripheralsCore),
        `${environment}: save/preflight code must stay in an on-demand module`);
    assert.ok(/ModuleLoader\.loadModule\(["']peripheral-io["'],[A-Za-z_$][\w$]*\)/.test(peripheralsCore),
        `${environment}: diagnostics code must stay in an on-demand module`);
    assert.ok(peripheralSave.includes("registerModule('peripheral-save'") && peripheralSave.includes('/api/peripherals/preflight'),
        `${environment}: peripheral save/preflight module contract`);
    assert.ok(/registerModule\(["']peripheral-io["']\s*,/.test(peripheralIo) &&
        /loadFragment\(["']["']\s*,\s*["']peripheral-io["']\)/.test(peripheralIo),
        `${environment}: peripheral diagnostics module contract`);

    const modalsHtml = readGzipText('pages/modals.html');
    Object.entries(MODAL_CAPABILITY_REQUIREMENTS).forEach(([modalId, rule]) => {
        assert.strictEqual(
            modalsHtml.includes(`id="${modalId}"`),
            capabilityRuleSatisfied(rule, context),
            `${environment}: #${modalId} modal`
        );
    });

    const protocolPublished = manifest.resources.includes('pages/protocol.html');
    assert.strictEqual(protocolPublished, manifest.compiledCapabilities.mqtt || manifest.compiledCapabilities.modbus,
        `${environment}: shared protocol page uses MQTT or Modbus semantics`);
    if (protocolPublished) {
        const protocolHtml = readGzipText('pages/protocol.html');
        const enabledTabs = Object.entries(PROTOCOL_TAB_CAPABILITY_REQUIREMENTS)
            .filter(([, rule]) => capabilityRuleSatisfied(rule, context))
            .map(([tabId]) => tabId);
        enabledTabs.forEach((tabId, index) => {
            assert.ok(protocolHtml.includes(`data-tab="${tabId}"`), `${environment}: ${tabId} tab missing`);
            const tabTag = (protocolHtml.match(new RegExp(`<div[^>]*data-tab="${tabId}"[^>]*>`)) || [])[0] || '';
            assert.strictEqual(/\bactive\b/.test(tabTag), index === 0,
                `${environment}: first compiled protocol tab must be active`);
        });
        Object.entries(PROTOCOL_TAB_CAPABILITY_REQUIREMENTS).forEach(([tabId, rule]) => {
            if (!capabilityRuleSatisfied(rule, context)) {
                assert.ok(!protocolHtml.includes(`data-tab="${tabId}"`),
                    `${environment}: unavailable ${tabId} tab must be removed`);
            }
        });
    }

    const deviceHtml = readGzipText('pages/device.html');
    assert.strictEqual(deviceHtml.includes('data-tab="dev-ota"'), manifest.compiledCapabilities.ota === true,
        `${environment}: OTA tab must match the compiled OTA capability`);

    const chunk2 = readGzipText('js/chunk-2-app.js');
    const moduleSequences = compiledModuleSequences(context);
    assert.deepStrictEqual(parseBuiltSequenceMap(chunk2), moduleSequences,
        `${environment}: runtime module sequences must not reference omitted assets`);
    assert.deepStrictEqual(moduleSequences['screen-panel'], manifest.compiledCapabilities.screenPanel
        ? ['screen-panel-model', 'screen-panel'] : undefined,
    `${environment}: screen panel must load its model first only when compiled`);
    if (manifest.compiledCapabilities.periphExec) {
        const editorSequence = moduleSequences['periph-exec-editor'] || [];
        assert.ok(editorSequence.includes('periph-exec-form'),
            `${environment}: periph exec editor must load the core form`);
        assert.ok(!editorSequence.includes('periph-exec-sensor') && !editorSequence.includes('periph-exec-effects'),
            `${environment}: sensor/effects modules must remain on demand`);
        const periphExecCore = readGzipText('js/modules/periph-exec.js');
        assert.ok(periphExecCore.includes('periph-exec-sensor') && periphExecCore.includes('periph-exec-effects'),
            `${environment}: periph exec core must route lazy action modules`);
        assert.ok(readGzipText('js/modules/periph-exec-sensor.js').includes("registerModule('periph-exec-sensor'"),
            `${environment}: sensor extension must register with ModuleLoader`);
        assert.ok(readGzipText('js/modules/periph-exec-effects.js').includes("registerModule('periph-exec-effects'"),
            `${environment}: effects extension must register with ModuleLoader`);
    }
    const chunk3 = readGzipText('js/chunk-3-state.js');
    assert.ok(!chunk3.includes('peripheral-experiments'),
        `${environment}: state bundle must not reference the removed experiment workspace`);
    const expectedPreloads = ['device-page', 'network-page', 'peripheral-page'];
    if (protocolPublished) expectedPreloads.push('protocol-page');
    assert.deepStrictEqual(parseBuiltPreloadPages(chunk3), expectedPreloads,
        `${environment}: page preloads must not request omitted pages`);
    if (!manifest.compiledCapabilities.mqtt && protocolPublished) {
        const expectedDefaultTab = 'modbus-rtu';
        assert.ok(chunk3.includes(`this.loadProtocolConfig('${expectedDefaultTab}')`),
            `${environment}: protocol-only build must open its first compiled tab`);
    }
}

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-web-capability-build-'));
try {
    CASES.forEach((testCase) => {
        let capabilityInput = '';
        if (testCase.buildFlags) {
            capabilityInput = path.join(tempDir, `${testCase.environment}.json`);
            fs.writeFileSync(capabilityInput, JSON.stringify({
                environment: testCase.environment,
                buildFlags: testCase.buildFlags
            }), 'utf8');
        }
        build(testCase, capabilityInput);
        verifyCase(testCase);
        if (testCase.runSmoke) runSmoke(testCase);
    });
} finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
}

console.log(`Web capability build contract OK: ${CASES.length} clean builds`);
