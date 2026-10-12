#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const projectRoot = path.resolve(import.meta.dirname, '..');
const defaultDeploymentRoot = path.join(projectRoot, 'artifacts', 'multi-device-deploy');
const defaultOutputRoot = path.join(projectRoot, 'artifacts', 'live-config-audit');
const username = process.env.FASTBEE_AUDIT_USERNAME || 'admin';
const password = process.env.FASTBEE_AUDIT_PASSWORD || '';
const deploymentStaSSID = process.env.FASTBEE_STA_SSID || '';

const devices = [
  {
    key: 's3-f16r8',
    name: 'ESP32-S3-F16R8',
    ip: '192.168.5.22',
    port: 'COM15',
    environment: 'esp32s3-F16R8',
    source: path.join(projectRoot, 'docs', 'device-test-configs', 'com15-esp32s3-f16r8', 'config'),
  },
  {
    key: 'c3-f4r0',
    name: 'ESP32-C3-F4R0',
    ip: '192.168.5.55',
    port: 'COM4',
    environment: 'esp32c3-F4R0',
    source: path.join(projectRoot, 'docs', 'device-test-configs', 'com4-esp32c3-f4r0', 'config'),
    forceModbusDisabled: true,
  },
  {
    key: 'c6-f4r0',
    name: 'ESP32-C6-F4R0',
    ip: '192.168.5.19',
    port: 'COM11',
    environment: 'esp32c6-F4R0',
    source: path.join(projectRoot, 'docs', 'c6-rgb-led-f4r0'),
  },
  {
    key: 'esp32-f4r0',
    name: 'ESP32-F4R0',
    ip: '192.168.5.21',
    port: 'COM16',
    environment: 'esp32-F4R0',
    source: path.join(projectRoot, 'docs', 'device-test-configs', 'com16-esp32-f4r0', 'config'),
  },
  {
    key: 's3-f8r0',
    name: 'ESP32-S3-F8R0',
    ip: '192.168.5.67',
    port: 'COM7',
    environment: 'esp32s3-F8R0',
    source: path.join(projectRoot, 'docs', 'waveshare-esp32s3-relay6ch'),
  },
];

const endpoints = {
  session: '/api/auth/session',
  device: '/api/device/config',
  features: '/api/features/status',
  peripherals: '/api/peripherals?page=1&pageSize=100',
  rules: '/api/periph-exec?page=1&pageSize=100',
  protocol: '/api/protocol/config?compact=1',
  network: '/api/network/config?compact=1',
  networkStatus: '/api/network/status',
  system: '/api/system/status',
  mqttStatus: '/api/mqtt/status',
  modbusStatus: '/api/modbus/status?compact=1',
};

const deviceOrder = devices.map((device) => device.name);

function usage() {
  return [
    'Usage: node scripts/audit-live-device-config.mjs [output-dir] [options]',
    '',
    'Options:',
    '  --output-dir <dir>           Audit report output root',
    '  --deployment-summary <file>  Exact five-device deployment summary to audit',
    '  --deployment-dir <dir>       Find the latest complete deployment below this directory',
    '  --help                       Show this help',
    '',
    'Without a deployment option, the latest complete successful five-device',
    'deployment below artifacts/multi-device-deploy is selected automatically.',
  ].join('\n');
}

export function parseArgs(argv = process.argv.slice(2)) {
  const options = {
    outputRoot: defaultOutputRoot,
    deploymentSummary: '',
    deploymentDir: defaultDeploymentRoot,
    help: false,
  };
  const positional = [];
  const valueAfter = (index, flag) => {
    const value = argv[index + 1];
    if (!value || value.startsWith('--')) throw new Error(`${flag} requires a path`);
    return value;
  };

  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === '--help' || argument === '-h') {
      options.help = true;
    } else if (argument === '--output-dir') {
      options.outputRoot = path.resolve(valueAfter(index, argument));
      index += 1;
    } else if (argument === '--deployment-summary') {
      options.deploymentSummary = path.resolve(valueAfter(index, argument));
      index += 1;
    } else if (argument === '--deployment-dir') {
      options.deploymentDir = path.resolve(valueAfter(index, argument));
      index += 1;
    } else if (argument.startsWith('-')) {
      throw new Error(`unknown option: ${argument}`);
    } else {
      positional.push(argument);
    }
  }

  if (positional.length > 1) throw new Error('only one positional output directory is supported');
  if (positional.length === 1) {
    if (argv.includes('--output-dir')) throw new Error('use either positional output-dir or --output-dir, not both');
    options.outputRoot = path.resolve(positional[0]);
  }
  if (options.deploymentSummary && argv.includes('--deployment-dir')) {
    throw new Error('--deployment-summary and --deployment-dir are mutually exclusive');
  }
  return options;
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function timestamp() {
  return new Date().toISOString().replace(/[-:]/g, '').replace('T', '-').slice(0, 15);
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
}

function samePath(left, right) {
  const normalize = (value) => path.resolve(value).replace(/\\/g, '/').toLowerCase();
  return normalize(left) === normalize(right);
}

function resolveExistingPath(value, summaryPath, label) {
  if (!value || typeof value !== 'string') throw new Error(`${label} path is missing`);
  const candidates = path.isAbsolute(value)
    ? [path.resolve(value)]
    : [path.resolve(path.dirname(summaryPath), value), path.resolve(projectRoot, value)];
  const resolved = candidates.find((candidate) => fs.existsSync(candidate));
  if (!resolved) throw new Error(`${label} was not found: ${candidates[0]}`);
  return resolved;
}

function deploymentTimestamp(summary, summaryPath) {
  const declared = Date.parse(summary.finishedAt || summary.startedAt || '');
  if (Number.isFinite(declared)) return declared;
  return fs.statSync(summaryPath).mtimeMs;
}

function collectSummaryFiles(root) {
  if (!fs.existsSync(root) || !fs.statSync(root).isDirectory()) {
    throw new Error(`deployment directory was not found: ${root}`);
  }
  const files = [];
  const pending = [path.resolve(root)];
  while (pending.length > 0) {
    const directory = pending.pop();
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const target = path.join(directory, entry.name);
      if (entry.isDirectory()) pending.push(target);
      else if (entry.isFile() && entry.name.endsWith('-summary.json')) files.push(target);
    }
  }
  return files;
}

