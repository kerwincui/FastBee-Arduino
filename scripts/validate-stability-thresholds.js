'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const THRESHOLDS_PATH = path.join(ROOT_DIR, 'scripts', 'device-stability-thresholds.json');
const PROFILES = ['lite', 'standard', 'full'];
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

function validateProfileThresholds(presetName, profile, thresholds, issues) {
    const label = `presets.${presetName}.profiles.${profile}`;
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

    if (profile === 'full') {
        if (thresholds.minPsramFreeBytes <= 0) {
            issues.push(`${label}.minPsramFreeBytes: full profile must enforce PSRAM free minimum`);
        }
        if (thresholds.maxPsramFreeDropBytes <= 0) {
            issues.push(`${label}.maxPsramFreeDropBytes: full profile must enforce PSRAM drop maximum`);
        }
    } else {
        if (thresholds.minPsramFreeBytes !== 0) {
            issues.push(`${label}.minPsramFreeBytes: non-full profiles must not require PSRAM`);
        }
        if (thresholds.maxPsramFreeDropBytes !== 0) {
            issues.push(`${label}.maxPsramFreeDropBytes: non-full profiles must not require PSRAM`);
        }
    }
}

function checkMonotonic(profiles, field, direction, issues) {
    const lite = profiles.lite;
    const standard = profiles.standard;
    const full = profiles.full;
    if (!lite || !standard || !full) return;
    if (!(field in lite) || !(field in standard) || !(field in full)) return;

    const l = lite[field], s = standard[field], f = full[field];
    if (direction === 'asc') {
        if (s < l) issues.push(`release: standard.${field} (${s}) must be >= lite (${l})`);
        if (f < s) issues.push(`release: full.${field} (${f}) must be >= standard (${s})`);
    } else {
        if (s > l) issues.push(`release: standard.${field} (${s}) must be <= lite (${l})`);
        if (f > s) issues.push(`release: full.${field} (${f}) must be <= standard (${s})`);
    }
}

function validateReleaseOrdering(releaseProfiles, issues) {
    const lite = releaseProfiles.lite;
    const standard = releaseProfiles.standard;
    const full = releaseProfiles.full;
    if (!lite || !standard || !full) return;

    // Minimums: full >= standard >= lite (more features need more memory)
    checkMonotonic(releaseProfiles, 'minHeapFreeBytes', 'asc', issues);
    checkMonotonic(releaseProfiles, 'minHeapMaxAllocBytes', 'asc', issues);
    checkMonotonic(releaseProfiles, 'minPsramFreeBytes', 'asc', issues);

    // Drop allowances: full >= standard >= lite (more features tolerate more drift)
    checkMonotonic(releaseProfiles, 'maxHeapFreeDropBytes', 'asc', issues);
    checkMonotonic(releaseProfiles, 'maxHeapMaxAllocDropBytes', 'asc', issues);
    checkMonotonic(releaseProfiles, 'maxPsramFreeDropBytes', 'asc', issues);

    // Latency: full >= standard >= lite (full builds may be slower under load)
    checkMonotonic(releaseProfiles, 'maxP95LatencyMs', 'asc', issues);

    // Failure budgets: should be consistent (all 0 for release), but if relaxed, full >= standard >= lite
    checkMonotonic(releaseProfiles, 'maxFailureRatePercent', 'asc', issues);
    checkMonotonic(releaseProfiles, 'maxEndpointFailureRatePercent', 'asc', issues);
    checkMonotonic(releaseProfiles, 'maxConsecutiveFailures', 'asc', issues);
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
        const profiles = preset && preset.profiles;
        if (!profiles || typeof profiles !== 'object' || Array.isArray(profiles)) {
            issues.push(`presets.${presetName}.profiles: must be an object`);
            return;
        }

        const actualProfiles = Object.keys(profiles);
        const extraProfiles = actualProfiles.filter((profile) => !PROFILES.includes(profile));
        const missingProfiles = PROFILES.filter((profile) => !actualProfiles.includes(profile));
        if (extraProfiles.length > 0) {
            issues.push(`presets.${presetName}.profiles: unexpected ${sorted(extraProfiles).join(', ')}`);
        }
        if (missingProfiles.length > 0) {
            issues.push(`presets.${presetName}.profiles: missing ${missingProfiles.join(', ')}`);
        }

        PROFILES.forEach((profile) => validateProfileThresholds(presetName, profile, profiles[profile], issues));
        if (presetName === 'release') {
            validateReleaseOrdering(profiles, issues);
        }
    });

    if (issues.length > 0) {
        console.error('FastBee stability threshold validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }

    const presetCount = Object.keys(doc.presets || {}).length;
    console.log(`FastBee stability thresholds OK: presets=${presetCount}, profiles=${PROFILES.length}`);
}

main();
