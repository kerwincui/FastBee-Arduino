import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import vm from 'node:vm';
import test from 'node:test';

const source = await fs.readFile(new URL('../../artifacts/motor-readback-r8/diagnose.mjs', import.meta.url), 'utf8');
const mac = 'E8:3D:C1:F7:65:44';

// Execute the actual runner in an isolated module; no real fetch, filesystem
// writes or child processes are exposed to the code under test.
async function run({ skipLogs = false, snapshot = false, neighbor = mac, polling = false, register,
  compareRelay, relayOnly, motorConfigured = true, singleMaster = false, baudRate = 9600, parity = 0,
  hardware = false, hardwareSnapshot = { supported: true, readOnly: true, schemaVersion: 1,
    evidenceType: 'LIVE_GPIO_UART_REGISTERS', driverReady: true, rx: { valid: true },
    tx: { valid: true }, rxInputMatrix: { valid: true } },
  resourcesStatus = 200, resources = { resources: [], diagnostics: [] }, replies = [], forbidRequests = false } = {}) {
  const routes = [];
  let busIndex = 0;
  let report;
  const process = { argv: ['node', 'diagnose.mjs', '--out=in-memory.json',
    ...(skipLogs ? ['--skip-logs'] : []), ...(snapshot ? ['--snapshot-only'] : []),
    ...(hardware ? ['--uart-hardware'] : []),
    ...(register === undefined ? [] : ['--register=' + register]),
    ...(compareRelay === undefined ? [] : ['--compare-relay=' + compareRelay]),
    ...(relayOnly === undefined ? [] : ['--relay-only=' + relayOnly]),
    ...(singleMaster ? ['--confirm-single-master'] : [])], env: {}, exitCode: 0 };
  const context = vm.createContext({ process, URLSearchParams, console: { log() {} },
    setTimeout(callback) { callback(); }, AbortSignal: { timeout() {} },
    fetch: async (url, options) => {
      assert(!forbidRequests, 'No network request allowed by this test');
      assert.equal(new URL(url).origin, 'http://192.168.5.18');
      const route = new URL(url).pathname + new URL(url).search;
      routes.push({ route, method: options.method });
      let status = 200, json;
      const data = value => ({ success: true, data: value });
      switch (route) {
        case '/api/health': json = data({ status: 'ok' }); break;
        case '/api/auth/login': json = { success: true, sessionId: 'test-only-session' }; break;
        case '/api/auth/logout': json = { success: true }; break;
        case '/api/network/status': json = data({ macAddress: mac, ipAddress: '192.168.5.18' }); break;
        case '/api/system/info': json = data({ uptime: 100, device: {} }); break;
        case '/api/protocol/config':
          json = data({ modbusRtu: { enabled: true, mode: 'master', peripheralId: 'rs485', master: {
            devices: motorConfigured ? [{ enabled: true, deviceType: 'motor', slaveAddress: 8, motorRegs: [0, 1, 2, 5, 7] }] : [],
            responseTimeout: 1000, maxRetries: 2, tasks: [{ enabled: polling }] } } }); break;
        case '/api/peripherals': json = data([{ id: 'rs485', type: 1, enabled: true,
          params: { baudRate, dataBits: 8, stopBits: 1, parity } }]); break;
        case '/api/peripherals/resources':
          status = resourcesStatus;
          json = status === 200 ? data(resources) : { success: false, error: 'Resource planner unavailable' };
          break;
        case '/api/modbus/status?full=1': json = data({ initialized: true }); break;
        case '/api/modbus/status?full=1&hardware=1':
          json = data({ initialized: true, uartHardware: hardwareSnapshot }); break;
        case '/api/screen-panel/state': json = data({ revision: 34, cards: [], capabilities: { touchReady: true } }); break;
        case '/api/logs?lines=200': status = 503; json = { success: false, error: 'Log resource unavailable' }; break;
        default:
          assert([...([5, 7].map(address => '/api/modbus/register/read?slaveAddress=8&functionCode=3&startAddress=' + address + '&quantity=1')),
            ...(compareRelay === 6 || relayOnly === 6 ? ['/api/modbus/register/read?slaveAddress=6&functionCode=3&startAddress=0&quantity=1'] : [])].includes(route));
          if (relayOnly === 6) assert(!route.includes('slaveAddress=8'), 'Isolated relay mode must never query the motor');
          assert.equal(options.method, 'GET');
          const scripted = replies[busIndex++];
          if (scripted) { status = scripted.status; json = scripted.json; break; }
          status = 504;
          json = { success: false, errorCode: 'TIMEOUT', diagnostic: {
            scope: 'request:lastAttempt', reason: 'NO_RESPONSE_BYTES', attempts: 3,
            uartReady: true, uartBaudRate: 9600, totalRxBuffered: 0 } };
      }
      return { status, ok: status >= 200 && status < 300, json: async () => json };
    } });
  const fakeFs = { access: async () => { throw Error('ENOENT'); },
    writeFile: async (name, bytes, options) => {
      assert.equal(name, 'in-memory.json'); assert.equal(options.flag, 'wx'); report = JSON.parse(bytes);
    } };
  const dependencies = {
    'node:fs/promises': { default: fakeFs },
    'node:assert/strict': { default: assert },
    'node:child_process': { execFileSync(command) {
      if (command === 'ping.exe') return '';
      assert.equal(command, 'powershell.exe');
      return JSON.stringify([{ IPAddress: '192.168.5.18', State: 'Reachable', LinkLayerAddress: neighbor }]);
    } },
  };
  const module = new vm.SourceTextModule(source, { context });
  await module.link(name => {
    assert(name in dependencies, 'Unexpected dependency: ' + name);
    const exports = dependencies[name];
    return new vm.SyntheticModule(Object.keys(exports), function () {
      for (const [key, value] of Object.entries(exports)) this.setExport(key, value);
    }, { context });
  });
  await module.evaluate();
  assert(report);
  assert(routes.filter(r => r.method !== 'GET').every(r => ['/api/auth/login', '/api/auth/logout'].includes(r.route)));
  assert.equal(report.motorActionsSent, 0);
  assert.equal(report.relayActionsSent, 0);
  assert.equal(report.deviceConfigurationWrites, 0);
  return { report, routes, exitCode: process.exitCode };
}

