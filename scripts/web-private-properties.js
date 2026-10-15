'use strict';

// Build-host only. These reviewed AppState private members are intentionally
// append-only within each family: their fixed names must not depend on build
// order, profile inclusion, or Terser's frequency-based name selection.
// Do not add wildcard patterns. Public methods, DOM metadata, wire keys,
// quoted/reflected names and cross-family consumers are excluded.
const fs = require('node:fs');
const path = require('node:path');
const {parse} = require('acorn');
const {minify_sync} = require('terser');
const ROOT = path.resolve(__dirname, '..');
const FAMILY_FILES = Object.freeze({
    execution: Object.freeze(['periph-exec.js', 'periph-exec-form.js', 'periph-exec-sensor.js',
        'periph-exec-templates.js', 'periph-exec-modbus.js', 'periph-exec-effects.js']),
    peripherals: Object.freeze(['peripherals.js', 'peripheral-io.js', 'peripheral-save.js'])
});
const PRIVATE_NAMES = Object.freeze({
    execution: Object.freeze([
        '_bindPeriphExecModalEvents',
        '_buildPeriphExecEffectsDisplayPanel',
        '_buildPeriphExecFormWarnings',
        '_buildPeriphExecInlineActionPanel',
        '_buildPeriphExecModbusPanel',
        '_buildPeriphExecTemplateDraft',
        '_clearAllFieldErrors',
        '_clearPeriphExecEventCatalogCache',
        '_clearPeriphExecRiskNotice',
        '_collectPeriphExecActions',
        '_collectPeriphExecEffectsAction',
        '_collectPeriphExecSensorAction',
        '_collectPeriphExecTriggers',
        '_collectSdRecorderConfig',
        '_collectThermostatConfig',
        '_createPeriphExecActionElement',
        '_createPeriphExecTriggerElement',
        '_ensurePeriphExecEditor',
        '_ensurePeriphExecModbusHealth',
        '_extractDataSources',
        '_findPeriphExecRemoteChannel',
        '_formatOledCounterStats',
        '_formatPeriphExecAge',
        '_formatPeriphExecCapacitySummary',
        '_formatPeriphExecPercent',
        '_formatPeriphExecUptimeMs',
        '_getConfiguredSensorDetails',
        '_getModbusDeviceByPeriphId',
        '_getPeriphExecActionSummary',
        '_getPeriphExecBlock',
        '_getPeriphExecBlockIndex',
        '_getPeriphExecBlockLimit',
        '_getPeriphExecCachedHealth',
        '_getPeriphExecDynamicEvents',
        '_getPeriphExecEventCatalog',
        '_getPeriphExecEventCategories',
        '_getPeriphExecLazyActionModule',
        '_getPeriphExecLocalSensorSources',
        '_getPeriphExecPageGeneration',
        '_getPeriphExecRiskMeta',
        '_getPeriphExecSensorPanelState',
        '_getPeriphExecStaticEvents',
        '_getPeriphExecTemplateTarget',
        '_getPeriphExecTriggerSummary',
        '_handlePeriphExecActionChange',
        '_handlePeriphExecActionClick',
        '_handlePeriphExecTriggerChange',
        '_handlePeriphExecTriggerClick',
        '_handlePeriphExecTriggerInput',
        '_hasSetModeTrigger',
        '_hiddenClass',
        '_highlightFieldError',
        '_initNeoPixelPresetFromData',
        '_invalidatePeriphExecRuntimeCaches',
        '_invalidatePeriphExecTemplatePreview',
        '_isNeoPixelTarget',
        '_isPeriphExecCapacityFull',
        '_isPeriphExecEditLoadActive',
        '_isPeriphExecEditRetryable',
        '_isPeriphExecEditorReady',
        '_isPollTriggerActive',
        '_isRfModuleTarget',
        '_isSetPwmTarget',
        '_loadPeriphExecEditData',
        '_loadPeriphExecLazyActionModule',
        '_loadPeriphExecRemoteChoices',
        '_makePeriphExecCollectionDraft',
        '_mountPeriphExecLazyActionPanel',
        '_onCtrlDeviceToggle',
        '_onNeoPixelPresetChange',
        '_onPeriphChannelSelect',
        '_onPeriphDeviceSelect',
        '_onTargetPeriphChange',
        '_pca9685CommandCatalog',
        '_pca9685CommandError',
        '_peConfiguredSensorError',
        '_peCurrentPage',
        '_peDataSources',
        '_peEditLoadSeq',
        '_peEditorDataReady',
        '_peExecRules',
        '_peLazyModuleError',
        '_peListFilter',
        '_peListSequence',
        '_peManualRuns',
        '_peModbusHealth',
        '_peModbusHealthFetchedAt',
        '_peModbusHealthPromise',
        '_pePageSize',
        '_pePeripherals',
        '_pePreflightReview',
        '_peProfile',
        '_peRemoteChannels',
        '_peRemoteResourcesAvailable',
        '_peSavePending',
        '_peSdRecorder',
        '_peSensorExtraJsonError',
        '_peSensorSources',
        '_peTemplateModbusConfig',
        '_peTemplatePreview',
        '_peThermostats',
        '_peTotalRules',
        '_peTypeCatalog',
        '_peWeatherSources',
        '_periphExecContractNotes',
        '_periphExecFormFingerprint',
        '_periphExecTemplateIdentity',
        '_populateButtonPeriphSelect',
        '_populateEventCategoriesInBlock',
        '_populateEventSelectInBlock',
        '_populateIrKeySelect',
        '_populateModbusDevicePanel',
        '_populatePeriphExecFilter',
        '_populatePeriphSelect',
        '_populateRemoteResourceSelect',
        '_populateRfKeySelect',
        '_populateRtcTargetSelect',
        '_populateSdTargetSelect',
        '_populateSensorPeriphSelect',
        '_populateThermostatTargetSelect',
        '_populateTriggerPeriphSelect',
        '_preflightPeriphExecSave',
        '_readPeriphExecTemplateInput',
        '_rebuildActionBlocksForTriggerChange',
        '_refreshPeriphExecList',
        '_refreshPeriphExecResults',
        '_refreshPeriphExecRiskNotice',
        '_refreshPeriphExecTemplateChoices',
        '_reindexPeriphExecBlocks',

        '_renderPeriphExecActionButton',
        '_renderPeriphExecLazyActionPanel',
        '_renderPeriphExecPagination',
        '_renderPeriphExecResults',
        '_renderPeriphExecRuleActions',
        '_renderPeriphExecRuleRow',
        '_renderPeriphExecStatusBadge',
        '_renderRtcCommandOptions',
        '_renderRtcSetOptions',
        '_renderSdRecorderRuntime',
        '_renderSensorDataFieldOptions',
        '_renderThermostatRuntime',

        '_requirePeriphExecData',
        '_resetPeriphExecTemplateWizard',
        '_resolvePeriphExecRiskLevel',
        '_restorePeriphExecLazyPanel',
        '_sdRecorderConfigSchema',
        '_setPeriphExecCapacity',
        '_setSectionVisible',
        '_setupSdRecorderRuleForm',
        '_setupThermostatRuleForm',
        '_showModbusCtrlPanel',
        '_showModbusTriggerCtrlPanel',
        '_snapshotPeriphExecLazyPanel',
        '_storePeriphExecTypeCatalog',
        '_syncConfiguredSensorAction',
        '_syncDhtSensorField',
        '_syncModbusActionOptions',
        '_syncPca9685ActionFields',
        '_syncRemoteActionValueState',
        '_syncSetModeToActions',
        '_thermostatConfigSchema',
        '_thermostatSources',
        '_thermostatTargets',
        '_uartSendHelp',
        '_updateExpanderActionAvailability',
        '_updateModbusWriteHelp',
        '_updateNeoPixelPresetVisibility',
        '_updatePeriphExecAddButtons',
        '_updatePeriphExecPanelOptions',
        '_validateSdRecorderForm',
        '_validateThermostatForm'
    ]),
    peripherals: Object.freeze([
        '_appendPeripheralIoLog',
        '_applyAnalogSignalDefaults',
        '_applyH1ConfigInput',
        '_bindPeripheralIoModalEvents',
        '_bindPeripheralModalEvents',
        '_canOpenPeripheralDiagnostics',
        '_clearPeripheralDs1302Reading',
        '_collectI2cDriverConfig',
        '_collectPca9685Config',
        '_descriptorPinRoles',
        '_descriptorStringList',
        '_diagnosticButtonLabel',
        '_diagnosticButtonTitle',
        '_ensurePeripheralIoModal',
        '_fallbackPinRoles',
        '_fillDriverSelect',
        '_formatI2cAddress',
        '_formatPeripheralBytes',
        '_formatPeripheralCapacitySummary',
        '_getPeripheralDiagnosticBlockReason',
        '_getPeripheralDiagnosticKind',
        '_getPeripheralDriverDescriptors',
        '_getPeripheralDriverMeta',
        '_getPeripheralTypeDescriptor',
        '_getPeripheralUartPayloadInfo',
        '_getPinRecommendation',
        '_h1FieldError',
        '_handlePeripheralTableClick',
        '_initPeripheralRemotePanel',
        '_isPeripheralTypeAvailable',
        '_isUnsupportedPeripheralPreflight',
        '_loadPeripheralHardwareCapabilities',
        '_loadPeripheralProviders',
        '_loadPeripheralSaveModule',
        '_normalizePeripheralHex',
        '_normalizePeripheralPreflight',
        '_openPeripheralIoWorkspace',
        '_openPeripheralResources',
        '_pausePeripheralUartPolling',
        '_pendingSensorDriverParams',
        '_periphCurrentPage',
        '_periphFormConfig',
        '_periphFormSession',
        '_periphHardware',
        '_periphI2cOperation',
        '_periphIoBlocked',
        '_periphIoBusy',
        '_periphIoConfig',
        '_periphIoId',
        '_periphIoKind',
        '_periphIoModalBound',
        '_periphIoPollTimer',
        '_periphIoSession',
        '_periphIoType',
        '_periphListFilter',
        '_periphListSequence',
        '_periphPageSize',
        '_periphPcaReview',
        '_periphPcaStatus',
        '_periphPreflightAcknowledgements',
        '_periphPreflightFingerprint',
        '_periphProfile',
        '_periphProviders',
        '_periphResourceDialog',
        '_periphResourceOpen',
        '_periphResourceRequest',
        '_periphSaveBusy',
        '_periphToggleBusy',
        '_periphTotalCount',
        '_periphTypeCatalog',
        '_periphTypeCatalogLoading',
        '_periphUartHexLog',
        '_periphUartTextLog',
        '_peripheralConfigFingerprint',
        '_peripheralIoErrorMessage',
        '_peripheralResourcesText',
        '_populatePeripheralDiagnosticChannels',
        '_refreshPeripheralList',
        '_renderDriverParams',
        '_renderPeriphPagination',
        '_renderPeripheralActionButton',
        '_renderPeripheralDs1302Data',
        '_renderPeripheralI2cData',
        '_renderPeripheralIoReading',
        '_renderPeripheralLcd1602Data',
        '_renderPeripheralPca9685',
        '_renderPeripheralPreflight',
        '_renderPeripheralRfidData',
        '_renderPeripheralSdData',
        '_resetPeripheralPreflight',
        '_runPeripheralPreflight',
        '_savePeripheralConfigWithModule',
        '_schedulePeripheralIoPolling',
        '_selectMcpGroupChannels',
        '_selectMcpGroupChannelsLoaded',
        '_setFeatureAvailabilityHint',
        '_setPeripheralCapacity',
        '_setPeripheralIoBusy',
        '_setPeripheralIoError',
        '_setPeripheralIoOutput',
        '_setPeripheralIoText',
        '_setPeripheralReferenceOptions',
        '_showPeripheralRemoteManagement',
        '_stopPeripheralIoPolling',
        '_syncPeripheralTypeSelect',
        '_updateDisplayFields',
        '_updateGenericSensorDriverFields',
        '_updateGpioSourceFields',
        '_updateIoExpanderFields',
        '_updatePca9685Controls',
        '_updatePca9685Fields',
        '_updatePeripheralPinRoleHint',
        '_updatePeripheralTypeAvailability',
        '_updateSdContentCount',
        '_updateSdcardPinHints',
        '_validatePinsInline',
        '_restoreRtcBootSeed'
    ])
});
const MAPPING = Object.freeze(Object.fromEntries(Object.entries(PRIVATE_NAMES).flatMap(([family, names]) =>
    names.map((name, index) => [name, '$' + (family === 'execution' ? 'e' : 'p') + index.toString(36)]))));