function validateDeploymentSummary(summaryPath) {
  const resolvedSummaryPath = path.resolve(summaryPath);
  if (!fs.existsSync(resolvedSummaryPath) || !fs.statSync(resolvedSummaryPath).isFile()) {
    throw new Error(`deployment summary was not found: ${resolvedSummaryPath}`);
  }

  const summary = readJson(resolvedSummaryPath);
  const errors = [];
  const results = Array.isArray(summary.results) ? summary.results : [];
  const resultByName = new Map(results.map((result) => [String(result.device || ''), result]));
  if (summary.dryRun === true) errors.push('summary is a dry run');
  if (summary.buildOnly === true) errors.push('summary is build-only');
  if (results.length !== devices.length || resultByName.size !== devices.length ||
      !deviceOrder.every((name) => resultByName.has(name))) {
    errors.push(`summary does not contain exactly the required ${devices.length} devices`);
  }
  if (Array.isArray(summary.order) && JSON.stringify(summary.order) !== JSON.stringify(deviceOrder)) {
    errors.push('deployment order differs from the required five-device order');
  }

  const bindings = new Map();
  const sourceHashes = [];
  for (const device of devices) {
    const result = resultByName.get(device.name);
    if (!result) continue;
    if (!['deployed', 'deployed-serial-warning'].includes(result.status)) {
      errors.push(`${device.name} status is ${JSON.stringify(result.status)}`);
    }
    if (String(result.environment || '') !== device.environment) {
      errors.push(`${device.name} environment differs from ${device.environment}`);
    }
    if (String(result.port || '').toUpperCase() !== device.port) {
      errors.push(`${device.name} port differs from ${device.port}`);
    }

    try {
      const reportPath = resolveExistingPath(result.report, resolvedSummaryPath, `${device.name} deployment report`);
      const deployment = readJson(reportPath);
      if (deployment.status !== 'success' || deployment.dryRun === true || deployment.buildOnly === true) {
        errors.push(`${device.name} deployment report is not a successful flash`);
      }
      if (String(deployment.environment || '') !== device.environment ||
          String(deployment.port || '').toUpperCase() !== device.port) {
        errors.push(`${device.name} deployment report identity does not match the fleet map`);
      }
      if (!deployment.validation || !Number.isFinite(Number(deployment.validation.peripherals)) ||
          !Number.isFinite(Number(deployment.validation.rules))) {
        errors.push(`${device.name} deployment report has no complete configuration validation`);
      }

      const sourceMeta = result.source || summary.sourceSha256?.[device.name] || {};
      const sourcePath = resolveExistingPath(sourceMeta.path || deployment.bundlePath || device.source,
        resolvedSummaryPath, `${device.name} configuration source`);
      const sourceHash = String(sourceMeta.sha256 || '').toLowerCase();
      if (!/^[a-f0-9]{64}$/.test(sourceHash)) {
        errors.push(`${device.name} source SHA-256 is missing or invalid`);
      } else {
        sourceHashes.push(sourceHash);
      }
      if (deployment.bundlePath && !samePath(deployment.bundlePath, sourcePath)) {
        errors.push(`${device.name} report bundle path differs from its summary source`);
      }
      bindings.set(device.name, { result, deployment, reportPath, sourcePath, sourceHash });
    } catch (error) {
      errors.push(String(error?.message || error));
    }
  }
  if (sourceHashes.length !== devices.length || new Set(sourceHashes).size !== devices.length) {
    errors.push('the five deployment sources do not have distinct valid SHA-256 identities');
  }
  if (errors.length > 0) {
    throw new Error(`invalid deployment summary ${resolvedSummaryPath}: ${errors.join('; ')}`);
  }
  return {
    summaryPath: resolvedSummaryPath,
    summary,
    bindings,
    finishedAtMs: deploymentTimestamp(summary, resolvedSummaryPath),
  };
}

export function resolveDeploymentContext({ deploymentSummary = '', deploymentDir = defaultDeploymentRoot } = {}) {
  if (deploymentSummary) return validateDeploymentSummary(deploymentSummary);

  const candidates = collectSummaryFiles(path.resolve(deploymentDir));
  const valid = [];
  const failures = [];
  for (const candidate of candidates) {
    try {
      valid.push(validateDeploymentSummary(candidate));
    } catch (error) {
      failures.push(String(error?.message || error));
    }
  }
  valid.sort((left, right) => right.finishedAtMs - left.finishedAtMs ||
    right.summaryPath.localeCompare(left.summaryPath));
  if (valid.length === 0) {
    const detail = failures.length > 0 ? ` First rejected candidate: ${failures[0]}` : '';
    throw new Error(`no complete successful five-device deployment summary found below ${deploymentDir}.${detail}`);
  }
  return valid[0];
}

function loadSource(sourcePath) {
  if (fs.statSync(sourcePath).isDirectory()) {
    const result = {};
    for (const name of ['device.json', 'features.json', 'network.json', 'peripherals.json', 'periph_exec.json', 'protocol.json']) {
      const file = path.join(sourcePath, name);
      if (fs.existsSync(file)) result[name] = readJson(file);
    }
    return result;
  }

  const bundle = readJson(sourcePath);
  const result = {};
  for (const entry of bundle.files || []) {
    if (typeof entry.name === 'string' && typeof entry.content === 'string') {
      result[entry.name] = JSON.parse(entry.content.replace(/^\uFEFF/, ''));
    }
  }
  return result;
}

function sourceContentHashes(sourcePath) {
  const hashes = {};
  if (fs.statSync(sourcePath).isDirectory()) {
    for (const entry of fs.readdirSync(sourcePath, { withFileTypes: true })) {
      if (!entry.isFile() || !entry.name.endsWith('.json')) continue;
      const text = fs.readFileSync(path.join(sourcePath, entry.name), 'utf8').replace(/^\uFEFF/, '');
      hashes[entry.name] = crypto.createHash('sha256').update(text, 'utf8').digest('hex');
    }
    return hashes;
  }
  const bundle = readJson(sourcePath);
  for (const entry of bundle.files || []) {
    if (typeof entry.name === 'string' && typeof entry.content === 'string') {
      // deploy-config-bundle.ps1 hashes the exact decoded `content` string
      // before JSON parsing, including an embedded BOM when one is present.
      hashes[entry.name] = crypto.createHash('sha256').update(entry.content, 'utf8').digest('hex');
    }
  }
  return hashes;
}