test('optional log omission still runs exactly two documented reads and retains failures', async () => {
  const { report, routes } = await run({ skipLogs: true });
  assert(!routes.some(r => r.route.startsWith('/api/logs')));
  assert.equal(report.modbusReadApiCalls, 2);
  assert.deepEqual(report.registerReads.map(r => r.address), [5, 7]);
  assert.equal(report.status, 'READBACK_FAILURE_REPRODUCED');
  assert(report.registerReads.every(r => r.body.diagnostic.totalRxBuffered === 0));
});

test('default log failure remains visible and does not become a motor timeout', async () => {
  const { report, exitCode } = await run();
  assert.equal(exitCode, 1);
  assert.equal(report.status, 'DIAGNOSTIC_INCOMPLETE');
  assert.equal(report.modbusReadApiCalls, 0);
  assert.match(report.error, /logs.*503/);
});

test('wrong fresh neighbor MAC never sends credentials or Modbus traffic', async () => {
  const { report, routes } = await run({ skipLogs: true, neighbor: '00:11:22:33:44:55' });
  assert.equal(report.status, 'DIAGNOSTIC_INCOMPLETE');
  assert.equal(report.modbusReadApiCalls, 0);
  assert.deepEqual(routes.map(r => r.route), ['/api/health']);
});

test('active background polling fails closed before bus requests', async () => {
  const { report } = await run({ skipLogs: true, polling: true });
  assert.equal(report.status, 'DIAGNOSTIC_INCOMPLETE');
  assert.equal(report.modbusReadApiCalls, 0);
  assert.match(report.error, /Background bus polling/);
});