const NAME_SETS = Object.fromEntries(Object.entries(PRIVATE_NAMES).map(([family, names]) => [family, new Set(names)]));
const FILE_FAMILY = Object.fromEntries(Object.entries(FAMILY_FILES).flatMap(([family, files]) => files.map(file => [file, family])));
const PRIVATE_TOKEN = /\b_[A-Za-z][A-Za-z0-9_]*/g;

function walk(root, accept, result) {
    if (!fs.existsSync(root)) return;
    for (const entry of fs.readdirSync(root, {withFileTypes: true})) {
        const file = path.join(root, entry.name);
        if (entry.isDirectory()) walk(file, accept, result);
        else if (entry.isFile() && accept(file)) result.push({file, text: fs.readFileSync(file, 'utf8')});
    }
}

function readAuditSources(rootDir = ROOT) {
    const records = [];
    walk(path.join(rootDir, 'web-src'), file => /\.(?:js|html|css|json)$/.test(file), records);
    for (const directory of ['src', 'include'])
        walk(path.join(rootDir, directory), file => /\.(?:cpp|h|hpp|c|ino|json)$/.test(file), records);
    // External configuration can define data keys. Never inspect published JS:
    // its fixed private names are expected and it is not an editable consumer.
    const data = path.join(rootDir, 'data');
    if (fs.existsSync(data)) for (const entry of fs.readdirSync(data, {withFileTypes: true})) {
        const file = path.join(data, entry.name);
        if (entry.isFile() && entry.name.endsWith('.json')) records.push({file, text: fs.readFileSync(file, 'utf8')});
        else if (entry.isDirectory() && entry.name !== 'www')
            walk(file, candidate => candidate.endsWith('.json'), records);
    }
    return records;
}