function redact(value, key = '') {
  if (/password|passphrase|secret|token|session|credential|authcode/i.test(key)) {
    return value === '' || value == null ? value : '[REDACTED]';
  }
  if (Array.isArray(value)) return value.map((item) => redact(item));
  if (value && typeof value === 'object') {
    return Object.fromEntries(Object.entries(value).map(([childKey, child]) => [childKey, redact(child, childKey)]));
  }
  return value;
}

function stable(value) {
  if (Array.isArray(value)) return `[${value.map(stable).join(',')}]`;
  if (value && typeof value === 'object') {
    return `{${Object.keys(value).sort().map((key) => `${JSON.stringify(key)}:${stable(value[key])}`).join(',')}}`;
  }
  return JSON.stringify(value);
}

function sha256(value) {
  return crypto.createHash('sha256').update(stable(value)).digest('hex');
}

function dataOf(response) {
  return response && typeof response === 'object' && 'data' in response ? response.data : response;
}

function listOf(response) {
  const data = dataOf(response);
  return Array.isArray(data) ? data : [];
}

function sorted(values) {
  return [...values].map(String).sort();
}

function sameSet(actual, expected) {
  return JSON.stringify(sorted(actual)) === JSON.stringify(sorted(expected));
}

export async function requestJson(url, options, retries = 2) {
  let lastError;
  const {
    timeoutMs = 30000,
    retryDelayMs = 1500,
    ...fetchOptions
  } = options;
  for (let attempt = 1; attempt <= retries + 1; attempt += 1) {
    const started = Date.now();
    try {
      const response = await fetch(url, {
        ...fetchOptions,
        signal: AbortSignal.timeout(timeoutMs),
        headers: {
          Accept: 'application/json',
          Connection: 'close',
          ...(fetchOptions.headers || {}),
        },
      });
      const text = await response.text();
      let body = null;
      let jsonError = '';
      if (!text.trim()) {
        jsonError = 'empty JSON response body';
      } else {
        try {
          body = JSON.parse(text);
          if (body === null || typeof body !== 'object') jsonError = 'JSON response is not an object or array';
        } catch {
          jsonError = `invalid JSON response (${Buffer.byteLength(text)} bytes)`;
        }
      }
      if (response.ok && jsonError) {
        lastError = new Error(jsonError);
        if (attempt <= retries) {
          await sleep(retryDelayMs * attempt);
          continue;
        }
        return {
          ok: false,
          status: response.status,
          durationMs: Date.now() - started,
          body: null,
          error: jsonError,
          setCookie: response.headers.get('set-cookie') || '',
        };
      }
      if (jsonError) {
        body = { parseError: true, byteLength: Buffer.byteLength(text) };
      }
      if ((response.status === 503 || response.status === 429) && attempt <= retries) {
        await sleep(retryDelayMs * attempt);
        continue;
      }
      return {
        ok: response.ok,
        status: response.status,
        durationMs: Date.now() - started,
        body,
        setCookie: response.headers.get('set-cookie') || '',
      };
    } catch (error) {
      lastError = error;
      if (attempt <= retries) {
        await sleep(retryDelayMs * attempt);
        continue;
      }
    }
  }
  return { ok: false, status: 0, durationMs: null, body: null, error: String(lastError?.message || lastError) };
}

async function login(device) {
  if (!password) {
    throw new Error('FASTBEE_AUDIT_PASSWORD is required for live device audit');
  }
  const body = new URLSearchParams({ username, password });
  const response = await requestJson(`http://${device.ip}/api/auth/login`, {
    method: 'POST',
    body,
    timeoutMs: 45000,
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
      'X-FastBee-Api-Client': '1',
    },
  }, 5);
  if (!response.ok || response.body?.success !== true) {
    throw new Error(`login failed (HTTP ${response.status || 0}${response.error ? `: ${response.error}` : ''})`);
  }
  const sessionId = response.body.sessionId;
  const cookiePair = response.setCookie.split(';')[0];
  if (cookiePair) return cookiePair;
  if (sessionId) return `sessionId=${sessionId}`;
  throw new Error('login succeeded without a session cookie');
}

function cloneJson(value) {
  return JSON.parse(JSON.stringify(value || {}));
}

function hasOwn(value, key) {
  return Boolean(value && Object.prototype.hasOwnProperty.call(value, key));
}

function sameValue(actual, expected) {
  return stable(actual) === stable(expected);
}

function getPath(value, keys) {
  let current = value;
  for (const key of keys) {
    if (!current || typeof current !== 'object' || !hasOwn(current, key)) return undefined;
    current = current[key];
  }
  return current;
}

function normalizedMqttTopics(value) {
  if (!Array.isArray(value)) return [];
  return value.filter((item) => item && typeof item.topic === 'string' && item.topic.trim());
}

function isSuccessfulJsonResponse(response) {
  return Boolean(
    response?.ok &&
    response.status === 200 &&
    response.body &&
    typeof response.body === 'object' &&
    response.body.parseError !== true &&
    response.body.success !== false
  );
}

export function secretResponseSemantics(value, configured) {
  const text = typeof value === 'string' ? value.trim() : '';
  let response = 'omitted';
  if (value !== undefined && value !== null && text) {
    const recognizedMask = /^\*{3,}$/.test(text) ||
      /^(?:\[?redacted\]?|<redacted>|\[?masked\]?|<masked>|hidden)$/i.test(text);
    response = recognizedMask ? 'masked' : 'exposed';
  } else if (value !== undefined && value !== null && typeof value !== 'string') {
    response = 'exposed';
  }

  const acceptable = response !== 'exposed' &&
    (configured === true || response === 'omitted');
  return { configured: Boolean(configured), response, acceptable };
}