test('snapshot mode never sends the two Modbus reads', async () => {
  const { report } = await run({ skipLogs: true, snapshot: true });
  assert.equal(report.modbusReadApiCalls, 0);
  assert.equal(report.status, 'SNAPSHOT_ONLY_NO_BUS_REQUESTS');
});

test('master-only snapshot works without a configured motor and never accesses a slave', async () => {
  const { report, routes } = await run({ skipLogs: true, snapshot: true, motorConfigured: false });
  assert.equal(report.status, 'SNAPSHOT_ONLY_NO_BUS_REQUESTS');
  assert.equal(report.modbusReadApiCalls, 0);
  assert.deepEqual(report.readPlan, []);
  assert.deepEqual(report.registerReads, []);
  assert.equal(routes.filter(r => r.route === '/api/peripherals/resources').length, 1);
  assert(!routes.some(r => /register\/|coil\/|periph-exec/.test(r.route)));
  assert.equal(report.resourceSnapshot.evidenceType, 'CONFIGURATION_RESOURCE_PLANNER_NOT_GPIO_REGISTERS');
  assert.equal(report.physicalMotorVerified, false);
  assert.equal(report.continuous24hStatus, 'NOT_RUN');
});

test('master-only snapshot retains resource conflicts and active polling without changing them', async () => {
  const resources = { resources: [{ kind: 'gpio', index: 43 }],
    diagnostics: [{ code: 'PIN_CONFLICT', index: 43 }], claimsTruncated: true, issuesTruncated: false };
  const { report, routes } = await run({ skipLogs: true, snapshot: true, motorConfigured: false, polling: true, resources });
  assert.equal(report.backgroundPolling.enabledConfiguredTaskCount, 1);
  assert.equal(report.backgroundPolling.changed, false);
  assert.deepEqual(report.resourceSnapshot.diagnostics, resources.diagnostics);
  assert.equal(report.resourceSnapshot.claimsTruncated, true);
  assert(!routes.some(r => r.route.startsWith('/api/modbus/register/')));
  assert.equal(report.deviceConfigurationWrites, 0);
});

test('master-only snapshot keeps resource API failure visible and sends no bus request', async () => {
  const { report } = await run({ skipLogs: true, snapshot: true, motorConfigured: false, resourcesStatus: 503 });
  assert.equal(report.status, 'DIAGNOSTIC_INCOMPLETE');
  assert.match(report.error, /resources.*503/);
  assert.equal(report.modbusReadApiCalls, 0);
  assert.equal(report.resourceSnapshot, undefined);
});

test('UART0 planner warnings are preserved without claiming a measured hardware conflict', async () => {
  const diagnostics = [43, 44].map(index => ({
    code: 'PIN_UART0_ACTIVE', index, ownerId: 'rs485', conflictingOwnerId: 'runtime:uart0-console'
  }));
  const { report, routes } = await run({ skipLogs: true, snapshot: true,
    resources: { diagnostics, hardwarePinStateMeasured: true } });
  assert.deepEqual(report.resourceSnapshot.diagnostics, diagnostics);
  assert.equal(report.resourceSnapshot.hardwarePinStateMeasured, false);
  assert.equal(report.resourceSnapshot.evidenceType, 'CONFIGURATION_RESOURCE_PLANNER_NOT_GPIO_REGISTERS');
  assert.equal(report.status, 'SNAPSHOT_ONLY_NO_BUS_REQUESTS');
  assert.equal(report.modbusReadApiCalls, 0);
  assert(!routes.some(r => /register\/|coil\/|periph-exec/.test(r.route)));
});

test('master-only snapshot still refuses authentication when fresh identity differs', async () => {
  const { report, routes } = await run({ skipLogs: true, snapshot: true, neighbor: '00:11:22:33:44:55' });
  assert.equal(report.status, 'DIAGNOSTIC_INCOMPLETE');
  assert.deepEqual(routes.map(r => r.route), ['/api/health']);
  assert.equal(report.modbusReadApiCalls, 0);
});

