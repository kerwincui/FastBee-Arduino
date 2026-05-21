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

function getWebProfile() {
    if (
        process.argv.includes('--web-slim') ||
        process.argv.includes('--slim') ||
        process.argv.includes('--web-prod') ||
        process.argv.includes('--prod')
    ) {
        return 'slim';
    }
    const fromArg = readArgValue(['--web-profile=', '--profile=']);
    const fromEnv = process.env.FASTBEE_WEB_PROFILE || process.env.WEB_PROFILE || '';
    const profile = String(fromArg || fromEnv || 'full').trim().toLowerCase();
    if (profile === 'prod' || profile === 'production') return 'slim';
    if (profile === 'slim' || profile === 'lite') return 'slim';
    return 'full';
}

function isSlimWebProfile() {
    return getWebProfile() === 'slim';
}

function isProdWebProfile() {
    return isSlimWebProfile();
}

function isFullWebProfile() {
    return getWebProfile() === 'full';
}

module.exports = {
    getWebProfile,
    isSlimWebProfile,
    isProdWebProfile,
    isFullWebProfile
};