export function applyDeploymentTransforms(device, deployment, source) {
  const protocol = cloneJson(source['protocol.json']);
  const network = cloneJson(source['network.json']);
  const mqtt = protocol.mqtt || {};
  const modbus = protocol.modbusRtu || {};

  if (device.forceModbusDisabled && hasOwn(modbus, 'enabled')) modbus.enabled = false;
  if (deployment.memoryProfile !== 'none' && mqtt.scheme === 'mqtts') {
    mqtt.scheme = 'mqtt';
    mqtt.port = 1883;
  }
  if (deployment.networkProfile === 'ecm50a' && source['network.json']) {
    Object.assign(network, {
      mode: 0,
      networkType: 0,
      connectTimeout: 20000,
      reconnectInterval: 5000,
      maxReconnectAttempts: 5,
      enableMDNS: true,
      failoverStrategy: 2,
      autoFailover: true,
      maxFailoverAttempts: 3,
      fallbackToDHCP: true,
    });
    if (!network.customDomain) network.customDomain = 'fastbee-s3';
    network.ethernet = Object.assign({}, network.ethernet, {
      spiMosi: 11, spiMiso: 13, spiSck: 12, csPin: 47, rstPin: 48, intPin: 14,
    });
    network.cellular = Object.assign({}, network.cellular, {
      txPin: 39, rxPin: 40, pwrPin: 38, baudRate: 115200, apn: 'CMNET',
    });
  }
  if (deployment.networkProvisioning?.staCredentialsInjected) {
    network.staSSID = deploymentStaSSID;
  }
  if (deployment.memoryProfile === 'classic-no-psram' && source['network.json']) {
    network.enableMDNS = false;
    if (String(network.staSSID || '').trim()) network.autoFailover = false;
  }
  if (deployment.memoryProfile === 'classic-no-psram' && protocol.mqtt) {
    // The classic target may persist this final degradation after boot when
    // Web/MQTT contention crosses its no-PSRAM budget. Audit the effective
    // deployed profile, not the untouched source bundle.
    mqtt.enabled = false;
    mqtt.publishTopics = [];
    mqtt.subscribeTopics = [];
  }
  return { protocol, network };
}

function verifySourceSnapshot(sourcePath, deployment) {
  const current = sourceContentHashes(sourcePath);
  const deployed = deployment.sourceSha256 || {};
  const currentNames = Object.keys(current).sort();
  const deployedNames = Object.keys(deployed).sort();
  if (!sameValue(currentNames, deployedNames)) {
    throw new Error('configuration source file set changed after deployment');
  }
  for (const name of deployedNames) {
    if (String(current[name]).toLowerCase() !== String(deployed[name]).toLowerCase()) {
      throw new Error(`configuration source ${name} changed after deployment`);
    }
  }
}

function expectedFor(device, deploymentContext) {
  const binding = deploymentContext.bindings.get(device.name);
  if (!binding) throw new Error(`deployment summary has no binding for ${device.name}`);
  const { deployment, reportPath, sourcePath } = binding;
  verifySourceSnapshot(sourcePath, deployment);
  const source = loadSource(sourcePath);
  const peripherals = source['peripherals.json']?.peripherals || [];
  const rules = source['periph_exec.json']?.rules || [];
  const removedPeripherals = new Set(deployment.memoryProfileDisabledPeripherals || []);
  const removedRules = new Set(deployment.memoryProfileDisabledRules || []);
  const stagedPeripherals = peripherals.filter((item) => !removedPeripherals.has(item.id));
  const stagedRules = rules.filter((item) => !removedRules.has(item.id));
  const effective = applyDeploymentTransforms(device, deployment, source);
  const protocol = effective.protocol;
  const sourceProtocol = source['protocol.json'] || {};
  const sourceNetwork = source['network.json'] || {};
  const explicitNetwork = cloneJson(sourceNetwork);
  if (deployment.networkProvisioning?.staCredentialsInjected) {
    if (!deploymentStaSSID) {
      throw new Error(`${device.name} audit requires FASTBEE_STA_SSID used for deployment`);
    }
    const expectedSsidHash = String(deployment.networkProvisioning.staSsidSha256 || '').toLowerCase();
    const actualSsidHash = crypto.createHash('sha256').update(deploymentStaSSID, 'utf8').digest('hex');
    if (expectedSsidHash && expectedSsidHash !== actualSsidHash) {
      throw new Error(`${device.name} FASTBEE_STA_SSID does not match the deployment report`);
    }
    explicitNetwork.staSSID = deploymentStaSSID;
    explicitNetwork.staPassword = deployment.networkProvisioning.staPasswordConfigured ? '[configured]' : '';
  }
  const expectedModbusEnabled = Boolean(protocol.modbusRtu?.enabled);
  const modbusTasks = protocol.modbusRtu?.master?.tasks || [];
  const enabledModbusTasks = modbusTasks.filter((item) => item.enabled !== false);

  return {
    deployment,
    reportPath,
    sourcePath,
    source,
    effective,
    explicit: {
      mqtt: sourceProtocol.mqtt || {},
      network: explicitNetwork,
    },
    summary: {
      identity: source['device.json'] || {},
      peripheralIds: sorted(stagedPeripherals.map((item) => item.id)),
      ruleIds: sorted(stagedRules.map((item) => item.id)),
      peripheralCount: deployment.validation.peripherals,
      enabledPeripheralCount: deployment.validation.enabledPeripherals,
      ruleCount: deployment.validation.rules,
      enabledRuleCount: deployment.validation.enabledRules,
      enabledFeatures: sorted(deployment.validation.enabledFeatures || []),
      modbusEnabled: expectedModbusEnabled,
      mqttEnabled: Boolean(protocol.mqtt?.enabled),
      modbusConfiguredTaskCount: modbusTasks.length,
      modbusConfiguredEnabledTaskCount: enabledModbusTasks.length,
      modbusStatusTaskCount: expectedModbusEnabled ? enabledModbusTasks.length : 0,
      modbusStatusEnabledTaskCount: expectedModbusEnabled ? enabledModbusTasks.length : 0,
    },
  };
}

export function prepareDeploymentAudit(deploymentContext) {
  const expectedByName = new Map();
  for (const device of devices) {
    expectedByName.set(device.name, expectedFor(device, deploymentContext));
  }
  return expectedByName;
}