test('explicit register selection sends just one documented read', async () => {
  for (const register of [5, 7]) {
    const { report } = await run({ skipLogs: true, register });
    assert.equal(report.modbusReadApiCalls, 1);
    assert.deepEqual(report.requestedRegisters, [register]);
    assert.deepEqual(report.registerReads.map(r => r.address), [register]);
    assert.equal(report.status, 'READBACK_FAILURE_REPRODUCED');
  }
});

test('hardware telemetry cannot be combined with bus read mode', async () => {
  await assert.rejects(run({ hardware: true, skipLogs: true, forbidRequests: true }), /requires snapshot-only/);
});

test('explicit hardware snapshot reads before and after without accessing any slave', async () => {
  const { report, routes } = await run({ hardware: true, snapshot: true, skipLogs: true });
  assert.equal(report.uartHardwareRequested, true);
  assert.equal(report.status, 'SNAPSHOT_ONLY_NO_BUS_REQUESTS');
  assert.equal(report.modbusReadApiCalls, 0);
  assert.equal(routes.filter(r => r.route === '/api/modbus/status?full=1&hardware=1').length, 2);
  assert.equal(report.modbusStatusBefore.uartHardware.evidenceType, 'LIVE_GPIO_UART_REGISTERS');
  assert.equal(report.modbusStatusAfter.uartHardware.schemaVersion, 1);
  assert(!routes.some(r => /register\/|coil\/|periph-exec/.test(r.route)));
});

test('missing or incomplete hardware telemetry fails closed with its evidence retained', async () => {
  for (const hardwareSnapshot of [undefined, { supported: false },
    { supported: true, schemaVersion: 1, readOnly: true, evidenceType: 'LIVE_GPIO_UART_REGISTERS', rx: { valid: false } }]) {
    const { report } = await run({ hardware: true, snapshot: true, skipLogs: true,
      hardwareSnapshot: hardwareSnapshot ?? null });
    assert.equal(report.status, 'DIAGNOSTIC_INCOMPLETE');
    assert.equal(report.modbusReadApiCalls, 0);
    assert.deepEqual(report.modbusStatusBefore.uartHardware, hardwareSnapshot ?? null);
  }
});

test('a measured disabled driver or RX output is not normalized into healthy hardware', async () => {
  const hardwareSnapshot = { supported: true, readOnly: true, schemaVersion: 1,
    evidenceType: 'LIVE_GPIO_UART_REGISTERS', driverReady: false,
    rx: { valid: true, outputEnabled: true }, tx: { valid: true }, rxInputMatrix: { valid: true } };
  const { report } = await run({ hardware: true, snapshot: true, skipLogs: true, hardwareSnapshot });
  assert.deepEqual(report.modbusStatusAfter.uartHardware, hardwareSnapshot);
  assert.equal(report.modbusReadApiCalls, 0);
  assert.equal(report.physicalMotorVerified, false);
});

test('undocumented or ambiguous register selections are rejected', async () => {
  for (const register of [0, 6, 65536, '5,7', '5x', '']) {
    await assert.rejects(run({ skipLogs: true, register }), /single documented register/);
  }
  await assert.rejects(run({ skipLogs: true, snapshot: true, register: 5 }), /Snapshot cannot/);
});

const comparisonOptions = { skipLogs: true, compareRelay: 6, singleMaster: true };
function successfulRead(address, value = 0) {
  return { status: 200, json: { success: true, data: { count: 1, startAddress: address, values: [value] },
    diagnostic: { crcValid: true, addressMatched: true, totalRxBuffered: 7, attempts: 1 } } };
}

test('comparison requires explicit single-master confirmation before any network request', async () => {
  await assert.rejects(run({ compareRelay: 6, forbidRequests: true }), /original master is disconnected/);
});

test('comparison rejects unknown slaves and snapshot combinations before network requests', async () => {
  for (const compareRelay of [0, 1, 8, 247, '6,8', '6x', '']) {
    await assert.rejects(run({ compareRelay, singleMaster: true, forbidRequests: true }), /user-confirmed comparison relay/);
  }
  await assert.rejects(run({ ...comparisonOptions, snapshot: true, forbidRequests: true }), /Snapshot cannot/);
});

