import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { calculateMotorTravel } from '../../scripts/lib/motor-travel-calibration.mjs';
import { planMotorTravel } from '../../scripts/plan-motor-travel.mjs';

// Synthetic fixtures, NOT measurements or settings for the user's slide.
const fixture = () => ({ units: 'mm', measurementConfirmed: true, usableTravelMm: 200,
  lowerMarginMm: 10, upperMarginMm: 10, allowedRelativeSpread: 0.02,
  samples: [{ pulses: 1000, measuredDistanceMm: 10 }, { pulses: 2020, measuredDistanceMm: 20 }] });

test('measured inputs produce a conservative offline candidate, never enable a device', () => {
  const result = calculateMotorTravel(fixture());
  assert.deepEqual(result.candidatePulseBounds, { minimum: 1010, maximum: 19000 });
  assert.equal(result.deployable, false);
  assert.equal(result.referenceKnown, false);
});
test('nominal length and one sample cannot stand in for calibration', () => {
  assert.throws(() => calculateMotorTravel({ usableTravelMm: 300 }));
  const input = fixture(); input.samples.pop();
  assert.throws(() => calculateMotorTravel(input));
});
test('middle-of-travel and photographed hardware do not supply measured calibration', () => {
  const input = { units: 'mm', measurementConfirmed: false,
    operatorReportedQualitativePosition: 'MIDDLE_OF_TRAVEL',
    operatorReportedNominalTravelMm: 300, operatorConfirmedScrewLeadMmPerRevolution: 5,
    photoSelectedDriverLabelPulsesPerRevolution: 400, theoreticalDriverInputPulsesPerMm: 80,
    usableTravelMm: null, lowerMarginMm: null, upperMarginMm: null,
    samples: [{ pulses: null, measuredDistanceMm: null }, { pulses: null, measuredDistanceMm: null }] };
  assert.throws(() => calculateMotorTravel(input), /Confirmed measurements/);
  assert.throws(() => calculateMotorTravel({ ...input, measurementConfirmed: true }), /usableTravelMm/);
});
test('hardware theoretical scale never overrides independently measured command counts', () => {
  const input = { ...fixture(), theoreticalDriverInputPulsesPerMm: 80 };
  const result = calculateMotorTravel(input);
  assert.deepEqual(result.pulsesPerMmRange, [100, 101]);
  assert.deepEqual(result.candidatePulseBounds, { minimum: 1010, maximum: 19000 });
  assert.equal(result.deployable, false);
});
test('operator middle and guessed reference never create a valid position or deployment', () => {
  const input = { ...fixture(), operatorReportedQualitativePosition: 'MIDDLE_OF_TRAVEL',
    assumedPositionMm: 100, referenceKnown: true, deployable: true };
  const result = calculateMotorTravel(input);
  assert.equal(result.referenceKnown, false);
  assert.equal(result.deployable, false);
  assert.equal(Object.hasOwn(result, 'currentPositionMm'), false);
  assert(result.requiredFieldChecks.includes('independent origin and direction verification'));
});
test('reject zero, negative, strings and nonfinite calibration data', () => {
  for (const key of ['usableTravelMm', 'lowerMarginMm', 'upperMarginMm'])
    for (const value of [0, -1, NaN, Infinity, '10', null])
      assert.throws(() => calculateMotorTravel({ ...fixture(), [key]: value }));
  for (const value of [0, -1, 1.5, 65536, NaN, Infinity, '10']) {
    const input = fixture(); input.samples[0].pulses = value;
    assert.throws(() => calculateMotorTravel(input));
  }
});
test('reject mismatched measurements, absent consent or unspecified accuracy', () => {
  assert.throws(() => calculateMotorTravel({ ...fixture(), measurementConfirmed: false }));
  for (const value of [undefined, 0, 0.2, NaN])
    assert.throws(() => calculateMotorTravel({ ...fixture(), allowedRelativeSpread: value }));
  const input = fixture(); input.samples[1].measuredDistanceMm = 10;
  assert.throws(() => calculateMotorTravel(input));
});
test('reject inverted limits and integer overflow', () => {
  assert.throws(() => calculateMotorTravel({ ...fixture(), lowerMarginMm: 190 }));
  assert.throws(() => calculateMotorTravel({ ...fixture(), usableTravelMm: 1e12 }));
});

test('offline CLI preserves existing evidence and does not write rejected plans', async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'motor-travel-cli-'));
  const input = path.join(root, 'measured.json'), output = path.join(root, 'review.json');
  try {
    await fs.writeFile(input, JSON.stringify(fixture()));
    const result = await planMotorTravel(['--measurements=' + input, '--out=' + output]);
    assert.equal(result.deployable, false);
    const first = await fs.readFile(output, 'utf8');
    await assert.rejects(planMotorTravel(['--measurements=' + input, '--out=' + output]));
    assert.equal(await fs.readFile(output, 'utf8'), first);
    await fs.writeFile(input, JSON.stringify({ usableTravelMm: 300 }));
    const rejected = path.join(root, 'rejected.json');
    await assert.rejects(planMotorTravel(['--measurements=' + input, '--out=' + rejected]));
    await assert.rejects(fs.access(rejected));
  } finally {
    assert.equal(path.dirname(path.resolve(root)), path.resolve(os.tmpdir()));
    assert(path.basename(root).startsWith('motor-travel-cli-'));
    await fs.rm(root, { recursive: true, force: true });
  }
});