export function analyse(device, expected, responses) {
  const session = dataOf(responses.session.body) || {};
  const deviceConfig = dataOf(responses.device.body) || {};
  const featureData = dataOf(responses.features.body) || {};
  const featureEndpointValid = isSuccessfulJsonResponse(responses.features) &&
    Array.isArray(featureData.features);
  const features = featureEndpointValid ? featureData.features : [];
  const peripherals = listOf(responses.peripherals.body);
  const rules = listOf(responses.rules.body);
  const protocol = dataOf(responses.protocol.body) || {};
  const network = dataOf(responses.network.body) || {};
  const networkStatus = dataOf(responses.networkStatus?.body) || {};
  const system = dataOf(responses.system.body) || {};
  const mqttStatus = dataOf(responses.mqttStatus.body) || {};
  const modbusStatus = dataOf(responses.modbusStatus.body) || {};
  const modbus = protocol.modbusRtu || {};
  const tasks = Array.isArray(modbus.master?.tasks) ? modbus.master.tasks : [];
  const modbusDevices = Array.isArray(modbus.master?.devices) ? modbus.master.devices : [];
  const mqtt = protocol.mqtt || {};
  const expectedMqtt = expected.effective.protocol.mqtt || {};
  const explicitMqtt = expected.explicit.mqtt;
  const expectedNetwork = expected.effective.network;
  const explicitNetwork = expected.explicit.network;
  const expectedIdentity = expected.summary.identity;
  const expectedFeatureKeys = new Set(expected.summary.enabledFeatures);
  const configuredFeatures = features.filter((item) => item.userEnabled).map((item) => item.key);
  const activeFeatures = features.filter((item) => item.active).map((item) => item.key);
  const mismatches = [];

  const check = (condition, message) => {
    if (!condition) mismatches.push(message);
  };
  for (const [name, response] of Object.entries(responses)) {
    const detail = response.error ? `: ${response.error}` : '';
    check(isSuccessfulJsonResponse(response),
      `${name} endpoint failed (HTTP ${response.status || 0}${detail})`);
  }
  if (isSuccessfulJsonResponse(responses.features)) {
    check(Array.isArray(featureData.features), 'features endpoint returned no feature metadata array');
  }
  check(session.sessionValid === true, 'authenticated session probe did not return sessionValid=true');
  check(peripherals.length === expected.summary.peripheralCount,
    `peripheral count ${peripherals.length} != ${expected.summary.peripheralCount}`);
  check(peripherals.filter((item) => item.enabled).length === expected.summary.enabledPeripheralCount,
    `enabled peripheral count ${peripherals.filter((item) => item.enabled).length} != ${expected.summary.enabledPeripheralCount}`);
  check(sameSet(peripherals.map((item) => item.id), expected.summary.peripheralIds), 'peripheral ID set differs from staged source');
  check(rules.length === expected.summary.ruleCount, `rule count ${rules.length} != ${expected.summary.ruleCount}`);
  check(rules.filter((item) => item.enabled).length === expected.summary.enabledRuleCount,
    `enabled rule count ${rules.filter((item) => item.enabled).length} != ${expected.summary.enabledRuleCount}`);
  check(sameSet(rules.map((item) => item.id), expected.summary.ruleIds), 'rule ID set differs from staged source');
  if (featureEndpointValid) {
    const seenFeatureKeys = new Set();
    for (const feature of features) {
      const key = String(feature?.key || '');
      check(Boolean(key), 'features endpoint returned metadata without a key');
      if (!key) continue;
      check(!seenFeatureKeys.has(key), `features endpoint returned duplicate metadata for ${key}`);
      seenFeatureKeys.add(key);
      const expectedEnabled = expectedFeatureKeys.has(key);
      if (hasOwn(feature, 'userEnabled')) {
        check(Boolean(feature.userEnabled) === expectedEnabled,
          `feature ${key} userEnabled=${Boolean(feature.userEnabled)} != ${expectedEnabled}`);
      }
      if (hasOwn(feature, 'active')) {
        const expectedActive = expectedEnabled && Boolean(feature.compiled) && !Boolean(feature.runtimeDisabled);
        check(Boolean(feature.active) === expectedActive,
          `feature ${key} active=${Boolean(feature.active)} != ${expectedActive}`);
      }
    }
  }
  check(Boolean(modbus.enabled) === expected.summary.modbusEnabled,
    `Modbus enabled=${Boolean(modbus.enabled)} != ${expected.summary.modbusEnabled}`);
  check(tasks.length === expected.summary.modbusConfiguredTaskCount,
    `configured Modbus task count ${tasks.length} != ${expected.summary.modbusConfiguredTaskCount}`);
  check(tasks.filter((item) => item.enabled !== false).length === expected.summary.modbusConfiguredEnabledTaskCount,
    `configured enabled Modbus task count ${tasks.filter((item) => item.enabled !== false).length} != ${expected.summary.modbusConfiguredEnabledTaskCount}`);
  check(Boolean(mqtt.enabled) === expected.summary.mqttEnabled,
    `MQTT enabled=${Boolean(mqtt.enabled)} != ${expected.summary.mqttEnabled}`);

  const checkedMqttFields = [];
  const mqttComparableFields = [
    'enabled', 'scheme', 'server', 'port', 'username', 'keepAlive',
    'autoReconnect', 'authType', 'willTopic', 'willQos', 'willRetain',
    'publishTopics', 'subscribeTopics',
  ];
  for (const key of mqttComparableFields) {
    if (!hasOwn(explicitMqtt, key)) continue;
    checkedMqttFields.push(key);
    const actual = key.endsWith('Topics') ? normalizedMqttTopics(mqtt[key]) : mqtt[key];
    const staged = key.endsWith('Topics') ? normalizedMqttTopics(expectedMqtt[key]) : expectedMqtt[key];
    check(sameValue(actual, staged), `MQTT ${key} differs from staged source`);
  }
  if (hasOwn(explicitMqtt, 'clientId')) {
    const stagedClientId = String(expectedMqtt.clientId || '').trim();
    checkedMqttFields.push(stagedClientId ? 'clientId' : 'clientId(generated)');
    check(stagedClientId ? sameValue(mqtt.clientId, expectedMqtt.clientId) : Boolean(String(mqtt.clientId || '').trim()),
      stagedClientId ? 'MQTT clientId differs from staged source' : 'MQTT clientId was not generated from the empty staged value');
  }
  const mqttSecretSemantics = {};
  for (const key of ['password', 'mqttSecret', 'authCode']) {
    if (!hasOwn(explicitMqtt, key)) continue;
    mqttSecretSemantics[key] = secretResponseSemantics(mqtt[key], Boolean(explicitMqtt[key]));
    check(mqttSecretSemantics[key].acceptable,
      mqttSecretSemantics[key].response === 'exposed'
        ? `MQTT ${key} API field is not masked or omitted`
        : `MQTT ${key} reports a stored secret although the staged source is empty`);
  }

  const networkFieldMap = [
    ['mode', ['network', 'mode']],
    ['networkType', ['network', 'networkType']],
    ['ipConfigType', ['network', 'ipConfigType']],
    ['enableMDNS', ['network', 'enableMDNS']],
    ['customDomain', ['network', 'customDomain']],
    ['staSSID', ['sta', 'ssid']],
    ['staticIP', ['sta', 'staticIP']],
    ['gateway', ['sta', 'gateway']],
    ['subnet', ['sta', 'subnet']],
    ['dns1', ['sta', 'dns1']],
    ['dns2', ['sta', 'dns2']],
    ['apSSID', ['ap', 'ssid']],
    ['apIP', ['ap', 'ip']],
    ['apChannel', ['ap', 'channel']],
    ['apHidden', ['ap', 'hidden']],
    ['apMaxConnections', ['ap', 'maxConnections']],
    ['connectTimeout', ['advanced', 'connectTimeout']],
    ['reconnectInterval', ['advanced', 'reconnectInterval']],
    ['maxReconnectAttempts', ['advanced', 'maxReconnectAttempts']],
    ['conflictDetection', ['advanced', 'conflictDetection']],
    ['failoverStrategy', ['advanced', 'failoverStrategy']],
    ['autoFailover', ['advanced', 'autoFailover']],
    ['conflictCheckInterval', ['advanced', 'conflictCheckInterval']],
    ['maxFailoverAttempts', ['advanced', 'maxFailoverAttempts']],
    ['conflictThreshold', ['advanced', 'conflictThreshold']],
    ['fallbackToDHCP', ['advanced', 'fallbackToDHCP']],
  ];
  const checkedNetworkFields = [];
  for (const [sourceKey, responsePath] of networkFieldMap) {
    if (!hasOwn(explicitNetwork, sourceKey)) continue;
    checkedNetworkFields.push(sourceKey);
    check(sameValue(getPath(network, responsePath), expectedNetwork[sourceKey]),
      `network ${sourceKey} differs from staged source`);
  }
  for (const section of ['ethernet', 'cellular']) {
    const explicitSection = explicitNetwork[section];
    if (!explicitSection || typeof explicitSection !== 'object') continue;
    for (const key of Object.keys(explicitSection)) {
      checkedNetworkFields.push(`${section}.${key}`);
      check(sameValue(getPath(network, [section, key]), expectedNetwork[section]?.[key]),
        `network ${section}.${key} differs from staged source`);
    }
  }
  const networkSecretSemantics = {};
  for (const [sourceKey, responsePath] of [
    ['staPassword', ['sta', 'password']],
    ['apPassword', ['ap', 'password']],
  ]) {
    if (!hasOwn(explicitNetwork, sourceKey)) continue;
    networkSecretSemantics[sourceKey] = secretResponseSemantics(
      getPath(network, responsePath),
      Boolean(explicitNetwork[sourceKey]),
    );
    check(networkSecretSemantics[sourceKey].acceptable,
      networkSecretSemantics[sourceKey].response === 'exposed'
        ? `network ${sourceKey} API field is not masked or omitted`
        : `network ${sourceKey} reports a stored secret although the staged source is empty`);
  }

  check(Boolean(mqttStatus.enabled) === expected.summary.mqttEnabled,
    `MQTT status enabled=${Boolean(mqttStatus.enabled)} != ${expected.summary.mqttEnabled}`);
  for (const key of ['server', 'port']) {
    if (!hasOwn(explicitMqtt, key)) continue;
    check(sameValue(mqttStatus[key], expectedMqtt[key]), `MQTT status ${key} differs from staged source`);
  }
  if (hasOwn(explicitMqtt, 'clientId')) {
    const stagedClientId = String(expectedMqtt.clientId || '').trim();
    check(stagedClientId
      ? sameValue(mqttStatus.clientId, expectedMqtt.clientId)
      : Boolean(String(mqttStatus.clientId || '').trim()),
    stagedClientId
      ? 'MQTT status clientId differs from staged source'
      : 'MQTT status clientId was not generated from the empty staged value');
  }
  const mqttDiagnostic = String(
    mqttStatus.statusReason || mqttStatus.lastErrorMessage || mqttStatus.errorMessage || '',
  ).trim();
  if (expected.summary.mqttEnabled && mqttStatus.connected !== true) {
    check(Boolean(mqttDiagnostic), 'MQTT is not connected but status API returned no failure/progress reason');
  }

  check(Number(modbusStatus.taskCount) === expected.summary.modbusStatusTaskCount,
    `Modbus status task count ${Number(modbusStatus.taskCount)} != ${expected.summary.modbusStatusTaskCount}`);
  check(Number(modbusStatus.health?.enabledTaskCount) === expected.summary.modbusStatusEnabledTaskCount,
    `Modbus status enabled task count ${Number(modbusStatus.health?.enabledTaskCount)} != ${expected.summary.modbusStatusEnabledTaskCount}`);
  if (!expected.summary.modbusEnabled) {
    check(modbusStatus.status === 'stopped', 'disabled Modbus status is not stopped');
  }
  const runtimeNetworkIp = String(networkStatus.ipAddress || '');
  check(networkStatus.connected === true, 'network status endpoint does not report connected=true');
  check(runtimeNetworkIp === device.ip,
    `network status endpoint IP ${JSON.stringify(runtimeNetworkIp)} differs from expected STA address ${device.ip}`);
  if (hasOwn(system, 'networkConnected')) {
    check(system.networkConnected === true, 'system status endpoint does not report networkConnected=true');
  }
  if (hasOwn(system, 'ipAddress')) {
    check(String(system.ipAddress || '') === device.ip,
      `system status endpoint IP ${JSON.stringify(String(system.ipAddress || ''))} differs from expected STA address ${device.ip}`);
  }

  // /api/network/config is a configuration read. Older constrained builds
  // returned its cached status snapshot before refreshing NetworkManager, so
  // connected=true could coexist with an empty IP/SSID. Preserve that signal
  // for diagnosis, but use the dedicated runtime endpoint for acceptance.
  const configSnapshotIp = String(network.status?.ipAddress || '');
  const configSnapshotStale = Boolean(
    network.status?.connected === true &&
    !configSnapshotIp &&
    networkStatus.connected === true &&
    runtimeNetworkIp
  );

  for (const key of ['deviceName', 'productNumber']) {
    if (expectedIdentity[key] !== undefined) {
      check(String(deviceConfig[key]) === String(expectedIdentity[key]),
        `${key}=${JSON.stringify(deviceConfig[key])} != staged value`);
    }
  }
  if (expectedIdentity.deviceId) {
    check(String(deviceConfig.deviceId) === String(expectedIdentity.deviceId), 'deviceId differs from staged value');
  } else {
    check(Boolean(deviceConfig.deviceId), 'deviceId was not generated from the empty staged value');
  }

  let special = {};
  if (device.key === 's3-f16r8') {
    const intervals = tasks.map((task) => Number(task.pollInterval));
    const disabledDeviceCount = modbusDevices.filter((item) => item.enabled === false).length;
    special = {
      pollTaskCount: tasks.length,
      pollIntervals: intervals,
      allPollIntervalsTenSeconds: tasks.length === 4 && intervals.every((value) => value === 10),
      disabledPollTaskCount: tasks.filter((item) => item.enabled === false).length,
      modbusDeviceCount: modbusDevices.length,
      disabledModbusDeviceCount: disabledDeviceCount,
    };
    check(special.allPollIntervalsTenSeconds, 'S3-F16R8 Modbus pollInterval values are not all 10 seconds');
    check(special.disabledPollTaskCount === 4, `S3-F16R8 disabled poll task count ${special.disabledPollTaskCount} != 4`);
    check(modbusDevices.length === 5 && disabledDeviceCount === 5,
      `S3-F16R8 disabled Modbus device templates ${disabledDeviceCount}/${modbusDevices.length} != 5/5`);
  }
  if (device.key === 's3-f8r0') {
    const sensors = modbusDevices.filter((item) => item.deviceType === 'sensor');
    special = {
      pollTaskCount: tasks.length,
      disabledPollTaskCount: tasks.filter((item) => item.enabled === false).length,
      sensorDeviceCount: sensors.length,
      disabledSensorDeviceCount: sensors.filter((item) => item.enabled === false).length,
    };
    check(sensors.length === 1 && sensors[0].enabled === false,
      'S3-F8R0 temperature/humidity sensor template is missing or enabled');
  }
  const fingerprintInput = {
    identity: {
      deviceId: deviceConfig.deviceId,
      productNumber: deviceConfig.productNumber,
      deviceName: deviceConfig.deviceName,
    },
    configuredFeatures: featureEndpointValid ? sorted(configuredFeatures) : ['[FEATURE_ENDPOINT_INVALID]'],
    peripherals: sorted(peripherals.map((item) => item.id)),
    rules: sorted(rules.map((item) => item.id)),
    modbus: redact(modbus),
    mqtt: {
      enabled: mqtt.enabled,
      scheme: mqtt.scheme,
      server: mqtt.server,
      port: mqtt.port,
      clientId: mqtt.clientId,
      username: mqtt.username,
    },
    network: redact({
      network: network.network,
      sta: network.sta,
      ap: network.ap,
      advanced: network.advanced,
      ethernet: network.ethernet,
      cellular: network.cellular,
    }),
  };

  return {
    identity: fingerprintInput.identity,
    sessionValid: session.sessionValid === true,
    endpointDurationsMs: Object.fromEntries(Object.entries(responses).map(([key, value]) => [key, value.durationMs])),
    configuredFeatures: featureEndpointValid ? sorted(configuredFeatures) : null,
    activeFeatures: featureEndpointValid ? sorted(activeFeatures) : null,
    notCompiledFeatures: featureEndpointValid
      ? sorted(features.filter((item) => !item.compiled).map((item) => item.key))
      : null,
    runtimeDisabledFeatures: featureEndpointValid
      ? sorted(features.filter((item) => item.runtimeDisabled).map((item) => item.key))
      : null,
    featureRuntimeDegraded: featureEndpointValid ? Boolean(featureData.summary?.runtimeDegraded) : null,
    peripheralCount: peripherals.length,
    enabledPeripheralCount: peripherals.filter((item) => item.enabled).length,
    peripheralIds: sorted(peripherals.map((item) => item.id)),
    memorySuspendedPeripheralIds: sorted(peripherals.filter((item) => item.memorySuspended).map((item) => item.id)),
    ruleCount: rules.length,
    enabledRuleCount: rules.filter((item) => item.enabled).length,
    ruleIds: sorted(rules.map((item) => item.id)),
    modbus: {
      enabled: Boolean(modbus.enabled),
      taskCount: tasks.length,
      enabledTaskCount: tasks.filter((item) => item.enabled !== false).length,
      deviceCount: modbusDevices.length,
      enabledDeviceCount: modbusDevices.filter((item) => item.enabled).length,
    },
    mqtt: {
      enabled: Boolean(mqtt.enabled),
      scheme: mqtt.scheme || '',
      server: mqtt.server || '',
      port: Number(mqtt.port || 0),
      clientId: mqtt.clientId || '',
      username: mqtt.username || '',
      checkedFields: checkedMqttFields,
      secretFields: mqttSecretSemantics,
      runtime: {
        enabled: Boolean(mqttStatus.enabled),
        initialized: Boolean(mqttStatus.initialized),
        connected: Boolean(mqttStatus.connected),
        connecting: Boolean(mqttStatus.connecting),
        disabledByMemory: Boolean(mqttStatus.disabledByMemory),
        stoppedByMemory: Boolean(mqttStatus.stoppedByMemory),
        diagnosticPresent: Boolean(mqttDiagnostic),
        lastErrorCode: Number(mqttStatus.lastErrorCode || 0),
        lastErrorClass: mqttStatus.lastErrorClass || '',
        statusReason: mqttDiagnostic,
      },
    },
    network: {
      mode: network.network?.mode,
      networkType: network.network?.networkType,
      staSsid: network.sta?.ssid || '',
      connected: Boolean(networkStatus.connected),
      ipAddress: runtimeNetworkIp,
      enableMDNS: Boolean(network.network?.enableMDNS),
      checkedFields: checkedNetworkFields,
      secretFields: networkSecretSemantics,
      configSnapshot: {
        connected: Boolean(network.status?.connected),
        ipAddress: configSnapshotIp,
        ssid: network.status?.ssid || '',
        stale: configSnapshotStale,
      },
      systemStatus: {
        connected: hasOwn(system, 'networkConnected') ? Boolean(system.networkConnected) : null,
        ipAddress: hasOwn(system, 'ipAddress') ? String(system.ipAddress || '') : null,
      },
    },
    modbusStatus: {
      status: modbusStatus.status || '',
      taskCount: Number(modbusStatus.taskCount || 0),
      enabledTaskCount: Number(modbusStatus.health?.enabledTaskCount || 0),
      riskLevel: modbusStatus.health?.riskLevel || '',
      warningCount: Array.isArray(modbusStatus.health?.warnings) ? modbusStatus.health.warnings.length : 0,
    },
    system: redact(system),
    special,
    configFingerprintSha256: sha256(fingerprintInput),
    mismatches,
    passed: mismatches.length === 0,
  };
}

