'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const THRESHOLDS_PATH = path.join(ROOT_DIR, 'scripts', 'device-stability-thresholds.json');
const REQUIRED_FIELDS = [
    'maxFailureRatePercent',
    'maxP95LatencyMs',
    'maxConsecutiveFailures',
    'maxEndpointFailureRatePercent',
    'maxUptimeResetCount',
    'minHeapFreeBytes',
    'minHeapMaxAllocBytes',
    'maxHeapFreeDropBytes',
    'maxHeapMaxAllocDropBytes',
    'minPsramFreeBytes',
    'maxPsramFreeDropBytes'
];

function readJson(filePath) {
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function sorted(values) {
    return Array.from(values).sort();
}

function validateHardwareThresholds(presetName, hardware, thresholds, issues) {
    const label = `presets.${presetName}.${hardware}`;
    if (!thresholds || typeof thresholds !== 'object' || Array.isArray(thresholds)) {
        issues.push(`${label}: must be an object`);
        return;
    }

    REQUIRED_FIELDS.forEach((field) => {
        if (!Object.prototype.hasOwnProperty.call(thresholds, field)) {
            issues.push(`${label}: missing ${field}`);
            return;
        }
        const value = thresholds[field];
        if (typeof value !== 'number' || !Number.isFinite(value)) {
            issues.push(`${label}.${field}: must be a finite number`);
        }
        if (field !== 'maxUptimeResetCount' && value < 0) {
            issues.push(`${label}.${field}: must be >= 0`);
        }
    });

    if (thresholds.maxFailureRatePercent < 0 || thresholds.maxFailureRatePercent > 100) {
        issues.push(`${label}.maxFailureRatePercent: must be between 0 and 100`);
    }
    if (thresholds.maxEndpointFailureRatePercent < 0 || thresholds.maxEndpointFailureRatePercent > 100) {
        issues.push(`${label}.maxEndpointFailureRatePercent: must be between 0 and 100`);
    }
    if (thresholds.maxUptimeResetCount < 0) {
        issues.push(`${label}.maxUptimeResetCount: release presets must require uptime reset checks`);
    }
    if (thresholds.maxP95LatencyMs <= 0) {
        issues.push(`${label}.maxP95LatencyMs: must be > 0`);
    }
    if (thresholds.minHeapFreeBytes <= 0) {
        issues.push(`${label}.minHeapFreeBytes: must be > 0`);
    }
    if (thresholds.minHeapMaxAllocBytes <= 0) {
        issues.push(`${label}.minHeapMaxAllocBytes: must be > 0`);
    }
    if (thresholds.maxHeapFreeDropBytes <= 0) {
        issues.push(`${label}.maxHeapFreeDropBytes: must be > 0`);
    }
    if (thresholds.maxHeapMaxAllocDropBytes <= 0) {
        issues.push(`${label}.maxHeapMaxAllocDropBytes: must be > 0`);
    }

    if (hardware === 'psramThresholds') {
        if (thresholds.minPsramFreeBytes <= 0) {
            issues.push(`${label}.minPsramFreeBytes: PSRAM hardware must enforce PSRAM free minimum`);
        }
        if (thresholds.maxPsramFreeDropBytes <= 0) {
            issues.push(`${label}.maxPsramFreeDropBytes: PSRAM hardware must enforce PSRAM drop maximum`);
        }
    } else {
        if (thresholds.minPsramFreeBytes !== 0) {
            issues.push(`${label}.minPsramFreeBytes: internal DRAM hardware must not require PSRAM`);
        }
        if (thresholds.maxPsramFreeDropBytes !== 0) {
            issues.push(`${label}.maxPsramFreeDropBytes: internal DRAM hardware must not require PSRAM`);
        }
    }
}

function main() {
    const doc = readJson(THRESHOLDS_PATH);
    const issues = [];

    if (!Number.isInteger(doc.version) || doc.version < 1) {
        issues.push('version must be a positive integer');
    }
    if (!doc.presets || typeof doc.presets !== 'object' || Array.isArray(doc.presets)) {
        issues.push('presets must be an object');
    }
    if (!doc.presets || !doc.presets.release) {
        issues.push('missing release preset');
    }

    Object.entries(doc.presets || {}).forEach(([presetName, preset]) => {
        if (!/^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(presetName)) {
            issues.push(`invalid preset name '${presetName}'`);
        }
        if (preset.profiles) issues.push(`presets.${presetName}: product editions are not supported`);
        validateHardwareThresholds(presetName, 'thresholds', preset.thresholds, issues);
        validateHardwareThresholds(presetName, 'psramThresholds', preset.psramThresholds, issues);
        if (preset.thresholds && preset.psramThresholds) {
            for (const field of ['minHeapFreeBytes', 'minHeapMaxAllocBytes']) {
                if (preset.psramThresholds[field] < preset.thresholds[field]) {
                    issues.push(`presets.${presetName}: PSRAM hardware must not weaken ${field}`);
                }
            }
        }
    });

    if (issues.length > 0) {
        console.error('FastBee stability threshold validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }

    const presetCount = Object.keys(doc.presets || {}).length;
    console.log(`FastBee stability thresholds OK: presets=${presetCount}, hardwareGuards=2`);
}

main();