function traverse(node, visit, ancestors = []) {
    if (!node || typeof node !== 'object') return;
    visit(node, ancestors);
    const parents = ancestors.concat(node);
    for (const value of Object.values(node)) {
        if (Array.isArray(value)) value.forEach(item => traverse(item, visit, parents));
        else if (value && typeof value === 'object') traverse(value, visit, parents);
    }
}

function parseSource(source, file) {
    try { return parse(source, {ecmaVersion: 'latest', sourceType: 'script'}); }
    catch (scriptError) {
        try { return parse(source, {ecmaVersion: 'latest', sourceType: 'module'}); }
        catch (_) { throw new Error('Cannot audit private-property consumer: ' + file + ': ' + scriptError.message); }
    }
}

function conditionalLiteralKeys(node) {
    return node.type === 'Literal' && typeof node.value === 'string' ||
        node.type === 'ConditionalExpression' && conditionalLiteralKeys(node.consequent) && conditionalLiteralKeys(node.alternate);
}

function decodeInlineEscapes(text) {
    // Conservative whole-HTML scan also covers inline handlers and scripts,
    // including numeric entities and JS escaped string literals. Scanning the
    // entire document deliberately rejects ambiguous reflected prefixes.
    return text.replace(/&#(?:x([0-9a-f]+)|([0-9]+));?/gi, (whole, hex, dec) => {
        const code = parseInt(hex || dec, hex ? 16 : 10);
        return code <= 0x10ffff ? String.fromCodePoint(code) : whole;
    }).replace(/\\(?:u\{([0-9a-f]+)\}|u([0-9a-f]{4})|x([0-9a-f]{2}))/gi, (whole, long, unit, byte) => {
        const code = parseInt(long || unit || byte, 16);
        return code <= 0x10ffff ? String.fromCodePoint(code) : whole;
    });
}

