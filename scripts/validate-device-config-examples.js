'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const REQUIRED_FILES = ['device.json', 'peripherals.json', 'periph_exec.json', 'protocol.json'];
const NON_PERIPHERAL_ACTIONS = new Set([6, 7, 8, 9, 15, 18, 21, 22, 23, 31, 32]);
const CONFIGS = [
    {
        environment: 'esp32c3-F4R0', port: 'COM4', ip: '192.168.5.55',
        source: 'docs/esp32c3-deployment-config.json',
        peripherals: { dht11: [38, [10]], led: [12, [8]], oled: [36, [5, 6]] }
    },
    {
        environment: 'esp32c6-F4R0', port: 'COM11', ip: '192.168.5.19',
        source: 'docs/c6-rgb-led-f4r0',
        peripherals: { rgb_led: [45, [8]] }
    },
    {
        environment: 'esp32-F4R0', port: 'COM16', ip: '192.168.5.21',
        source: 'docs/puzhong-esp32-config.json',
        peripherals: { adc_pot: [26, [34]], relay: [12, [33]] }
    },
    {
        environment: 'esp32s3-F16R8', port: 'COM15', ip: '192.168.5.22',
        source: 'docs/ecm50a-s3-f16r8',
        peripherals: { ai_soil_temp: [26, [7]], ai_soil_humidity: [26, [6]], di1: [11, [4]], di2: [11, [5]], do1: [12, [15]], do2: [12, [16]], rs485: [1, [18, 17]] },
        modbus: {
            peripheralId: 'rs485', tasks: 5,
            devices: ['modbus_relay']
        }
    },
    {
        environment: 'esp32s3-F8R0', port: 'COM7', ip: '192.168.5.67',
        source: 'docs/waveshare-esp32s3-relay6ch',
        peripherals: {
            relay1: [12, [1]], relay2: [12, [2]], relay3: [12, [41]],
            relay4: [12, [42]], relay5: [12, [45]], relay6: [12, [46]],
            rgb: [45, [38]], rs485: [1, [18, 17]]
        },
        modbus: { peripheralId: 'rs485', tasks: 1, devices: ['modbus_th'] }
    }
];

