'use strict';

const { spawnSync } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { auditRepository } = require('./audit-repository-retention');

const ROOT = path.resolve(__dirname, '..');
const REQUIRED_IGNORES = [
    '/artifacts/',
    '/tmp/',
    '/test-results/',
    'test/browser/reports/',
    'test/reports/',
    '/.device-config-backups/',
    'docs/.device-config-backups/'
];
const FORBIDDEN_TRACKED = [
    /^tools\//,
    /^artifacts\//,
    /^test-results\//,
    /^test\/browser\/reports\//,
    /^test\/reports\//,
    /^\.device-config-backups\//,
    /^docs\/\.device-config-backups\//,
    /^scripts\/build-size-report\.json$/,
    /^docs\/superpowers\//,
    /^\.superpowers\//,
    /^docs\/knowledge-experience-index\.md$/,
    /^DEBUG-TEST-GUIDE\.md$/,
    /^test\/browser\/debug_[^/]+\.js$/,
    /^scripts\/(?:deploy-puzhong-esp32|flash-c3|flash-c6(?:-final|-recovery|-v2)?|flash_full)\.(?:ps1|py)$/,
    /(^|\/)[^/]+\.log$/i,
    /(^|\/)[^/]*\.cookies$/,
    /(^|\/)[^/]*-(?:cookie|cookies|headers|metrics|mqtt-status)\.(?:txt|json)$/
];
const PUBLIC_CONFIG_PATTERNS = [
    /^docs\/.*\.json$/i,
    /^data\/config\/[^/]+\.json$/i,
    /^scripts\/[^/]+\.example\.json$/i
];
const PRIVATE_CONFIG_PREFIXES = ['.device-config-backups/', 'docs/.device-config-backups/'];
const STA_CREDENTIAL_KEYS = new Set(['stassid', 'stapassword', 'wifissid', 'wifipassword']);
const MQTT_CREDENTIAL_KEYS = new Set([
    'clientid', 'username', 'password', 'mqttsecret', 'authcode', 'devicenum', 'userid',
    'clientsecret', 'accesstoken', 'token'
]);
const DEVICE_IDENTITY_KEYS = new Set(['deviceid', 'productnumber', 'productid', 'userid']);

function readJson(relativePath) {
    return JSON.parse(fs.readFileSync(path.join(ROOT, relativePath), 'utf8'));
}

function trackedFiles() {
    const result = spawnSync('git', ['ls-files', '-z'], { cwd: ROOT, encoding: 'utf8', windowsHide: true });
    if (result.error || result.status !== 0) return [];
    return result.stdout
        .split('\0')
        .filter(Boolean)
        .map((item) => item.replace(/\\/g, '/'))
        .filter((item) => fs.existsSync(path.join(ROOT, item)));
}

function committableConfigJsonFiles(root = ROOT) {
    const result = spawnSync(
        'git',
        ['ls-files', '-z', '--cached', '--others', '--exclude-standard'],
        { cwd: root, encoding: 'utf8', windowsHide: true }
    );
    if (result.error || result.status !== 0) {
        throw new Error('unable to enumerate Git-committable JSON files');
    }
    return result.stdout
        .split('\0')
        .filter(Boolean)
        .map((item) => item.replace(/\\/g, '/'))
        .filter((item) => !PRIVATE_CONFIG_PREFIXES.some((prefix) => item.startsWith(prefix)))
        .filter((item) => PUBLIC_CONFIG_PATTERNS.some((pattern) => pattern.test(item)))
        .filter((item) => fs.existsSync(path.join(root, item)))
        .sort();
}

function hasCredentialValue(value) {
    if (value === null || value === undefined || value === false) return false;
    if (typeof value === 'string') return value.trim() !== '';
    if (typeof value === 'number') return value !== 0;
    if (Array.isArray(value)) return value.length > 0;
    if (typeof value === 'object') return Object.keys(value).length > 0;
    return true;
}

function normalizeKey(key) {
    return String(key).replace(/[^a-z0-9]/gi, '').toLowerCase();
}

function isFactoryUsersDocument(label) {
    return !label.includes('::') && label.replace(/\\/g, '/').toLowerCase() === 'data/config/users.json';
}

