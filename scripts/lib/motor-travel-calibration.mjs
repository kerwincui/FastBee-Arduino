// Pure offline arithmetic. No device I/O or default hardware assumptions.
export function calculateMotorTravel(input) {
  const finitePositive = (key, value) => {
    if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0)
      throw new Error(key + ' must be a measured positive number');
    return value;
  };
  if (!input || input.units !== 'mm' || input.measurementConfirmed !== true)
    throw new Error('Confirmed measurements in mm required');
  const travel = finitePositive('usableTravelMm', input.usableTravelMm);
  const lowMargin = finitePositive('lowerMarginMm', input.lowerMarginMm);
  const highMargin = finitePositive('upperMarginMm', input.upperMarginMm);
  if (lowMargin + highMargin >= travel) throw new Error('Margins leave no usable travel');
  if (!Array.isArray(input.samples) || input.samples.length < 2)
    throw new Error('At least two independent calibration measurements required');
  const ppm = input.samples.map(sample => {
    const pulses = finitePositive('pulses', sample.pulses);
    if (!Number.isSafeInteger(pulses) || pulses > 65535) throw new Error('pulses must be a finite uint16 move');
    return pulses / finitePositive('measuredDistanceMm', sample.measuredDistanceMm);
  });
  const tolerance = input.allowedRelativeSpread;
  if (typeof tolerance !== 'number' || !Number.isFinite(tolerance) || tolerance <= 0 || tolerance > 0.05)
    throw new Error('Explicit relative spread acceptance in (0, 0.05] required');
  const min = Math.min(...ppm), max = Math.max(...ppm), spread = (max - min) / min;
  if (spread > tolerance) throw new Error('Calibration measurements disagree; do not enable limits');
  // The smaller observed pulses/mm gives a larger physical move per pulse.
  // This is arithmetic review, not a claim that future lost steps/drift are bounded.
  const lower = Math.ceil(lowMargin * max);
  const upper = Math.floor((travel - highMargin) * min);
  if (!Number.isSafeInteger(lower) || !Number.isSafeInteger(upper) || lower < 0 || upper > 2147483647 || upper <= lower)
    throw new Error('Pulse bounds overflow or leave no representable interval');
  return { status: 'OFFLINE_REVIEW_ONLY', usableTravelMm: travel, pulsesPerMmRange: [min, max],
    relativeSpread: spread, candidatePulseBounds: { minimum: lower, maximum: upper },
    referenceKnown: false, deployable: false,
    requiredFieldChecks: ['independent origin and direction verification', 'position uncertainty and braking margin',
      'restart/stop/lost-reply re-reference', 'physical end-stop and lift load protection'] };
}
