'use strict';

/**
 * Keep the Waveshare Touch LCD4 RGB build contract explicit.
 *
 * The display JSON is a run-time configuration while the bounce-buffer size
 * is a compile-time choice.  A build can therefore appear to request 16
 * lines in Web/configuration and still ship a different (or direct-EDMA)
 * backend unless the two artefacts are checked together.
 */

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const TOUCH_BUNDLE = path.join(ROOT, 'docs', 'device-test-configs', 'com8-com9-esp32s3-touch', 'config');
const CONTRACT_FILE = path.join(TOUCH_BUNDLE, '..', 'rgb-build-contract.json');
const RGB_BACKEND_SOURCE = path.join(ROOT, 'src', 'screen', 'RgbDisplayBackend.cpp');
const OPTIONAL_CANDIDATE_INI = path.join(ROOT, 'artifacts', 'platformio-rgb-xip16-r19.ini');
const OPTIONAL_BUILD_REPORTS = [
    path.join(ROOT, 'artifacts', 'rgb-resume-touch-roll-edge-build.json'),
    path.join(ROOT, 'artifacts', 'rgb-xip16-r19-build.json')
];

const EXPECTED = Object.freeze({
    backend: 'rgb',
    width: 480,
    height: 480,
    bufferLines: 16,
    rgbFrequency: 12000000,
    environment: 'esp32s3-F16R8-rgb-xip40-v12'
});

function readJson(filePath) {
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function findTouchDisplay(bundle) {
    const items = Array.isArray(bundle.peripherals) ? bundle.peripherals : [];
    const display = items.find((item) => item && item.type === 56);
    if (!display) throw new Error('Touch LCD4 bundle has no type-56 display');
    return display;
}

function parseCandidateIni(text) {
    const section = text.match(/\[env:([^\]\r\n]+)\]/);
    const flags = [...text.matchAll(/^\s*(-DFASTBEE_RGB_[A-Z_]+=-?\d+)\s*$/gm)]
        .map((match) => match[1]);
    const sdkLines = text.split(/\r?\n/)
        .map((line) => line.trim())
        .filter((line) => line && !line.startsWith(';'));
    return {
        environment: section ? section[1].trim() : '',
        flags,
        sdkLines,
        text
    };
}

function candidateFromContract(contract) {
    return {
        environment: contract.environment,
        flags: [...contract.buildFlags],
        sdkLines: [
            ...contract.sdkConfig,
            ...contract.disabledSdkConfig.map((name) => `# ${name} is not set`)
        ],
        text: JSON.stringify(contract)
    };
}

function validateTouchBuildContract({ bundle, candidateIni, build }) {
    const display = findTouchDisplay(bundle);
    const params = display.params || {};
    const errors = [];
    const fail = (message) => errors.push(message);

    if (params.backend !== EXPECTED.backend) fail(`display backend must be ${EXPECTED.backend}`);
    if (params.width !== EXPECTED.width || params.height !== EXPECTED.height) {
        fail(`display geometry must be ${EXPECTED.width}x${EXPECTED.height}`);
    }
    if (params.bufferLines !== EXPECTED.bufferLines) {
        fail(`display bufferLines must be ${EXPECTED.bufferLines}`);
    }
    if (params.rgbFrequency !== EXPECTED.rgbFrequency) {
        fail(`display rgbFrequency must be ${EXPECTED.rgbFrequency}`);
    }

    const linesFlags = candidateIni.flags.filter((flag) => flag.startsWith('-DFASTBEE_RGB_BOUNCE_LINES='));
    const requireFlags = candidateIni.flags.filter((flag) => flag.startsWith('-DFASTBEE_RGB_REQUIRE_BOUNCE='));
    if (candidateIni.environment !== EXPECTED.environment) {
        fail(`candidate environment must be ${EXPECTED.environment}`);
    }
    if (linesFlags.length !== 1 || linesFlags[0] !== `-DFASTBEE_RGB_BOUNCE_LINES=${EXPECTED.bufferLines}`) {
        fail('candidate ini must set exactly one 16-line FASTBEE_RGB_BOUNCE_LINES macro');
    }
    if (requireFlags.length !== 1 || requireFlags[0] !== '-DFASTBEE_RGB_REQUIRE_BOUNCE=1') {
        fail('candidate ini must require the RGB bounce buffer');
    }
    for (const required of [
        'CONFIG_SPIRAM_XIP_FROM_PSRAM=y',
        'CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y',
        'CONFIG_SPIRAM_RODATA=y',
        'CONFIG_LCD_RGB_ISR_IRAM_SAFE=y',
        'CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y'
    ]) {
        if (!candidateIni.sdkLines.includes(required)) fail(`candidate ini is missing ${required}`);
    }
    if (candidateIni.sdkLines.some((line) => line === 'CONFIG_LCD_RGB_RESTART_IN_VSYNC=y')) {
        fail('candidate ini must keep CONFIG_LCD_RGB_RESTART_IN_VSYNC disabled');
    }

    // The backend allocates two 16-bit bounce rows per line: width * lines * 2
    // bytes * two buffers.  Keep the expression visible so a changed geometry
    // cannot silently retain the old 30,720-byte contract.
    const expectedBounceBytes = EXPECTED.width * EXPECTED.bufferLines * 2 * 2;
    if (build.environment !== EXPECTED.environment) fail('build report environment does not match candidate ini');
    if (build.exitCode !== 0) fail('build report must have exitCode=0');
    if (build.sourceStable !== true) fail('build report must prove sourceStable=true');
    if (build.sdkRecompiled !== false) fail('build report must prove sdkRecompiled=false');
    if (build.requestedBounceLines !== EXPECTED.bufferLines) fail('build report requestedBounceLines must be 16');
    if (build.requestedBounceBytes !== expectedBounceBytes) fail('build report requestedBounceBytes must be 30720');
    if (build.bounceRequired !== true) fail('build report must prove bounceRequired=true');
    if (build.lcdRgbRestartInVsync !== false) fail('build report must prove RGB restart-in-vsync is disabled');

    if (errors.length) throw new Error(errors.join('; '));
    return { display, expectedBounceBytes };
}