function scanCredentialDocument(label, document, issues, options = {}, context = {}) {
    const factoryAPPassword = String(options.factoryAPPassword || '');
    const logicalName = String(context.logicalName || path.posix.basename(label.split('::').pop())).toLowerCase();
    const deviceDocument = logicalName === 'device.json';
    const factoryUsersDocument = isFactoryUsersDocument(label);

    function addIssue(field, category) {
        issues.push(`${label}: non-empty ${category} field '${field}' is not allowed in public configuration`);
    }

    function walk(value, state = {}) {
        if (Array.isArray(value)) {
            value.forEach((item) => walk(item, state));
            return;
        }
        if (!value || typeof value !== 'object') return;

        Object.entries(value).forEach(([key, child]) => {
            const normalized = normalizeKey(key);
            const mqttContext = Boolean(state.mqtt || normalized === 'mqtt');
            const savedNetworkContext = Boolean(state.savedNetwork || normalized === 'networks');

            if (STA_CREDENTIAL_KEYS.has(normalized) && hasCredentialValue(child)) {
                addIssue(key, 'STA credential');
            } else if (savedNetworkContext && (normalized === 'ssid' || normalized === 'password') && hasCredentialValue(child)) {
                addIssue(key, 'saved STA credential');
            } else if (normalized === 'appassword' && hasCredentialValue(child) && String(child) !== factoryAPPassword) {
                addIssue(key, 'non-factory AP credential');
            } else if (mqttContext && MQTT_CREDENTIAL_KEYS.has(normalized) && hasCredentialValue(child)) {
                addIssue(key, 'MQTT credential');
            } else if (deviceDocument && DEVICE_IDENTITY_KEYS.has(normalized) && hasCredentialValue(child)) {
                const isZeroProduct = normalized.startsWith('product') && Number(child) === 0;
                if (!isZeroProduct) addIssue(key, 'device identity');
            } else if ((normalized === 'passwordhash' || normalized === 'salt') &&
                       hasCredentialValue(child) && !factoryUsersDocument) {
                addIssue(key, 'authentication material');
            } else if (normalized === 'password' && hasCredentialValue(child)) {
                addIssue(key, 'credential');
            }

            walk(child, { mqtt: mqttContext, savedNetwork: savedNetworkContext });
        });
    }

    walk(document);

    if (document && Array.isArray(document.files)) {
        document.files.forEach((entry, index) => {
            if (!entry || typeof entry.content !== 'string') return;
            const entryName = typeof entry.name === 'string' && entry.name ? entry.name : `entry-${index}.json`;
            const embeddedLabel = `${label}::${entryName}`;
            let embedded;
            try {
                embedded = JSON.parse(entry.content);
            } catch (error) {
                issues.push(`${embeddedLabel}: bundle content is not valid JSON`);
                return;
            }
            scanCredentialDocument(embeddedLabel, embedded, issues, options, { logicalName: entryName });
        });
    }
}

function validatePublicConfigCredentials(files, issues, options = {}) {
    const root = path.resolve(options.root || ROOT);
    files.forEach((relativePath) => {
        let document;
        try {
            document = JSON.parse(fs.readFileSync(path.join(root, relativePath), 'utf8'));
        } catch (error) {
            issues.push(`${relativePath}: public configuration is not valid JSON`);
            return;
        }
        scanCredentialDocument(relativePath, document, issues, options);
    });
}

function readFactoryAPPassword(root = ROOT) {
    const constantsPath = path.join(root, 'include', 'core', 'SystemConstants.h');
    if (!fs.existsSync(constantsPath)) return '';
    const match = fs.readFileSync(constantsPath, 'utf8').match(/DEFAULT_AP_PASSWORD\s*=\s*"([^"]+)"/);
    return match ? match[1] : '';
}

function validateForbiddenTracked(files, issues) {
    files.forEach((file) => {
        if (FORBIDDEN_TRACKED.some((pattern) => pattern.test(file))) {
            issues.push(`${file}: generated, private, or internal file must not be tracked`);
        }
    });
}

