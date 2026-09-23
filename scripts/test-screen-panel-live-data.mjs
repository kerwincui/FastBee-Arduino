/** Read-only acceptance of actual screen bindings. No configuration/control writes. */
import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';

export function validateLiveData(state, weather, { sources, weatherId }) {
  assert.equal(state.available, true, 'Panel unavailable');
  assert.equal(state.application?.runtime?.running, true, 'Screen runtime stopped');
  assert.equal(state.clock?.valid, true, 'NTP/system time is not valid');
  assert.equal(state.clock.quality, 'good', 'Clock quality');
  assert.match(state.clock.time, /^\d{2}:\d{2}(?::\d{2})?$/, 'Clock format');
  for (const source of sources) {
    const item = state.items?.[source];
    assert.equal(item?.quality, 'good', `${source}: sample quality`);
    assert.ok(item.value !== '' && Number.isFinite(Number(item.value)), `${source}: numeric value missing`);
    const charts = state.cards.filter(card => card.sourceRef === source && card.historyCount > 0);
    assert.ok(charts.length, `${source}: history card missing`);
    for (const chart of charts) {
      assert.equal(chart.historyCount, 10, `${source}: expected ten samples`);
      assert.equal(chart.historyStale, false, `${source}: stale history`);
      assert.ok(!chart.historyError, `${source}: history error`);
    }
  }
  assert.equal(weather.sourceId, weatherId, 'Weather source mismatch');
  assert.equal(weather.status, 'ready', 'Weather fetch has not succeeded');
  assert.ok(weather.updatedMs > 0, 'Weather has never updated');
  assert.ok(Number.isFinite(weather.preview?.temperature), 'Weather temperature missing');
  assert.ok(weather.preview.city && weather.preview.condition, 'Weather visualization fields missing');
  const bound = state.items?.['weather.' + weatherId];
  assert.equal(bound?.quality, 'good', 'Weather card unbound or stale');
  assert.equal(bound.condition, weather.preview.condition, 'Weather condition differs from fetched data');
  assert.equal(bound.temperature, weather.preview.temperature, 'Weather temperature differs from fetched data');
  return {
    revision: state.revision, clock: state.clock,
    samples: sources.map(source => ({ source, value: state.items[source].value, unit: state.items[source].unit, historyCount: 10 })),
    weather: weather.preview, runtime: state.application.runtime,
  };
}

const option = key => process.argv.find(arg => arg.startsWith(`--${key}=`))?.slice(key.length + 3);
if (process.argv.includes('--contract-only')) {
  const state = { available: true, application: { runtime: { running: true } },
    clock: { valid: true, quality: 'good', time: '17:00' },
    items: { temp: { quality: 'good', value: '24.2' },
      'weather.home': { quality: 'good', condition: 'cloudy', temperature: 27 } },
    cards: [{ sourceRef: 'temp', historyCount: 10, historyStale: false, historyError: '' }] };
  const weather = { sourceId: 'home', status: 'ready', updatedMs: 10,
    preview: { temperature: 27, city: 'Test', condition: 'cloudy', forecast: [] } };
  const opts = { sources: ['temp'], weatherId: 'home' };
  validateLiveData(state, weather, opts);
  for (const change of [
    s => { s.application.runtime.running = false; },
    s => { s.clock.valid = false; },
    s => { s.items.temp.quality = 'stale'; },
    s => { s.items.temp.value = ''; },
    s => { s.cards[0].historyCount = 9; },
    s => { s.cards[0].historyStale = true; },
    s => { delete s.items['weather.home']; },
    s => { s.items['weather.home'].temperature = 0; },
  ]) {
    const invalid = structuredClone(state); change(invalid);
    assert.throws(() => validateLiveData(invalid, weather, opts));
  }
  assert.throws(() => validateLiveData(state, { ...weather, status: 'idle' }, opts));
  assert.throws(() => validateLiveData(state, { ...weather, updatedMs: 0 }, opts));
  console.log('PASS: live screen data contract (one valid + ten failure cases)');
} else if (option('url')) {
  const base = new URL(option('url'));
  assert.ok(['http:', 'https:'].includes(base.protocol) && !base.username && !base.password);
  const destination = option('report');
  assert.ok(destination, 'Use --url=http://device --report=file.json [--sources=a,b --weather=id]');
  const opts = { sources: (option('sources') || 'sht30_temperature,sht30_humidity').split(','), weatherId: option('weather') || 'home_weather' };
  const report = { at: new Date().toISOString(), origin: base.origin, passed: false, mode: 'read-only', checks: [] };
  let token = '';
  async function get(route) {
    const response = await fetch(base.origin + route, { headers: { Authorization: 'Bearer ' + token }, signal: AbortSignal.timeout(15000) });
    report.checks.push({ route, status: response.status });
    assert.equal(response.status, 200, route);
    const json = await response.json(); assert.notEqual(json.success, false, route);
    return json.data ?? json;
  }
  try {
    const response = await fetch(base.origin + '/api/auth/login', { method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'X-FastBee-Api-Client': 'screen-live-data-test' },
      body: new URLSearchParams({ username: process.env.FASTBEE_USERNAME || 'admin', password: process.env.FASTBEE_PASSWORD || 'admin123' }),
      signal: AbortSignal.timeout(15000) });
    const login = await response.json(); token = login.sessionId ?? login.data?.sessionId;
    assert.ok(response.ok && token, 'Login failed');
    const weather = await get('/api/remote-resources/weather-test?id=' + encodeURIComponent(opts.weatherId));
    const state = await get('/api/screen-panel/state');
    // Keep enough non-secret evidence to distinguish transport, stale samples,
    // weather startup and binding failures without archiving device config.
    report.observed = {
      clock: state.clock,
      samples: opts.sources.map(source => ({ source,
        quality: state.items?.[source]?.quality, value: state.items?.[source]?.value,
        unit: state.items?.[source]?.unit, updatedMs: state.items?.[source]?.updatedMs })),
      weather: { status: weather.status, updatedMs: weather.updatedMs,
        retryAfterMs: weather.retryAfterMs, preview: weather.preview },
      runtime: state.application?.runtime,
    };
    report.data = validateLiveData(state, weather, opts);
    report.passed = true;
  } catch (error) { report.error = error.message; process.exitCode = 1; }
  finally {
    token = '';
    await mkdir(path.dirname(path.resolve(destination)), { recursive: true });
    await writeFile(destination, JSON.stringify(report, null, 2) + '\n', { flag: 'wx' });
    console.log(JSON.stringify(report));
  }
}