function clone(value) {
    return JSON.parse(JSON.stringify(value));
}

function validateGenericDefaults(source) {
    assert.match(source, /#ifndef FASTBEE_RGB_BOUNCE_LINES\s+#define FASTBEE_RGB_BOUNCE_LINES 4\b/s,
        'generic RGB backend default must remain four lines');
    assert.match(source, /#ifndef FASTBEE_RGB_REQUIRE_BOUNCE\s+#define FASTBEE_RGB_REQUIRE_BOUNCE 0\b/s,
        'generic RGB backend must keep direct fallback as its default');
}

function expectRejected(label, mutate, fixtures) {
    const broken = {
        bundle: clone(fixtures.bundle),
        candidateIni: clone(fixtures.candidateIni),
        build: clone(fixtures.build)
    };
    mutate(broken);
    assert.throws(
        () => validateTouchBuildContract(broken),
        /./,
        `${label} must be rejected by the touch RGB contract`
    );
}

function main() {
    const contract = readJson(CONTRACT_FILE);
    validateGenericDefaults(readText(RGB_BACKEND_SOURCE));
    const fixtures = {
        bundle: readJson(path.join(TOUCH_BUNDLE, 'peripherals.json')),
        contract,
        iniText: fs.existsSync(OPTIONAL_CANDIDATE_INI) ? readText(OPTIONAL_CANDIDATE_INI) : null,
        build: null
    };
    const contractCandidate = candidateFromContract(contract);
    const contractBuild = {
        environment: contract.environment,
        exitCode: 0,
        sourceStable: contract.sourceStable,
        sdkRecompiled: contract.sdkRecompiled,
        requestedBounceLines: contract.requestedBounceLines,
        requestedBounceBytes: contract.requestedBounceBytes,
        bounceRequired: contract.bounceRequired,
        lcdRgbRestartInVsync: contract.lcdRgbRestartInVsync
    };
    fixtures.candidateIni = contractCandidate;
    fixtures.build = contractBuild;
    // The tracked contract is always checked. Generated candidate ini/report
    // files are ignored build outputs, so validate them when present without
    // making ordinary host tests depend on a previous local build.
    const result = validateTouchBuildContract({
        bundle: fixtures.bundle,
        candidateIni: contractCandidate,
        build: contractBuild
    });
    if (fixtures.iniText) {
        validateTouchBuildContract({
            bundle: fixtures.bundle,
            candidateIni: parseCandidateIni(fixtures.iniText),
            build: contractBuild
        });
    }
    const reportPath = OPTIONAL_BUILD_REPORTS.find((filePath) => fs.existsSync(filePath));
    if (reportPath) {
        fixtures.build = readJson(reportPath);
        validateTouchBuildContract({
            bundle: fixtures.bundle,
            candidateIni: contractCandidate,
            build: fixtures.build
        });
    }
    if (process.argv.includes('--require-build-report') && !reportPath) {
        throw new Error('No generated RGB touch build report found; run a touch candidate build first');
    }

    expectRejected('32-line candidate', (broken) => {
        broken.candidateIni.flags = broken.candidateIni.flags.map((flag) =>
            flag.startsWith('-DFASTBEE_RGB_BOUNCE_LINES=') ? '-DFASTBEE_RGB_BOUNCE_LINES=32' : flag);
        broken.build.requestedBounceLines = 32;
        broken.build.requestedBounceBytes = 61440;
    }, fixtures);
    expectRejected('40-line candidate', (broken) => {
        broken.candidateIni.flags = broken.candidateIni.flags.map((flag) =>
            flag.startsWith('-DFASTBEE_RGB_BOUNCE_LINES=') ? '-DFASTBEE_RGB_BOUNCE_LINES=40' : flag);
        broken.build.requestedBounceLines = 40;
        broken.build.requestedBounceBytes = 76800;
    }, fixtures);
    expectRejected('direct-EDMA candidate', (broken) => {
        broken.candidateIni.flags = broken.candidateIni.flags.map((flag) =>
            flag.startsWith('-DFASTBEE_RGB_REQUIRE_BOUNCE=') ? '-DFASTBEE_RGB_REQUIRE_BOUNCE=0' : flag);
        broken.build.bounceRequired = false;
    }, fixtures);
    expectRejected('inconsistent byte count', (broken) => {
        broken.build.requestedBounceBytes = 30720 - 1;
    }, fixtures);
    expectRejected('runtime 32-line display setting', (broken) => {
        broken.bundle.peripherals.find((item) => item.type === 56).params.bufferLines = 32;
    }, fixtures);

    const checked = [
        'tracked contract',
        fixtures.iniText ? 'local candidate ini' : null,
        reportPath ? `build report ${path.basename(reportPath)}` : null
    ].filter(Boolean).join(' + ');
    console.log(`RGB touch build contract OK (${checked}): ${EXPECTED.width}x${EXPECTED.height}, ${EXPECTED.bufferLines} lines, ${result.expectedBounceBytes} bytes`);
}

if (require.main === module) main();

module.exports = {
    EXPECTED,
    parseCandidateIni,
    validateTouchBuildContract
};