function validateFactoryConfig(issues) {
    const device = readJson('data/config/device.json');
    const network = readJson('data/config/network.json');
    const protocol = readJson('data/config/protocol.json');
    const peripherals = readJson('data/config/peripherals.json');
    const exec = readJson('data/config/periph_exec.json');

    if (String(device.deviceId || '') || Number(device.productNumber || 0) !== 0 || String(device.userId || '')) {
        issues.push('data/config/device.json: factory image must not contain a live platform identity');
    }
    if (String(network.staSSID || '') || String(network.staPassword || '') || (network.networks || []).length > 0) {
        issues.push('data/config/network.json: factory image must not contain STA credentials');
    }
    const mqtt = protocol.mqtt || {};
    if (mqtt.enabled !== true) {
        issues.push('data/config/protocol.json: factory MQTT must default to enabled');
    }
    if (mqtt.scheme !== 'mqtt' || mqtt.server !== 'iot.fastbee.cn' || Number(mqtt.port) !== 1883) {
        issues.push('data/config/protocol.json: factory MQTT endpoint must be mqtt://iot.fastbee.cn:1883');
    }
    ['clientId', 'username', 'password', 'mqttSecret', 'authCode', 'deviceNum', 'userId'].forEach((key) => {
        if (String(mqtt[key] || '')) issues.push(`data/config/protocol.json: mqtt.${key} must be blank`);
    });
    if ((peripherals.peripherals || []).some((item) => item?.enabled === true)) {
        issues.push('data/config/peripherals.json: factory peripherals must be disabled or absent');
    }
    if ((exec.rules || []).some((item) => item?.enabled === true)) {
        issues.push('data/config/periph_exec.json: factory execution rules must be disabled or absent');
    }
    if (fs.existsSync(path.join(ROOT, 'data', 'config', 'auth.json'))) {
        issues.push('data/config/auth.json: runtime authentication state must not exist in source staging');
    }
}

function validateNoEmbeddedStaPassword(issues) {
    const files = [
        'scripts/deploy-config-bundle.ps1',
        'scripts/deploy-multi-device.ps1',
        'scripts/run-browser-tests.ps1',
        'scripts/audit-live-device-config.mjs'
    ];
    const literalPattern = /(?:staPassword|WiFiPassword)\s*[:=]\s*['"]([^'"]+)['"]/gi;
    files.forEach((relativePath) => {
        const fullPath = path.join(ROOT, relativePath);
        if (!fs.existsSync(fullPath)) return;
        const text = fs.readFileSync(fullPath, 'utf8');
        let match = literalPattern.exec(text);
        while (match) {
            issues.push(`${relativePath}: embedded STA/WiFi password literal is not allowed`);
            match = literalPattern.exec(text);
        }
    });
}

function validateNoCredentialDefaults(issues) {
    const browserFixture = fs.readFileSync(path.join(ROOT, 'test', 'browser', 'fixtures', 'base.fixture.ts'), 'utf8');
    const environmentDefault = /(?:WIFI_PASSWORD|MQTT_PASSWORD)\s*\|\|\s*['"]([^'"]+)['"]/g;
    let match = environmentDefault.exec(browserFixture);
    while (match) {
        issues.push('test/browser/fixtures/base.fixture.ts: credential environment variables must not have non-empty defaults');
        match = environmentDefault.exec(browserFixture);
    }

    const constants = fs.readFileSync(path.join(ROOT, 'include', 'core', 'SystemConstants.h'), 'utf8');
    const firmwareDefault = /DEFAULT_MQTT_(?:USER|PASSWORD)\s*=\s*"([^"]+)"/g;
    match = firmwareDefault.exec(constants);
    while (match) {
        issues.push('include/core/SystemConstants.h: MQTT credentials must not have non-empty firmware defaults');
        match = firmwareDefault.exec(constants);
    }
}

function validatePortableScripts(issues) {
    trackedFiles()
        .filter((file) => /^scripts\/.*\.(?:js|mjs|py|ps1)$/i.test(file))
        .forEach((file) => {
            const text = fs.readFileSync(path.join(ROOT, file), 'utf8');
            if (/[A-Za-z]:[\\/](?:Users|project)[\\/]/i.test(text)) {
                issues.push(`${file}: machine-specific absolute path is not allowed`);
            }
        });
}