test('comparison validates 9600 8N1 and stops before bus access on UART mismatch', async () => {
  for (const settings of [{ baudRate: 38400 }, { parity: 2 }]) {
    const { report } = await run({ ...comparisonOptions, ...settings });
    assert.equal(report.status, 'DIAGNOSTIC_INCOMPLETE');
    assert.equal(report.modbusReadApiCalls, 0);
    assert.match(report.error, /9600 8N1/);
  }
});

test('comparison brackets motor reads with relay reads and retains no-response evidence', async () => {
  const { report } = await run({ ...comparisonOptions,
    replies: [successfulRead(0), undefined, undefined, successfulRead(0)] });
  assert.deepEqual(report.registerReads.map(r => [r.slaveAddress, r.address]), [[6, 0], [8, 5], [8, 7], [6, 0]]);
  assert.equal(report.modbusReadApiCalls, 4);
  assert.equal(report.comparison.outcome, 'RELAY_RESPONDS_MOTOR_READBACK_FAILED');
  assert.equal(report.comparison.rootCauseConfirmed, false);
  assert.equal(report.physicalMotorVerified, false);
  assert(report.registerReads.filter(r => r.slaveAddress === 8).every(r => r.body.diagnostic.totalRxBuffered === 0));
});

test('two responsive slaves only prove readback, not physical outputs', async () => {
  const { report } = await run({ ...comparisonOptions, replies: [0, 5, 7, 0].map(a => successfulRead(a)) });
  assert.equal(report.status, 'READBACK_SUCCEEDED');
  assert.equal(report.comparison.outcome, 'BOTH_RESPOND');
  assert.equal(report.comparison.physicalOutputsVerified, false);
  assert.equal(report.continuous24hStatus, 'NOT_RUN');
});

test('both failures do not falsely identify a wiring or firmware root cause', async () => {
  const { report } = await run(comparisonOptions);
  assert.equal(report.comparison.outcome, 'NEITHER_READBACK_SUCCEEDED');
  assert.equal(report.comparison.rootCauseConfirmed, false);
});

test('motor success and relay failures remain separate outcomes', async () => {
  const { report } = await run({ ...comparisonOptions, replies: [undefined, successfulRead(5), successfulRead(7)] });
  assert.equal(report.comparison.outcome, 'MOTOR_RESPONDS_RELAY_READBACK_FAILED');
});

test('one relay response or partial motor feedback is classified as mixed', async () => {
  for (const replies of [[successfulRead(0)], [successfulRead(0), successfulRead(5), undefined, successfulRead(0)]]) {
    const { report } = await run({ ...comparisonOptions, replies });
    assert.equal(report.comparison.outcome, 'MIXED_OR_INTERMITTENT');
  }
});

test('comparison fails closed on incomplete, mismatched, nested failure or invalid CRC feedback', async () => {
  const malformed = [
    { success: true },
    { success: true, data: { success: false, count: 1, startAddress: 0, values: [0] } },
    { success: true, data: { count: 1, startAddress: 0, values: [] } },
    { success: true, data: { count: 1, startAddress: 1, values: [0] } },
    { success: true, data: { count: 1, startAddress: 0, values: ['0'] } },
    { success: true, data: { count: 1, startAddress: 0, values: [65536] } },
    { ...successfulRead(0).json, diagnostic: { crcValid: false, addressMatched: true } },
    { ...successfulRead(0).json, diagnostic: { crcValid: true, addressMatched: false } }
  ];
  for (const json of malformed) {
    const { report } = await run({ ...comparisonOptions, replies: [{ status: 200, json }] });
    assert.equal(report.registerReads[0].validFeedback, false);
    assert.notEqual(report.status, 'READBACK_SUCCEEDED');
  }
});

