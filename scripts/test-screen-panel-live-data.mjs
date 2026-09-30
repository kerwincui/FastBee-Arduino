/** Read-only acceptance of actual screen bindings. No configuration/control writes. */
import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const WEATHER_CONFIG_ROUTE = '/api/screen-panel/weather';
const weatherStatusRoute = id => '/api/screen-panel/weather/refresh?id=' + encodeURIComponent(id);

export function deriveLiveOptions(config, overrides = {}) {
  assert.ok(Array.isArray(config?.pages) && config.pages.length > 0, 'Saved panel pages missing');
  const cards = config.pages.flatMap(page => page.cards);
  assert.ok(cards.every(card => card && typeof card === 'object'), 'Saved panel cards malformed');
  const historyCards = cards.filter(card => card.historyMode === 'recent10');
  assert.ok(historyCards.every(card => card.sourceRef), 'History card has no binding');
  const sources = overrides.sources ?? [...new Set(historyCards.map(card => card.sourceRef))];
  assert.ok(sources.every(source => source && historyCards.some(card => card.sourceRef === source)),
    'Requested history source is not bound to a configured chart');
  const weatherCards = cards.filter(card => card.kind === 'weather');
  assert.ok(weatherCards.every(card => /^weather\.[A-Za-z0-9_-]+$/.test(card.sourceRef || '')),
    'Weather card has no valid source binding');
  const weatherIds = [...new Set(weatherCards.map(card => card.sourceRef.slice('weather.'.length)))];
  assert.ok(weatherIds.length <= 1, 'Seniverse cards must use the shared weather source');
  const weatherId = overrides.weatherId ?? weatherIds[0];
  assert.ok(weatherId === undefined || weatherIds.includes(weatherId), 'Requested weather is not bound to a configured card');
  return { sources, weatherId, historyCardIds: historyCards.filter(card => sources.includes(card.sourceRef)).map(card => card.id) };
}

export function validateWeatherSecrets(value) {
  if (!value || typeof value !== 'object') return;
  for (const [key, nested] of Object.entries(value)) {
    assert.ok(!/^(apiKey|authHeader|password|token|credentials?|raw|url|forecast)$/i.test(key),
      'Weather response contains a forbidden credential or retired field');
    validateWeatherSecrets(nested);
  }
}

function weatherEvidence(weather) {
  if (!weather) return undefined;
  const pick = (value, fields) => Object.fromEntries(fields.filter(key =>
    ['string', 'number', 'boolean'].includes(typeof value?.[key])).map(key => [key, value[key]]));
  return { ...pick(weather, ['sourceId', 'status', 'updatedMs', 'retryAfterMs', 'reason', 'stale']),
    preview: pick(weather.preview, ['temperature', 'unit', 'city', 'description', 'condition', 'humidity']) };
}