function validateGeneratedOutputLocations(issues) {
    const staleReport = path.join(ROOT, 'scripts', 'build-size-report.json');
    if (fs.existsSync(staleReport)) {
        issues.push('scripts/build-size-report.json: generated size report must live under ignored artifacts/');
    }

    const artifactScript = fs.readFileSync(path.join(ROOT, 'scripts', 'build-all-artifacts.ps1'), 'utf8');
    if (!artifactScript.includes('artifacts\\build-size-report.json') ||
        artifactScript.includes('scripts\\build-size-report.json')) {
        issues.push('scripts/build-all-artifacts.ps1: size report output must stay under artifacts/');
    }
}

function hashJsonDirectory(directory) {
    const files = fs.readdirSync(directory, { withFileTypes: true })
        .filter((entry) => entry.isFile() && entry.name.toLowerCase().endsWith('.json'))
        .map((entry) => entry.name)
        .sort();
    const hash = crypto.createHash('sha256');
    files.forEach((name) => {
        hash.update(`${name}\n`, 'utf8');
        hash.update(fs.readFileSync(path.join(directory, name)));
        hash.update('\n', 'utf8');
    });
    return { files, sha256: hash.digest('hex') };
}

function validatePrivateBackups(issues, backupsRoot = path.join(ROOT, 'docs', '.device-config-backups')) {
    if (!fs.existsSync(backupsRoot)) return;

    const snapshots = fs.readdirSync(backupsRoot, { withFileTypes: true })
        .filter((entry) => entry.isDirectory())
        .map((entry) => path.join(backupsRoot, entry.name));

    snapshots.forEach((snapshot) => {
        const manifestPath = path.join(snapshot, 'manifest.json');
        const label = path.relative(ROOT, snapshot).replace(/\\/g, '/');
        if (!fs.existsSync(manifestPath)) {
            issues.push(`${label}: private backup snapshot is missing manifest.json`);
            return;
        }

        let manifest;
        try {
            manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
        } catch (error) {
            issues.push(`${label}/manifest.json: invalid JSON (${error.message})`);
            return;
        }
        if (manifest.private !== true || manifest.trackedByGit !== false) {
            issues.push(`${label}/manifest.json: backup must be marked private and untracked`);
        }

        const devices = Array.isArray(manifest.devices) ? manifest.devices : [];
        const environments = new Set(devices.map((device) => String(device.environment || '')));
        const expectedEnvironments = new Set(
            Array.isArray(manifest.expectedEnvironments)
                ? manifest.expectedEnvironments.map((environment) => String(environment))
                : environments
        );
        expectedEnvironments.forEach((environment) => {
            if (!environments.has(environment)) issues.push(`${label}/manifest.json: missing ${environment}`);
        });
        if (devices.length === 0 || devices.length !== expectedEnvironments.size || environments.size !== devices.length) {
            issues.push(`${label}/manifest.json: device entries must be non-empty, unique, and match expectedEnvironments`);
        }

        devices.forEach((device) => {
            const environment = String(device.environment || '<unknown>');
            const relativeSource = String(device.path || '');
            const sourcePath = path.resolve(snapshot, relativeSource);
            if (!relativeSource || (!sourcePath.startsWith(`${snapshot}${path.sep}`))) {
                issues.push(`${label}/manifest.json: invalid source path for ${environment}`);
            } else if (!fs.existsSync(sourcePath)) {
                issues.push(`${label}/manifest.json: source path is missing for ${environment}`);
            } else if (fs.statSync(sourcePath).isDirectory()) {
                const sourceDigest = hashJsonDirectory(sourcePath);
                if (Number(device.jsonFiles) !== sourceDigest.files.length) {
                    issues.push(`${label}/manifest.json: ${environment} jsonFiles does not match ${sourceDigest.files.length}`);
                }
                if (String(device.sourceSha256 || '').toLowerCase() !== sourceDigest.sha256) {
                    issues.push(`${label}/manifest.json: ${environment} sourceSha256 does not match backup contents`);
                }
            } else {
                const sourceSha256 = crypto.createHash('sha256').update(fs.readFileSync(sourcePath)).digest('hex');
                if (Number(device.jsonFiles) !== 1) {
                    issues.push(`${label}/manifest.json: ${environment} file source must declare jsonFiles=1`);
                }
                if (String(device.fileSha256 || '').toLowerCase() !== sourceSha256) {
                    issues.push(`${label}/manifest.json: ${environment} fileSha256 does not match backup contents`);
                }
                try {
                    JSON.parse(fs.readFileSync(sourcePath, 'utf8'));
                } catch (error) {
                    issues.push(`${label}/${relativeSource}: invalid JSON (${error.message})`);
                }
            }

            if (device.sourceBundle) {
                const bundlePath = path.resolve(snapshot, String(device.sourceBundle));
                if (!bundlePath.startsWith(`${snapshot}${path.sep}`) || !fs.existsSync(bundlePath)) {
                    issues.push(`${label}/manifest.json: sourceBundle is missing or invalid for ${environment}`);
                } else {
                    const bundleSha256 = crypto.createHash('sha256').update(fs.readFileSync(bundlePath)).digest('hex');
                    if (String(device.sourceBundleSha256 || '').toLowerCase() !== bundleSha256) {
                        issues.push(`${label}/manifest.json: ${environment} sourceBundleSha256 does not match backup contents`);
                    }
                }
            }

            const relativeDeployed = String(device.deployedPath || '');
            const deployedPath = path.resolve(snapshot, relativeDeployed);
            if (!relativeDeployed || (!deployedPath.startsWith(`${snapshot}${path.sep}`))) {
                issues.push(`${label}/manifest.json: invalid deployedPath for ${environment}`);
                return;
            }
            if (!fs.existsSync(deployedPath) || !fs.statSync(deployedPath).isDirectory()) {
                issues.push(`${label}/manifest.json: deployedPath is missing for ${environment}`);
                return;
            }

            const digest = hashJsonDirectory(deployedPath);
            digest.files.forEach((name) => {
                try {
                    JSON.parse(fs.readFileSync(path.join(deployedPath, name), 'utf8'));
                } catch (error) {
                    issues.push(`${label}/${relativeDeployed}/${name}: invalid JSON (${error.message})`);
                }
            });
            if (Number(device.deployedJsonFiles) !== digest.files.length) {
                issues.push(`${label}/manifest.json: ${environment} deployedJsonFiles does not match ${digest.files.length}`);
            }
            if (String(device.deployedSha256 || '').toLowerCase() !== digest.sha256) {
                issues.push(`${label}/manifest.json: ${environment} deployedSha256 does not match backup contents`);
            }
        });
    });
}

