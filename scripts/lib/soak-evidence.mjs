export const EXPECTED_CARDS = [
  'water_leak', 'modbus_tvoc', 'modbus_hcho', 'pm25', 'pm10', 'modbus_temp',
  'modbus_humidity', 'ai_soil_temp_soil_temperature', 'ai_soil_humidity_soil_humidity'
];
const FAILURE_COUNTERS = ['displayErrors', 'httpErrorCount', 'pageErrorCount',
  'blockedWrites', 'samplingGaps', 'identityErrors', 'sseStreamErrors', 'sseJsonErrors'];

export function createReport(seconds, interval = 60) {
  return {status: 'RUNNING', stabilityStatus: 'RUNNING', measurementStatus: 'RUNNING',
    requestedSeconds: seconds, intervalSeconds: interval, attempts: 0, samples: 0,
    samplingGaps: 0, minimumRequiredSamples: Math.max(2, Math.ceil(seconds / interval * .9)),
    durationComplete: false, coveragePassed: false, displayErrors: 0, httpErrorCount: 0,
    httpStatusCounts: {}, http429Count: 0, transportErrors: 0, pageErrorCount: 0,
    sseStreamErrors: 0, sseJsonErrors: 0,
    blockedWrites: 0, identityErrors: 0, invalidMeasurementSamples: 0,
    measurementUnavailableSamples: 0, examples: [], errorTypeCounts: {},
    unverified: ['traceable-sensor-accuracy', 'physical-lamp-feedback', 'physical-edge-latency']};
}

export class CoverageTracker {
  constructor(report, started) {
    this.report = report; this.started = started; this.limit = Math.max(90, report.intervalSeconds * 3);
    this.lastSuccess = this.previousAttempt = null; this.gapOpen = false;
    this.firstSuccess = null;
    report.firstSuccessfulSampleAt = null;
    report.actualSampleSpanSeconds = 0;
    report.lastSuccessfulSampleAt = null;
  }
  update(now) {
    const r = this.report, elapsed = Math.max(0, now - this.started);
    const gap = Math.max(0, now - (this.lastSuccess ?? this.started));
    r.observedSeconds = elapsed;
    r.lastSuccessfulSampleAgeSeconds = this.lastSuccess === null ? null : gap;
    r.timeWithoutSuccessfulSampleSeconds = gap;
    r.maximumSamplingGapSeconds = Math.max(r.maximumSamplingGapSeconds || 0, gap);
    if (gap > this.limit && !this.gapOpen) { r.samplingGaps++; this.gapOpen = true; }
    r.durationComplete = elapsed >= r.requestedSeconds;
    r.coveragePassed = r.durationComplete && r.actualSampleSpanSeconds >= r.requestedSeconds &&
      r.samples >= r.minimumRequiredSamples && !r.samplingGaps;
    if (r.samplingGaps) r.stabilityStatus = 'FAIL';
  }
  attempt(now) {
    if (this.previousAttempt !== null) this.report.maximumAttemptGapSeconds = Math.max(
      this.report.maximumAttemptGapSeconds || 0, now - this.previousAttempt);
    this.previousAttempt = now; this.report.attempts++; this.update(now);
  }
  complete(now, at, complete) {
    this.update(now);
    if (complete) {
      if (this.firstSuccess === null) {
        this.firstSuccess = now;
        this.report.firstSuccessfulSampleAt = at;
      }
      this.report.samples++; this.lastSuccess = now; this.gapOpen = false;
      this.report.actualSampleSpanSeconds = Math.max(0, now - this.firstSuccess);
      this.report.lastSuccessfulSampleAt = at;
    } else { this.report.measurementUnavailableSamples++; this.report.measurementStatus = 'FAIL'; }
    this.update(now);
  }
}

export function recordError(report, kind, value, at = new Date().toISOString()) {
  const text = typeof value === 'string' ? value : JSON.stringify(value);
  const errorType = value?.status ? 'HTTP_' + value.status :
    /10051/.test(text) ? 'NETWORK_UNREACHABLE' : /10065/.test(text) ? 'HOST_UNREACHABLE' :
    /ERR_CONNECTION_RESET/.test(text) ? 'CONNECTION_RESET' : /[Tt]imeout|TIMED_OUT/.test(text) ? 'TIMEOUT' : kind;
  const event = {at, kind, errorType, value};
  report.latestError = event; (report.lastErrorByKind ??= {})[kind] = event;
  report.errorTypeCounts[errorType] = (report.errorTypeCounts[errorType] || 0) + 1;
  if (report.examples.length < 30) report.examples.push(event);
  return event;
}

export function recordHttpStatus(report, status, url, loginSubmitted) {
  if (status < 400 || (status === 401 && !loginSubmitted)) return false;
  report.httpErrorCount++;
  report.httpStatusCounts[status] = (report.httpStatusCounts[status] || 0) + 1;
  if (status === 429) report.http429Count++;
  if (new URL(url).pathname.startsWith('/api/') && [401, 403].includes(status)) report.authenticationLost = true;
  report.stabilityStatus = 'FAIL';
  return true;
}