test('HTTP busy, throttling or lost authentication stops further comparison reads', async () => {
  for (const status of [401, 403, 429, 503]) {
    const { report } = await run({ ...comparisonOptions, replies: [{ status, json: { success: false } }] });
    assert.equal(report.status, 'DIAGNOSTIC_INCOMPLETE');
    assert.equal(report.modbusReadApiCalls, 1);
    assert.equal(report.comparison, undefined);
  }
});

test('comparison keeps the same fresh identity gate and never authenticates an unknown device', async () => {
  const { report, routes } = await run({ ...comparisonOptions, neighbor: '00:11:22:33:44:55' });
  assert.equal(report.modbusReadApiCalls, 0);
  assert.deepEqual(routes.map(r => r.route), ['/api/health']);
});

test('selected motor register still uses relay checks before and after', async () => {
  const { report } = await run({ ...comparisonOptions, register: 5 });
  assert.deepEqual(report.registerReads.map(r => [r.slaveAddress, r.address]), [[6, 0], [8, 5], [6, 0]]);
});

const relayOnlyOptions = { skipLogs: true, relayOnly: 6, singleMaster: true };
test('isolated relay mode sends exactly one documented relay read and no motor requests', async () => {
  const { report, routes } = await run(relayOnlyOptions);
  assert.deepEqual(report.registerReads.map(r => [r.slaveAddress, r.address]), [[6, 0]]);
  assert.equal(report.modbusReadApiCalls, 1);
  assert.deepEqual(report.requestedRegisters, []);
  assert(!routes.some(r => r.route.includes('slaveAddress=8')));
  assert.equal(report.relayIsolation.outcome, 'RELAY_READBACK_FAILED_WITH_MOTOR_DISCONNECTED');
  assert.equal(report.relayIsolation.rootCauseConfirmed, false);
});

test('isolated relay read does not require a configured motor or prove physical output operation', async () => {
  const { report } = await run({ ...relayOnlyOptions, motorConfigured: false, replies: [successfulRead(0)] });
  assert.equal(report.status, 'READBACK_SUCCEEDED');
  assert.equal(report.relayIsolation.outcome, 'RELAY_RESPONDS_WITH_MOTOR_DISCONNECTED');
  assert.equal(report.relayIsolation.physicalOutputsVerified, false);
  assert.equal(report.physicalMotorVerified, false);
});

test('relay-only invalid selections and mixed modes are rejected before any network request', async () => {
  for (const relayOnly of [0, 1, 8, '6,8', '', '6x']) {
    await assert.rejects(run({ ...relayOnlyOptions, relayOnly, forbidRequests: true }), /isolated relay slave 6/);
  }
  for (const extra of [{ register: 5 }, { compareRelay: 6 }, { snapshot: true }]) {
    await assert.rejects(run({ ...relayOnlyOptions, ...extra, forbidRequests: true }), /Relay-only cannot/);
  }
  await assert.rejects(run({ ...relayOnlyOptions, singleMaster: false, forbidRequests: true }), /original master is disconnected/);
});

test('relay-only retains identity, UART and background polling gates', async () => {
  for (const extra of [{ neighbor: '00:11:22:33:44:55' }, { baudRate: 38400 }, { parity: 2 }, { polling: true }]) {
    const { report } = await run({ ...relayOnlyOptions, ...extra });
    assert.equal(report.status, 'DIAGNOSTIC_INCOMPLETE');
    assert.equal(report.modbusReadApiCalls, 0);
  }
});

test('relay-only requires valid request-owned CRC feedback and preserves HTTP admission failures', async () => {
  const invalid = successfulRead(0);
  invalid.json.diagnostic.crcValid = false;
  const { report } = await run({ ...relayOnlyOptions, replies: [invalid] });
  assert.equal(report.status, 'READBACK_FAILURE_REPRODUCED');
  for (const status of [401, 403, 429, 503]) {
    const result = await run({ ...relayOnlyOptions, replies: [{ status, json: { success: false } }] });
    assert.equal(result.report.status, 'DIAGNOSTIC_INCOMPLETE');
    assert.equal(result.report.relayIsolation, undefined);
  }
});