function main() {
    const issues = [];
    const ignoreText = fs.readFileSync(path.join(ROOT, '.gitignore'), 'utf8');
    REQUIRED_IGNORES.forEach((pattern) => {
        if (!ignoreText.split(/\r?\n/).includes(pattern)) issues.push(`.gitignore: missing ${pattern}`);
    });

    validateForbiddenTracked(trackedFiles(), issues);
    validateFactoryConfig(issues);
    validateNoEmbeddedStaPassword(issues);
    validateNoCredentialDefaults(issues);
    validatePortableScripts(issues);
    validateGeneratedOutputLocations(issues);
    validatePrivateBackups(issues);
    try {
        const retention = auditRepository();
        retention.issues.forEach((issue) => issues.push(`retention: ${issue}`));
    } catch (error) {
        issues.push(`repository retention audit failed: ${error.message}`);
    }
    try {
        validatePublicConfigCredentials(committableConfigJsonFiles(), issues, {
            factoryAPPassword: readFactoryAPPassword()
        });
    } catch (error) {
        issues.push(`public configuration credential scan failed: ${error.message}`);
    }

    if (issues.length > 0) {
        console.error('FastBee project hygiene validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }
    console.log('FastBee project hygiene OK: factory config sanitized, generated output excluded, retention classified');
}

if (require.main === module) main();

module.exports = {
    committableConfigJsonFiles,
    hashJsonDirectory,
    scanCredentialDocument,
    validateForbiddenTracked,
    validatePrivateBackups,
    validatePublicConfigCredentials
};
