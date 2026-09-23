'use strict';

const fs = require('fs');
const path = require('path');

const OMITTED_CONFIG_FILES = new Set(['auth.json', 'users.json']);
const BLANK_KEYS = new Set([
    'authcode',
    'apikey',
    'authheader',
    'clientid',
    'deviceid',
    'devicenum',
    'mqttsecret',
    'password',
    'passwordhash',
    'salt',
    'stapassword',
    'username',
    'userid',
    'wifipassword'
]);

function cloneJson(value) {
    return JSON.parse(JSON.stringify(value));
}

function scrubSecrets(value) {
    if (Array.isArray(value)) return value.map(scrubSecrets);
    if (!value || typeof value !== 'object') return value;

    Object.entries(value).forEach(([key, child]) => {
        const normalized = key.toLowerCase();
        if (normalized === 'appassword') {
            value[key] = 'admin123';
        } else if (BLANK_KEYS.has(normalized)) {
            value[key] = '';
        } else {
            value[key] = scrubSecrets(child);
        }
    });
    return value;
}

function sanitizeDocument(fileName, source) {
    const normalizedName = path.basename(fileName).toLowerCase();
    if (OMITTED_CONFIG_FILES.has(normalizedName)) return null;

    const result = scrubSecrets(cloneJson(source));
    if (normalizedName === 'device.json') {
        if (Object.prototype.hasOwnProperty.call(result, 'productNumber')) result.productNumber = 0;
        if (Object.prototype.hasOwnProperty.call(result, 'location')) result.location = '';
    }
    if (normalizedName === 'network.json') {
        result.staSSID = '';
        result.staPassword = '';
        if (Object.prototype.hasOwnProperty.call(result, 'networks')) result.networks = [];
    }
    if (normalizedName === 'protocol.json' && result.mqtt && typeof result.mqtt === 'object') {
        result.mqtt.enabled = false;
        result.mqtt.clientId = '';
        result.mqtt.username = '';
        result.mqtt.password = '';
        result.mqtt.mqttSecret = '';
        result.mqtt.authCode = '';
        if (Object.prototype.hasOwnProperty.call(result.mqtt, 'deviceNum')) result.mqtt.deviceNum = '';
        if (Object.prototype.hasOwnProperty.call(result.mqtt, 'userId')) result.mqtt.userId = '';
        if (Object.prototype.hasOwnProperty.call(result.mqtt, 'summary')) result.mqtt.summary = '';
    }
    if (normalizedName === 'protocol.json') {
        for (const source of result.weatherSources || []) {
            source.enabled = false;
            // A custom endpoint may embed vendor credentials in its query.
            for (const key of ['url', 'forecastUrl']) {
                if (typeof source[key] === 'string') source[key] = source[key].split('?')[0];
            }
        }
        for (const channel of result.remoteChannels || []) channel.enabled = false;
    }
    return result;
}

function sanitizeBundle(source) {
    const result = cloneJson(source);
    if (!Array.isArray(result.files)) throw new Error('FastBee bundle must contain a files array');

    result.files = result.files.flatMap((entry) => {
        const name = String(entry && entry.name || '');
        if (!name || typeof entry.content !== 'string') {
            throw new Error('Every FastBee bundle entry must have name and string content');
        }
        if (OMITTED_CONFIG_FILES.has(path.basename(name).toLowerCase())) return [];
        const parsed = JSON.parse(entry.content);
        const sanitized = sanitizeDocument(name, parsed);
        return [{ ...entry, content: `${JSON.stringify(sanitized, null, 2)}\n` }];
    });
    result.scope = 'selected';
    result.sanitized = true;
    delete result.exportedAt;
    return result;
}

function sanitizeJsonFile(inputPath) {
    const source = JSON.parse(fs.readFileSync(inputPath, 'utf8'));
    if (Array.isArray(source.files)) return sanitizeBundle(source);
    return sanitizeDocument(path.basename(inputPath), source);
}

function writeJson(filePath, value) {
    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    fs.writeFileSync(filePath, `${JSON.stringify(value, null, 2)}\n`, 'utf8');
}

function sanitizePath(inputPath, outputPath) {
    const input = path.resolve(inputPath);
    const output = path.resolve(outputPath);
    const stat = fs.statSync(input);

    if (stat.isFile()) {
        const sanitized = sanitizeJsonFile(input);
        if (sanitized === null) throw new Error(`Refusing to publish authentication file ${input}`);
        writeJson(output, sanitized);
        return [output];
    }
    if (!stat.isDirectory()) throw new Error(`Unsupported input path: ${input}`);

    const written = [];
    fs.readdirSync(input, { withFileTypes: true })
        .filter((entry) => entry.isFile() && entry.name.toLowerCase().endsWith('.json'))
        .sort((a, b) => a.name.localeCompare(b.name))
        .forEach((entry) => {
            if (OMITTED_CONFIG_FILES.has(entry.name.toLowerCase())) return;
            const sanitized = sanitizeJsonFile(path.join(input, entry.name));
            const destination = path.join(output, entry.name);
            writeJson(destination, sanitized);
            written.push(destination);
        });
    if (written.length === 0) throw new Error(`No publishable JSON files found in ${input}`);
    return written;
}

function parseArgs(argv) {
    const args = { input: '', output: '', inPlace: false };
    for (let index = 0; index < argv.length; index += 1) {
        const arg = argv[index];
        if (arg === '--input') args.input = argv[++index] || '';
        else if (arg === '--output') args.output = argv[++index] || '';
        else if (arg === '--in-place') args.inPlace = true;
        else throw new Error(`Unknown argument: ${arg}`);
    }
    if (!args.input) throw new Error('Usage: node scripts/sanitize-config-export.js --input <file-or-dir> (--output <path> | --in-place)');
    if (args.inPlace === Boolean(args.output)) throw new Error('Choose exactly one of --output or --in-place');
    if (args.inPlace) args.output = args.input;
    return args;
}

function main() {
    const args = parseArgs(process.argv.slice(2));
    const written = sanitizePath(args.input, args.output);
    console.log(`Sanitized FastBee configuration: files=${written.length}`);
}

if (require.main === module) {
    try {
        main();
    } catch (error) {
        console.error(`Configuration sanitization failed: ${error.message}`);
        process.exit(1);
    }
}

module.exports = {
    OMITTED_CONFIG_FILES,
    sanitizeBundle,
    sanitizeDocument,
    sanitizePath
};