async function auditDevice(device, runDir, deploymentContext, expected) {
  const cookie = await login(device);
  const rawResponses = {};
  for (const [name, endpoint] of Object.entries(endpoints)) {
    const response = await requestJson(`http://${device.ip}${endpoint}`, {
      method: 'GET',
      timeoutMs: 30000,
      headers: { Cookie: cookie },
    }, 2);
    rawResponses[name] = {
      ok: response.ok,
      status: response.status,
      durationMs: response.durationMs,
      error: response.error || null,
      body: response.body,
    };
    await sleep(150);
  }
  // Compare secrets only in memory. The persisted response payloads are always
  // recursively redacted before they enter the report object.
  const analysis = analyse(device, expected, rawResponses);
  const responses = Object.fromEntries(Object.entries(rawResponses).map(([name, response]) => [name, {
    ...response,
    body: redact(response.body),
  }]));
  const report = {
    auditedAt: new Date().toISOString(),
    device: device.name,
    ip: device.ip,
    deploymentSummary: deploymentContext.summaryPath,
    deploymentReport: expected.reportPath,
    configurationSource: expected.sourcePath,
    expected: expected.summary,
    analysis,
    responses,
  };
  fs.writeFileSync(path.join(runDir, `${device.key}.json`), `${JSON.stringify(report, null, 2)}\n`);
  return report;
}