function auditPrivateRuntimeSources(records, rootDir = ROOT) {
    const runtimeDir = path.join(rootDir, 'web-src/modules/runtime');
    const mappedNames = Object.keys(MAPPING), generated = Object.values(MAPPING);
    if (new Set(generated).size !== generated.length) throw new Error('Private property mapping collision');
    const found = new Set();
    for (const record of records) {
        const decodedText = decodeInlineEscapes(record.text);
        // Fail closed even for a key in an external JSON value or inline HTML.
        for (const name of generated) if (decodedText.includes(name))
            throw new Error('Generated private name already exists: ' + name + ' in ' + record.file);
        const relative = path.relative(runtimeDir, record.file);
        const family = !relative.includes(path.sep) ? FILE_FAMILY[relative] : undefined;
        const tokens = [...decodedText.matchAll(PRIVATE_TOKEN)].map(match => match[0]);
        for (const token of tokens) {
            for (const name of mappedNames) if (name === token) {
                if (!family || !NAME_SETS[family].has(name))
                    throw new Error('External private member consumer: ' + name + ' in ' + record.file);
                if (name === token) found.add(name);
            }
        }
        if (!family) {
            if (record.file.endsWith('.html')) {
                for (const match of decodedText.matchAll(PRIVATE_TOKEN))
                    if (mappedNames.some(name => name.startsWith(match[0])))
                        throw new Error('External reflected private prefix: ' + match[0] + ' in ' + record.file);
            }
            if (record.file.endsWith('.js')) traverse(parseSource(record.text, record.file), node => {
                // Acorn resolves escaped identifier spelling. Check the actual
                // member/identifier name as well as the raw source token scan.
                if (node.type === 'Identifier' && Object.prototype.hasOwnProperty.call(MAPPING, node.name))
                    throw new Error('External private member consumer: ' + node.name + ' in ' + record.file);
                if ((node.type !== 'Literal' || typeof node.value !== 'string') && node.type !== 'TemplateElement') return;
                const value = node.type === 'Literal' ? node.value : (node.value.cooked ?? node.value.raw);
                const prefixes = [...value.matchAll(PRIVATE_TOKEN)].map(match => match[0]);
                for (const prefix of prefixes) if (mappedNames.some(name => name.startsWith(prefix)))
                    throw new Error('External reflected private prefix: ' + prefix + ' in ' + record.file);
            });
            continue;
        }
        const tree = parseSource(record.text, record.file);
        traverse(tree, (node, ancestors) => {
            if (node.type === 'CallExpression' && node.callee.type === 'MemberExpression' && !node.callee.computed &&
                ['_initPeripheralRemotePanel', '_showPeripheralRemoteManagement', '_getPeriphExecEventCatalog'].includes(node.callee.property.name)) {
                const index = node.callee.property.name === '_getPeriphExecEventCatalog' ? 0 : 1;
                const argument = node.arguments[index];
                if (argument?.type !== 'Literal' || typeof argument.value !== 'string' ||
                    mappedNames.some(name => name.startsWith(argument.value)))
                    throw new Error('Dynamic dispatch key is not an excluded literal in ' + record.file);
            }
            if ((node.type === 'Literal' && typeof node.value === 'string') || node.type === 'TemplateElement') {
                const text = node.type === 'Literal' ? node.value : (node.value.cooked ?? node.value.raw);
                const tokens = [...text.matchAll(PRIVATE_TOKEN)].map(match => match[0]);
                for (const token of tokens) for (const name of PRIVATE_NAMES[family])
                    if (name.startsWith(token)) throw new Error('Reflected private name/prefix must stay public: ' + token + ' in ' + record.file);
            }
            if (node.type !== 'MemberExpression' || !node.computed ||
                !(node.object.type === 'ThisExpression' ||
                  (node.object.type === 'Identifier' && ['self', 'AppState'].includes(node.object.name)))) return;
            const property = ancestors.slice().reverse().find(parent => parent.type === 'Property' &&
                ['FunctionExpression', 'ArrowFunctionExpression'].includes(parent.value?.type));
            const method = property?.key?.name;
            // The only dynamic AppState key family in these modules is the
            // event cache. Its quoted base keys and both suffixes are excluded.
            if (relative === 'periph-exec-form.js' &&
                ['_clearPeriphExecEventCatalogCache', '_getPeriphExecEventCatalog'].includes(method)) return;
            if (relative === 'periph-exec-templates.js' && method === '_renderPeriphExecLazyActionPanel' &&
                conditionalLiteralKeys(node.property)) return; // Every literal is audited above.
            if (relative === 'peripherals.js' &&
                ['_initPeripheralRemotePanel', '_showPeripheralRemoteManagement'].includes(method)) return;
            throw new Error('Unreviewed computed AppState access in ' + record.file + ':' + node.start);
        });
    }
    const missing = mappedNames.filter(name => !found.has(name));
    if (missing.length) throw new Error('Stale private-member whitelist: ' + missing.join(', '));
    return {members: mappedNames.length, generatedNames: generated.length};
}