export function validateLiveData(state, weather, { sources, weatherId, historyCardIds }) {
  assert.equal(state.available, true, 'Panel unavailable');
  assert.equal(state.application?.runtime?.running, true, 'Screen runtime stopped');
  assert.equal(state.clock?.valid, true, 'NTP/system time is not valid');
  assert.equal(state.clock.quality, 'good', 'Clock quality');
  assert.match(state.clock.time, /^\d{2}:\d{2}(?::\d{2})?$/, 'Clock format');
  for (const source of sources) {
    const item = state.items?.[source];
    assert.equal(item?.quality, 'good', `${source}: sample quality`);
    assert.ok(item.value !== null && typeof item.value !== 'boolean' && String(item.value).trim() !== '' &&
      Number.isFinite(Number(item.value)), `${source}: numeric value missing`);
    const charts = state.cards.filter(card => card.sourceRef === source &&
      (historyCardIds ? historyCardIds.includes(card.id) : card.historyCount !== undefined));
    assert.ok(charts.length, `${source}: history card missing`);
    for (const chart of charts) {
      assert.equal(chart.historyCount, 10, `${source}: expected ten samples`);
      assert.equal(chart.historyStale, false, `${source}: stale history`);
      assert.ok(!chart.historyError, `${source}: history error`);
    }
  }
  if (historyCardIds) {
    for (const id of historyCardIds) assert.ok(state.cards.some(card => card.id === id), 'Configured history card missing from runtime');
  }
  if (weatherId !== undefined) {
  validateWeatherSecrets(weather);
  assert.ok(weather, 'Configured weather status missing');
  assert.equal(weather.sourceId, weatherId, 'Weather source mismatch');
  assert.equal(weather.status, 'ready', 'Weather fetch has not succeeded');
  assert.notEqual(weather.stale, true, 'Weather data is stale');
  assert.ok(weather.updatedMs > 0, 'Weather has never updated');
  assert.ok(Number.isFinite(weather.preview?.temperature), 'Weather temperature missing');
  assert.ok(weather.preview.city && weather.preview.condition, 'Weather visualization fields missing');
  const bound = state.items?.['weather.' + weatherId];
  assert.equal(bound?.quality, 'good', 'Weather card unbound or stale');
  assert.equal(bound.condition, weather.preview.condition, 'Weather condition differs from fetched data');
  assert.equal(bound.temperature, weather.preview.temperature, 'Weather temperature differs from fetched data');
  }
  return {
    revision: state.revision, clock: state.clock,
    samples: sources.map(source => ({ source, value: state.items[source].value, unit: state.items[source].unit, historyCount: 10 })),
    weather: weatherEvidence(weather)?.preview, runtime: state.application.runtime,
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
    preview: { temperature: 27, city: 'Test', condition: 'cloudy' } };
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
  for (const value of [null, true, '   ']) {
    const invalid = structuredClone(state); invalid.items.temp.value = value;
    assert.throws(() => validateLiveData(invalid, weather, opts));
  }
  const config = { pages: [{ id: 'custom', cards: [
    { id: 'chart', sourceRef: 'temp', historyMode: 'recent10' }, { kind: 'weather', sourceRef: 'weather.home' }
  ] }] };
  assert.deepEqual(deriveLiveOptions(config), { sources: ['temp'], weatherId: 'home', historyCardIds: ['chart'] });
  assert.throws(() => deriveLiveOptions(config, { sources: ['unbound'] }));
  assert.throws(() => deriveLiveOptions(config, { weatherId: 'unbound' }));
  for (const count of [1, 2]) {
    const custom = { pages: Array.from({ length: count }, (_, index) => ({ id: 'page' + index, cards: [] })) };
    const customOpts = deriveLiveOptions(custom);
    assert.deepEqual(customOpts, { sources: [], weatherId: undefined, historyCardIds: [] });
    validateLiveData(state, undefined, customOpts);
  }
  for (const key of ['apiKey', 'authHeader', 'token', 'raw', 'url', 'forecast']) {
    const invalid = structuredClone(weather); invalid.preview[key] = 'must-not-appear';
    assert.throws(() => validateLiveData(state, invalid, opts));
    assert.ok(!JSON.stringify(weatherEvidence(invalid)).includes('must-not-appear'));
  }
  assert.equal(WEATHER_CONFIG_ROUTE, '/api/screen-panel/weather');
  assert.equal(weatherStatusRoute('home'), '/api/screen-panel/weather/refresh?id=home');
  const nestedSecret = { ...weather, preview: { temperature: { apiKey: 'nested-secret' } } };
  assert.ok(!JSON.stringify(weatherEvidence(nestedSecret)).includes('nested-secret'));
  assert.throws(() => validateLiveData(state, { ...weather, stale: true }, opts));
  const missingChart = structuredClone(state); missingChart.cards[0].id = 'other';
  assert.throws(() => validateLiveData(missingChart, weather, { ...opts, historyCardIds: ['chart'] }));
  // The existing host unittest invokes this entry point. Keep the API and
  // browser script contracts under that same offline regression gate.
  for (const filename of ['test-screen-panel-device.mjs', 'test-screen-panel-browser-device.mjs']) {
    const result = spawnSync(process.execPath, [fileURLToPath(new URL(filename, import.meta.url)), '--contract-only'], { encoding: 'utf8', timeout: 15000 });
    assert.equal(result.status, 0, `${filename}: ${result.stdout}${result.stderr}`);
  }
  console.log('PASS: live screen data contract (one valid + ten failure cases; dynamic pages, binding and redaction regressions)');
} else if (option('url')) {
  const base = new URL(option('url'));
  assert.ok(['http:', 'https:'].includes(base.protocol) && !base.username && !base.password);
  const destination = option('report');
  assert.ok(destination, 'Use --url=http://device --report=file.json [--sources=a,b --weather=id]');
  let opts;
  const report = { at: new Date().toISOString(), origin: base.origin, passed: false, mode: 'read-only', checks: [] };
  let token = '';
  async function get(route) {
    const started = Date.now();
    const check = { route, method: 'GET' }; report.checks.push(check);
    const response = await fetch(base.origin + route, { headers: { Authorization: 'Bearer ' + token }, signal: AbortSignal.timeout(15000), redirect: 'error' });
    Object.assign(check, { status: response.status, milliseconds: Date.now() - started });
    assert.equal(response.status, 200, route);
    const json = await response.json(); assert.equal(json.success, true, route);
    return json.data ?? json;
  }
  try {
    const response = await fetch(base.origin + '/api/auth/login', { method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'X-FastBee-Api-Client': 'screen-live-data-test' },
      body: new URLSearchParams({ username: process.env.FASTBEE_USERNAME || 'admin', password: process.env.FASTBEE_PASSWORD || 'admin123' }),
      signal: AbortSignal.timeout(15000), redirect: 'error' });
    report.checks.push({ route: '/api/auth/login', method: 'POST', status: response.status });
    const login = await response.json(); token = login.sessionId ?? login.data?.sessionId;
    assert.ok(response.ok && token, 'Login failed');
    const panel = await get('/api/screen-panel/config');
    opts = deriveLiveOptions(panel.config, {
      ...(option('sources') !== undefined ? { sources: option('sources').split(',') } : {}),
      ...(option('weather') !== undefined ? { weatherId: option('weather') } : {}),
    });
    report.bindings = { pageCount: panel.config.pages.length, ...opts };
    let weather;
    if (opts.weatherId !== undefined) {
      const configuration = await get(WEATHER_CONFIG_ROUTE);
      validateWeatherSecrets(configuration);
      assert.equal(configuration.enabled, true, 'Configured weather component is disabled');
      assert.equal(configuration.hasApiKey, true, 'Seniverse credential not configured');
      assert.equal(configuration.id, opts.weatherId, 'Weather component uses a different shared source');
      weather = await get(weatherStatusRoute(opts.weatherId));
      validateWeatherSecrets(weather);
    }
    const state = await get('/api/screen-panel/state');
    // Keep enough non-secret evidence to distinguish transport, stale samples,
    // weather startup and binding failures without archiving device config.
    report.observed = {
      clock: state.clock,
      samples: opts.sources.map(source => ({ source,
        quality: state.items?.[source]?.quality, value: state.items?.[source]?.value,
        unit: state.items?.[source]?.unit, updatedMs: state.items?.[source]?.updatedMs })),
      weather: weatherEvidence(weather),
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