async function main() {
  const options = parseArgs();
  if (options.help) {
    console.log(usage());
    return;
  }
  const deploymentContext = resolveDeploymentContext(options);
  // Validate report/source identity for all five boards before issuing the
  // first HTTP request. A stale source must fail the audit as one unit.
  const expectedByName = prepareDeploymentAudit(deploymentContext);
  const runDir = path.join(options.outputRoot, timestamp());
  fs.mkdirSync(runDir, { recursive: true });
  const settled = await Promise.allSettled(
    devices.map((device) => auditDevice(
      device,
      runDir,
      deploymentContext,
      expectedByName.get(device.name),
    )),
  );
  const reports = settled.map((result, index) => {
    if (result.status === 'fulfilled') return result.value;
    return {
      auditedAt: new Date().toISOString(),
      device: devices[index].name,
      ip: devices[index].ip,
      analysis: { passed: false, mismatches: [String(result.reason?.message || result.reason)] },
    };
  });
  const fingerprints = reports.map((item) => item.analysis.configFingerprintSha256).filter(Boolean);
  const summary = {
    auditedAt: new Date().toISOString(),
    runDir,
    deploymentSummary: deploymentContext.summaryPath,
    deploymentRunId: deploymentContext.summary.runId || '',
    deviceCount: reports.length,
    passedCount: reports.filter((item) => item.analysis.passed).length,
    distinctConfigFingerprints: new Set(fingerprints).size,
    collectedFingerprintsAllDistinct: new Set(fingerprints).size === fingerprints.length,
    allDeviceFingerprintsVerifiedDistinct: fingerprints.length === devices.length && new Set(fingerprints).size === devices.length,
    devices: reports.map((item) => ({
      device: item.device,
      ip: item.ip,
      passed: item.analysis.passed,
      identity: item.analysis.identity,
      peripheralCount: item.analysis.peripheralCount,
      ruleCount: item.analysis.ruleCount,
      configuredFeatures: item.analysis.configuredFeatures,
      activeFeatures: item.analysis.activeFeatures,
      sessionValid: item.analysis.sessionValid,
      modbus: item.analysis.modbus,
      modbusStatus: item.analysis.modbusStatus,
      mqtt: item.analysis.mqtt,
      network: item.analysis.network,
      special: item.analysis.special,
      configFingerprintSha256: item.analysis.configFingerprintSha256,
      endpointDurationsMs: item.analysis.endpointDurationsMs,
      mismatches: item.analysis.mismatches,
    })),
  };
  if (!summary.collectedFingerprintsAllDistinct) {
    summary.fleetMismatch = 'At least two collected live config fingerprints are identical';
  } else if (!summary.allDeviceFingerprintsVerifiedDistinct) {
    summary.fleetMismatch = `Only ${fingerprints.length}/${devices.length} live config fingerprints were collected`;
  }
  fs.writeFileSync(path.join(runDir, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);
  console.log(JSON.stringify(summary, null, 2));
  process.exitCode = summary.passedCount === devices.length && summary.allDeviceFingerprintsVerifiedDistinct ? 0 : 1;
}

const directExecution = process.argv[1] &&
  import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href;
if (directExecution) await main();