// Every snapshot carries the cumulative connection history for this document.
// Repeated snapshots or later successful data cannot subtract earlier failures.
export function recordSseFailures(report, connections = []) {
  let streamErrors = 0, jsonErrors = 0;
  for (const connection of Array.isArray(connections) ? connections : []) {
    if (Number.isInteger(connection?.errors) && connection.errors > 0) streamErrors += connection.errors;
    if (Array.isArray(connection?.invalidJson)) jsonErrors += connection.invalidJson.length;
  }
  const delta = {streamErrors: Math.max(0, streamErrors - (report.sseStreamErrors || 0)),
    jsonErrors: Math.max(0, jsonErrors - (report.sseJsonErrors || 0))};
  report.sseStreamErrors = Math.max(report.sseStreamErrors || 0, streamErrors);
  report.sseJsonErrors = Math.max(report.sseJsonErrors || 0, jsonErrors);
  if (report.sseStreamErrors || report.sseJsonErrors) report.stabilityStatus = 'FAIL';
  return delta;
}

const numeric = value => (typeof value === 'number' || typeof value === 'string') &&
  String(value).trim() !== '' && Number.isFinite(Number(value));
const validMeasurement = card => !!card && numeric(card.value) &&
  card.measurementValid !== false && card.measurementValid !== 'false' &&
  card.valuePlaceholder !== true && card.valuePlaceholder !== 'true' &&
  (!card.quality || ['valid', 'good', 'ok'].includes(String(card.quality).toLowerCase()));
export function validateSnapshot(row, {authenticated, fresh}) {
  const cards = Array.isArray(row.cards) ? row.cards : [], coils = Array.isArray(row.coils) ? row.coils : [];
  const ids = new Set(cards.map(c => c.id)), channels = new Set(coils.map(c => String(c.channel)));
  const displayValid = cards.length === 9 && ids.size === 9 && EXPECTED_CARDS.every(id => ids.has(id)) &&
    coils.length === 4 && channels.size === 4 && ['0', '1', '2', '3'].every(ch => channels.has(ch));
  const modbusAvailable = EXPECTED_CARDS.slice(0, 7).every(id => validMeasurement(cards.find(c => c.id === id)));
  const temperatureAvailable = validMeasurement(cards.find(c => c.id === 'ai_soil_temp_soil_temperature'));
  const relayAvailable = coils.length === 4 && coils.every(c => !c.unknown && ['ON', 'OFF'].includes(c.text));
  const complete = !!(displayValid && modbusAvailable && temperatureAvailable && relayAvailable && authenticated && fresh);
  return {displayValid, modbusAvailable, temperatureAvailable, relayAvailable, authenticated, fresh, complete,
    measurementValid: complete && cards.every(validMeasurement)};
}

export function finishReport(report, {completed = false, interrupted = false} = {}) {
  const failed = report.stabilityStatus === 'FAIL' || FAILURE_COUNTERS.some(k => report[k]);
  const measuredFailed = report.measurementStatus === 'FAIL' || report.invalidMeasurementSamples || report.measurementUnavailableSamples;
  const covered = report.durationComplete && report.coveragePassed &&
    Number.isFinite(report.actualSampleSpanSeconds) && report.actualSampleSpanSeconds >= report.requestedSeconds;
  report.coveragePassed = Boolean(covered);
  const stable = completed && covered && !failed;
  report.stabilityStatus = stable ? 'PASS' : failed || completed ? 'FAIL' : 'INCOMPLETE';
  report.measurementStatus = measuredFailed ? 'FAIL' : completed && covered && report.samples ? 'PASS' : 'INCOMPLETE';
  report.status = interrupted ? 'INTERRUPTED' : stable && report.measurementStatus === 'PASS' ? 'PASS' : 'FAIL';
}

// Passive page instrumentation: observe existing streams, never open a new connection.
export function installSseEvidence() {
  if (window.__soakSseEvidenceInstalled) return;
  const Original = window.EventSource;
  window.__soakDataAt = {};
  window.__soakSseConnections = [];
  if (!Original) return;
  window.__soakSseEvidenceInstalled = true;
  window.EventSource = class extends Original {
    constructor(...args) {
      super(...args);
      const info = {createdAt: performance.now(), openedAt: null, lastEventAt: null,
        closedAt: null, errors: 0, events: {}, invalidJson: []};
      this.__soakEvidence = info;
      window.__soakSseConnections.push(info);
      this.addEventListener('open', () => { info.openedAt = performance.now(); });
      this.addEventListener('error', () => { if (info.closedAt === null) info.errors++; });
      for (const type of ['modbus-data', 'sensor-data', 'heartbeat']) this.addEventListener(type, event => {
        info.lastEventAt = performance.now();
        info.events[type] = (info.events[type] || 0) + 1;
        if (type === 'heartbeat') return;
        try {
          const value = JSON.parse(event.data);
          if (value && typeof value === 'object' && Object.keys(value).length) window.__soakDataAt[type] = performance.now();
        } catch {
          // Count failures without retaining payload text or credentials.
          info.invalidJson.push({at: performance.now(), event: type});
        }
      });
    }
    close() {
      if (this.__soakEvidence.closedAt === null) this.__soakEvidence.closedAt = performance.now();
      return super.close();
    }
  };
}
