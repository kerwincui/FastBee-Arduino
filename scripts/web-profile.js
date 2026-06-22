'use strict';

function readArgValue(prefixes) {
    for (const arg of process.argv.slice(2)) {
        for (const prefix of prefixes) {
            if (arg.startsWith(prefix)) {
                return arg.slice(prefix.length);
            }
        }
    }
    return '';
}

function normalizeWebProfile(value) {
    const profile = String(value || '').trim().toLowerCase();
    if (!profile) return 'full';
    if (profile === 'prod' || profile === 'production') return 'lite';
    if (profile === 'lite') return 'lite';
    if (profile === 'std' || profile === 'standard') return 'standard';
    if (profile === 'full') return 'full';
    return 'full';
}

function getWebProfile() {
    if (
        process.argv.includes('--web-lite') ||
        process.argv.includes('--lite') ||
        process.argv.includes('--web-prod') ||
        process.argv.includes('--prod')
    ) {
        return 'lite';
    }
    if (
        process.argv.includes('--web-standard') ||
        process.argv.includes('--standard')
    ) {
        return 'standard';
    }
    if (
        process.argv.includes('--web-full') ||
        process.argv.includes('--full')
    ) {
        return 'full';
    }
    const fromArg = readArgValue(['--web-profile=', '--profile=']);
    const fromEnv = process.env.FASTBEE_WEB_PROFILE || process.env.WEB_PROFILE || '';
    return normalizeWebProfile(fromArg || fromEnv || 'full');
}

function isLiteWebProfile() {
    return getWebProfile() === 'lite';
}

function isStandardWebProfile() {
    return getWebProfile() === 'standard';
}

function isCompactWebProfile() {
    return isLiteWebProfile() || isStandardWebProfile();
}

function isProdWebProfile() {
    return isLiteWebProfile();
}

function isFullWebProfile() {
    return getWebProfile() === 'full';
}

module.exports = {
    getWebProfile,
    normalizeWebProfile,
    isLiteWebProfile,
    isStandardWebProfile,
    isCompactWebProfile,
    isProdWebProfile,
    isFullWebProfile
};