function createPrivateRuntimeMinifier(rootDir = ROOT, records = readAuditSources(rootDir)) {
    auditPrivateRuntimeSources(records, rootDir);
    const inputs = new Map(records.filter(record => path.dirname(record.file) === path.join(rootDir, 'web-src/modules/runtime'))
        .map(record => [path.basename(record.file), record.text]));
    // The same predetermined cache serves direct and bundled runtime writers.
    const nameCache = {props: {props: Object.fromEntries(Object.entries(MAPPING).map(([key, value]) => ['$' + key, value]))}};
    return {
        minify(fileName, source) {
            const family = FILE_FAMILY[fileName];
            if (!family) throw new Error('No reviewed private runtime family: ' + fileName);
            const original = inputs.get(fileName);
            if (!original || !source.includes(original.trim()))
                throw new Error('Runtime input changed after private-member audit: ' + fileName);
            const ordinary = ['periph-exec.js', 'periph-exec-form.js', 'peripherals.js', 'peripheral-io.js'].includes(fileName);
            return minify_sync(source, {
                compress: {passes: 2},
                // Reviewed printer-only choices. They change literal delimiters,
                // never values, public keys, compression safety or function names.
                format: {quote_style: ({'periph-exec.js': 2, 'periph-exec-form.js': 1,
                    'periph-exec-modbus.js': 1, 'peripherals.js': 2, 'peripheral-io.js': 1})[fileName] || 0},
                mangle: {properties: {regex: new RegExp('^(?:' + PRIVATE_NAMES[family].join('|') + ')$'), keep_quoted: true}},
                ...(ordinary ? {} : {keep_fnames: true}),
                nameCache
            }).code;
        }
    };
}

let sharedMinifier;
function minifyPrivateRuntimeJS(fileName, source) {
    if (!sharedMinifier) sharedMinifier = createPrivateRuntimeMinifier();
    return sharedMinifier.minify(fileName, source);
}
function isPrivateRuntimeModule(fileName) { return Object.prototype.hasOwnProperty.call(FILE_FAMILY, fileName); }

module.exports = {minifyPrivateRuntimeJS, isPrivateRuntimeModule, createPrivateRuntimeMinifier,
    auditPrivateRuntimeSources, readAuditSources, PRIVATE_NAMES, FAMILY_FILES, MAPPING};