function readJson(filePath) {
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function stableJsonStringify(value) {
    if (Array.isArray(value)) {
        return `[${value.map((item) => stableJsonStringify(item)).join(',')}]`;
    }
    if (value !== null && typeof value === 'object') {
        return `{${Object.keys(value).sort().map((key) =>
            `${JSON.stringify(key)}:${stableJsonStringify(value[key])}`
        ).join(',')}}`;
    }
    return JSON.stringify(value);
}

function fingerprintDocuments(documents) {
    const hash = crypto.createHash('sha256');
    [...documents.keys()].sort().forEach((name) => {
        hash.update(`${name}\n`, 'utf8');
        hash.update(stableJsonStringify(documents.get(name)), 'utf8');
        hash.update('\n', 'utf8');
    });
    return hash.digest('hex');
}

function loadConfigSet(relativeSource) {
    const sourcePath = path.join(ROOT, relativeSource);
    if (!fs.existsSync(sourcePath)) throw new Error(`${relativeSource}: configuration source does not exist`);
    const stat = fs.statSync(sourcePath);
    const documents = new Map();
    if (stat.isFile()) {
        const root = readJson(sourcePath);
        if (root.type !== 'fastbee-config-bundle' || !Array.isArray(root.files)) {
            throw new Error(`${relativeSource}: expected a FastBee configuration bundle`);
        }
        if (root.sanitized !== true) throw new Error(`${relativeSource}: public bundle must be marked sanitized=true`);
        root.files.forEach((entry) => {
            if (documents.has(entry.name)) throw new Error(`${relativeSource}: duplicate ${entry.name}`);
            documents.set(entry.name, JSON.parse(entry.content));
        });
    } else if (stat.isDirectory()) {
        fs.readdirSync(sourcePath)
            .filter((name) => name.toLowerCase().endsWith('.json'))
            .sort()
            .forEach((name) => {
                const text = fs.readFileSync(path.join(sourcePath, name), 'utf8');
                documents.set(name, JSON.parse(text));
            });
    } else {
        throw new Error(`${relativeSource}: unsupported configuration source type`);
    }
    return {
        documents,
        fingerprint: fingerprintDocuments(documents)
    };
}

function validatePublicIdentity(config, docs, issues) {
    const device = docs.get('device.json') || {};
    const network = docs.get('network.json');
    const protocol = docs.get('protocol.json') || {};
    const mqtt = protocol.mqtt || {};
    const label = config.source;

    if (String(device.deviceId || '') !== '' || Number(device.productNumber || 0) !== 0 || String(device.userId || '') !== '') {
        issues.push(`${label}: public example must not contain a live device identity`);
    }
    if (network) {
        if (String(network.staSSID || '') !== '' || String(network.staPassword || '') !== '') {
            issues.push(`${label}: public example must not contain STA credentials`);
        }
        if (Array.isArray(network.networks) && network.networks.length > 0) {
            issues.push(`${label}: public example must not contain remembered WiFi networks`);
        }
        if (!['', 'admin123'].includes(String(network.apPassword || ''))) {
            issues.push(`${label}: AP password must be blank or the documented factory default`);
        }
    }
    if (mqtt.enabled === true) issues.push(`${label}: MQTT must remain disabled until credentials are supplied`);
    ['clientId', 'username', 'password', 'mqttSecret', 'authCode', 'deviceNum', 'userId'].forEach((key) => {
        if (String(mqtt[key] || '') !== '') issues.push(`${label}: mqtt.${key} must be blank in a public example`);
    });
    ['auth.json', 'users.json'].forEach((name) => {
        if (docs.has(name)) issues.push(`${label}: ${name} must stay in the ignored local backup only`);
    });
}

function validatePeripherals(config, docs, issues) {
    const peripherals = Array.isArray(docs.get('peripherals.json')?.peripherals)
        ? docs.get('peripherals.json').peripherals : [];
    const ids = new Map();
    peripherals.forEach((item) => {
        const id = String(item?.id || '');
        if (!id) issues.push(`${config.source}: peripheral without id`);
        else if (ids.has(id)) issues.push(`${config.source}: duplicate peripheral id ${id}`);
        else ids.set(id, item);
    });

    Object.entries(config.peripherals).forEach(([id, [type, pins]]) => {
        const item = ids.get(id);
        if (!item) {
            issues.push(`${config.source}: required peripheral ${id} is missing`);
            return;
        }
        if (Number(item.type) !== type) issues.push(`${config.source}: ${id} type ${item.type} != ${type}`);
        if (item.enabled !== true) issues.push(`${config.source}: required peripheral ${id} must be enabled`);
        if (JSON.stringify(item.pins || []) !== JSON.stringify(pins)) {
            issues.push(`${config.source}: ${id} pins ${JSON.stringify(item.pins || [])} != ${JSON.stringify(pins)}`);
        }
    });

    const rules = Array.isArray(docs.get('periph_exec.json')?.rules) ? docs.get('periph_exec.json').rules : [];
    const enabledRules = rules.filter((rule) => rule?.enabled === true);
    if (enabledRules.length === 0) issues.push(`${config.source}: no enabled peripheral execution rule`);
    const ruleIds = new Set();
    rules.forEach((rule) => {
        const ruleId = String(rule?.id || '');
        if (!ruleId) issues.push(`${config.source}: execution rule without id`);
        else if (ruleIds.has(ruleId)) issues.push(`${config.source}: duplicate execution rule id ${ruleId}`);
        else ruleIds.add(ruleId);

        (Array.isArray(rule?.triggers) ? rule.triggers : []).forEach((trigger) => {
            const target = String(trigger?.triggerPeriphId || '');
            if (target && !ids.has(target) && !target.startsWith('modbus')) {
                issues.push(`${config.source}: rule ${ruleId} trigger references unknown peripheral ${target}`);
            }
        });
        (Array.isArray(rule?.actions) ? rule.actions : []).forEach((action) => {
            const target = String(action?.targetPeriphId || '');
            const type = Number(action?.actionType || 0);
            const special = target === 'led_all' || target.startsWith('modbus:') || target.startsWith('modbus-task:');
            if (target && !ids.has(target) && !special && !NON_PERIPHERAL_ACTIONS.has(type)) {
                issues.push(`${config.source}: rule ${ruleId} action references unknown peripheral ${target}`);
            }
        });
        if (rule?.enabled === true) {
            if (!Array.isArray(rule.triggers) || rule.triggers.length === 0) {
                issues.push(`${config.source}: enabled rule ${ruleId} has no trigger`);
            }
            if (!Array.isArray(rule.actions) || rule.actions.length === 0) {
                issues.push(`${config.source}: enabled rule ${ruleId} has no action`);
            }
        }
    });
}

function validateModbus(config, docs, issues) {
    if (!config.modbus) return;

    const rtu = docs.get('protocol.json')?.modbusRtu;
    if (!rtu || rtu.enabled !== true || rtu.mode !== 'master') {
        issues.push(`${config.source}: Modbus RTU master must be enabled`);
        return;
    }
    if (rtu.peripheralId !== config.modbus.peripheralId) {
        issues.push(`${config.source}: Modbus peripheralId ${rtu.peripheralId || '<empty>'} != ${config.modbus.peripheralId}`);
    }

    const peripherals = Array.isArray(docs.get('peripherals.json')?.peripherals)
        ? docs.get('peripherals.json').peripherals : [];
    const uart = peripherals.find((item) => item?.id === rtu.peripheralId);
    if (!uart || Number(uart.type) !== 1 || uart.enabled !== true) {
        issues.push(`${config.source}: Modbus peripheralId must reference an enabled UART peripheral`);
    }

    const tasks = Array.isArray(rtu.master?.tasks) ? rtu.master.tasks : [];
    const devices = Array.isArray(rtu.master?.devices) ? rtu.master.devices : [];
    if (tasks.length !== config.modbus.tasks) {
        issues.push(`${config.source}: Modbus task count ${tasks.length} != ${config.modbus.tasks}`);
    }
    const actualDeviceIds = devices.map((device) => String(device?.sensorId || '')).sort();
    const expectedDeviceIds = [...config.modbus.devices].sort();
    if (JSON.stringify(actualDeviceIds) !== JSON.stringify(expectedDeviceIds)) {
        issues.push(`${config.source}: Modbus devices ${JSON.stringify(actualDeviceIds)} != ${JSON.stringify(expectedDeviceIds)}`);
    }
}

function validateFleetMappings(issues) {
    const deployText = fs.readFileSync(path.join(ROOT, 'scripts', 'deploy-multi-device.ps1'), 'utf8');
    const fleet = readJson(path.join(ROOT, 'scripts', 'device-fleet.example.json'));
    const entries = Array.isArray(fleet) ? fleet : (fleet.devices || []);

    CONFIGS.forEach((config) => {
        for (const token of [config.environment, config.port, config.ip, config.source.replace(/\//g, '\\')]) {
            if (!deployText.includes(token)) issues.push(`scripts/deploy-multi-device.ps1: missing ${config.environment} mapping token ${token}`);
        }
        const fleetEntry = entries.find((entry) => entry.label === config.environment);
        if (!fleetEntry || fleetEntry.ip !== config.ip || String(fleetEntry.serial).toUpperCase() !== config.port) {
            issues.push(`scripts/device-fleet.example.json: incorrect mapping for ${config.environment}`);
        }
    });
}

function main() {
    const issues = [];
    const fingerprints = new Map();
    CONFIGS.forEach((config) => {
        try {
            const loaded = loadConfigSet(config.source);
            REQUIRED_FILES.forEach((name) => {
                if (!loaded.documents.has(name)) issues.push(`${config.source}: missing ${name}`);
            });
            validatePublicIdentity(config, loaded.documents, issues);
            validatePeripherals(config, loaded.documents, issues);
            validateModbus(config, loaded.documents, issues);
            const previous = fingerprints.get(loaded.fingerprint);
            if (previous) issues.push(`${config.source}: configuration is identical to ${previous}`);
            fingerprints.set(loaded.fingerprint, config.source);
        } catch (error) {
            issues.push(error.message);
        }
    });
    validateFleetMappings(issues);

    if (issues.length > 0) {
        console.error('FastBee device configuration example validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }
    console.log(`FastBee device configuration examples OK: devices=${CONFIGS.length}, unique=${fingerprints.size}`);
}

if (require.main === module) main();

module.exports = { fingerprintDocuments, loadConfigSet, stableJsonStringify };
