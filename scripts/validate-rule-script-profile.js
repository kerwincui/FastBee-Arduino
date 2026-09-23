'use strict';

// Unified architecture has one web/runtime profile. Keep the rule-script
// feature controlled by FASTBEE_ENABLE_RULE_SCRIPT and reject stale profile
// compile switches that silently produce mismatched firmware/web images.
const fs = require('fs');
const path = require('path');
const root = path.resolve(__dirname, '..');
const platformio = fs.readFileSync(path.join(root, 'platformio.ini'), 'utf8');
const flags = fs.readFileSync(path.join(root, 'include', 'core', 'FeatureFlags.h'), 'utf8');
const forbidden = [/FASTBEE_WEB_PROFILE_(?:LITE|STANDARD|FULL)/i];
for (const re of forbidden) {
    if (re.test(platformio) || re.test(flags)) {
        console.error(`rule-script profile validation failed: stale switch ${re}`);
        process.exit(1);
    }
}
if (!platformio.includes('FASTBEE_ENABLE_RULE_SCRIPT') || !flags.includes('FASTBEE_ENABLE_RULE_SCRIPT')) {
    console.error('rule-script profile validation failed: canonical feature flag is missing');
    process.exit(1);
}
console.log('rule-script profile validation passed: unified web profile and canonical feature flag');
