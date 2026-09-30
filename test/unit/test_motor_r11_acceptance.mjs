import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { exerciseMotor, retreatMotor } from '../../artifacts/motor-rx-r11/controls.mjs';

function fixture(fail = () => false) {
  const calls = [], state = { speed: 50, pulse: 600 };
  const command = async (action, value = 0) => {
    calls.push({ action, value });
    if (fail(action, calls)) throw Error('injected failure');
    if (action === 'setSpeed') state.speed = value;
    if (action === 'setPulse') state.pulse = value;
    return { success: true, pending: false, ...state };
  };
  return { calls, state, command };
}
test('bounded forward/reverse are followed by stop and original parameter readback', async () => {
  const f = fixture(), report = {};
  await exerciseMotor(f.command, report, async () => {});
  assert.deepEqual(f.calls.filter(c => ['forward','reverse'].includes(c.action)).map(c => c.action), ['forward','reverse']);
  assert.deepEqual(report.testParameters, { speed: 10, pulse: 100 });
  assert.deepEqual(f.state, { speed: 50, pulse: 600 });
  assert(report.finalStopAcknowledged && report.parametersRestoredAndReadBack);
});
test('unknown parameters never permit a write or motion', async () => {
  const f = fixture(action => action === 'readStatus');
  await assert.rejects(exerciseMotor(f.command, {}, async () => {}));
  assert.deepEqual(f.calls.map(c => c.action), ['readStatus']);
});
test('ambiguous start is not retried and prevents reverse', async () => {
  const f = fixture(action => action === 'forward'), report = {};
  await assert.rejects(exerciseMotor(f.command, report, async () => {}));
  assert.equal(f.calls.filter(c => c.action === 'forward').length, 1);
  assert(!f.calls.some(c => c.action === 'reverse'));
  assert(report.finalStopAcknowledged && report.parametersRestoredAndReadBack);
});
test('unconfirmed cleanup stop prohibits increasing/restoring parameters', async () => {
  const f = fixture((action, calls) => action === 'stop' && calls.some(c => c.action === 'forward')), report = {};
  await assert.rejects(exerciseMotor(f.command, report, async () => {}));
  assert.equal(report.finalStopAcknowledged, false);
  assert(!f.calls.some(c => c.action === 'reverse'));
  assert(!f.calls.some(c => c.action === 'setPulse' && c.value === 600));
  assert.deepEqual(f.state, { speed: 10, pulse: 100 });
});

test('upper end recovery sends reverse once and leaves reduced verified parameters', async () => {
  const f = fixture(), report = {};
  await retreatMotor(f.command, report, async () => {});
  assert.deepEqual(f.calls.filter(c => ['forward','reverse'].includes(c.action)).map(c => c.action), ['reverse']);
  assert.deepEqual(f.state, { speed: 5, pulse: 10 });
  assert(report.reverseAcknowledged && report.finalStopAcknowledged && report.reducedParametersReadBack);
  assert.equal(report.parameterRestorePolicy, 'KEEP_REDUCED_AFTER_UPPER_END_INCIDENT');
  assert(!f.calls.some(c => c.action === 'setPulse' && c.value > 10));
});

test('retreat requires initial stop acknowledgement before parameters or movement', async () => {
  const f = fixture(action => action === 'stop');
  await assert.rejects(retreatMotor(f.command, {}, async () => {}));
  assert.deepEqual(f.calls.map(c => c.action), ['stop']);
});

test('retreat read failure never sends parameter writes or movement', async () => {
  const f = fixture(action => action === 'readStatus');
  await assert.rejects(retreatMotor(f.command, {}, async () => {}));
  assert.deepEqual(f.calls.map(c => c.action), ['stop', 'readStatus']);
});

test('ambiguous reverse is never retried and is followed by stop without parameter increase', async () => {
  const f = fixture(action => action === 'reverse'), report = {};
  await assert.rejects(retreatMotor(f.command, report, async () => {}));
  assert.equal(f.calls.filter(c => c.action === 'reverse').length, 1);
  assert.equal(f.calls.at(-1).action, 'stop');
  assert(report.finalStopAcknowledged);
  assert.deepEqual(f.state, { speed: 5, pulse: 10 });
});

test('retreat stop failure prohibits further reads writes and motion', async () => {
  const f = fixture((action, calls) => action === 'stop' && calls.some(c => c.action === 'reverse')), report = {};
  await assert.rejects(retreatMotor(f.command, report, async () => {}));
  assert.equal(report.finalStopAcknowledged, false);
  assert.equal(f.calls.at(-1).action, 'stop');
  assert.deepEqual(f.state, { speed: 5, pulse: 10 });
});

test('retreat parameter readback mismatch prevents movement', async () => {
  const f = fixture(), report = {};
  const command = async (action, value) => {
    const result = await f.command(action, value);
    if (action === 'readStatus' && f.calls.some(c => c.action === 'setPulse')) result.pulse = 600;
    return result;
  };
  await assert.rejects(retreatMotor(command, report, async () => {}));
  assert(!f.calls.some(c => ['forward','reverse'].includes(c.action)));
  assert(report.finalStopAcknowledged);
});

test('retreat does not increase already lower parameters', async () => {
  const f = fixture(), report = {};
  f.state.speed = 2; f.state.pulse = 3;
  await retreatMotor(f.command, report, async () => {});
  assert.deepEqual(f.state, { speed: 2, pulse: 3 });
});

test('retired two-direction live runner rejects before any network or output access', () => {
  const result = spawnSync(process.execPath, [
    fileURLToPath(new URL('../../artifacts/motor-rx-r11/controls.mjs', import.meta.url)),
    '--execute-bounded', '--out=DO_NOT_CREATE_RETIRED_MOTION_REPORT.json'
  ], { encoding: 'utf8', timeout: 5000 });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /Two-direction acceptance retired/);
});
