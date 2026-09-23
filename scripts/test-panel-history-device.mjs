/** Explicit, bounded hardware check of ten-point history across a controlled reboot. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
const option = key => process.argv.find(arg => arg.startsWith(`${key}=`))?.slice(key.length + 1);
const base = option('--url');
const sources = (option('--sources') || 'sht30_temperature,sht30_humidity').split(',');
const output = option('--report');
if (!base || !output || !process.argv.includes('--restart')) {
  throw Error('Use --url=http://device --sources=a,b --report=file.json --restart (reboots the device)');
}
let token = '';
const report = {at: new Date().toISOString(), baseUrl: base, passed: false, restartRequested: false, sources};
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function login() {
  const response = await fetch(base + '/api/auth/login', {
    method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded', 'X-FastBee-Api-Client': 'history-reboot-test'},
    body: new URLSearchParams({username: process.env.FASTBEE_USERNAME || 'admin', password: process.env.FASTBEE_PASSWORD || 'admin123'}),
    signal: AbortSignal.timeout(6000)
  });
  const data = await response.json();
  assert.ok(response.ok && data.sessionId, 'Authentication failed');
  token = data.sessionId;
}
async function get(route) {
  const response = await fetch(base + route, {headers: {Authorization: 'Bearer ' + token}, signal: AbortSignal.timeout(6000)});
  assert.equal(response.status, 200, route);
  const json = await response.json();
  assert.notEqual(json.success, false, route);
  return json.data ?? json;
}
async function histories() {
  const result = {};
  for (const source of sources) {
    result[source] = await get('/api/periph-exec/history?sourceRef=' + encodeURIComponent(source));
    await delay(250);
  }
  return result;
}
try {
  await login();
  const deadline = Date.now() + 45000;
  do {
    report.before = await histories();
    if (Object.values(report.before).every(h => h.count === 10 && h.persistence?.lastFlushSucceeded)) break;
    await delay(1500);
  } while (Date.now() < deadline);
  for (const source of sources) {
    const history = report.before[source];
    assert.equal(history.count, 10, `${source}: collect at least ten real samples first`);
    // Continuous acquisition can dirty the ring immediately after a periodic
    // flush. The controlled restart flushes history before reboot; verify the
    // newest observed point after restart instead of waiting for an idle gap.
    assert.ok(history.persistence.lastFlushSucceeded, `${source}: no successful persistence yet`);
    assert.ok(history.points.every(p => Number.isFinite(p.value) && p.bootId), `${source}: invalid samples`);
  }
  const reboot = await fetch(base + '/api/system/restart', {method: 'POST', headers: {Authorization: 'Bearer ' + token}, signal: AbortSignal.timeout(10000)});
  assert.ok(reboot.ok, 'Restart rejected');
  report.restartRequested = true;
  const reconnectDeadline = Date.now() + 120000;
  let lastError;
  do {
    await delay(2000);
    try {
      await login();
      const after = await histories();
      if (!sources.every(source => after[source]?.points?.some(p => p.restored))) continue;
      report.after = after;
      break;
    } catch (error) { lastError = error.message; }
  } while (Date.now() < reconnectDeadline);
  assert.ok(report.after, 'No restored points after restart: ' + (lastError || 'deadline'));
  for (const source of sources) {
    const before = report.before[source];
    const after = report.after[source];
    assert.equal(after.count, 10, `${source}: incorrect restored ring size`);
    const retained = after.points.filter(p => p.restored && before.points.some(old => old.seq === p.seq && old.bootId === p.bootId && old.value === p.value));
    assert.ok(retained.length > 0, `${source}: restored values/sequence/boot identity differ`);
    const newest = before.points.reduce((latest, point) => point.seq > latest.seq ? point : latest);
    assert.ok(retained.some(point => point.seq === newest.seq && point.bootId === newest.bootId && point.value === newest.value),
      `${source}: newest observed sample was not flushed across controlled restart`);
    assert.equal(after.sourceRevision, before.sourceRevision);
    assert.equal(after.unit, before.unit);
  }
  report.passed = true;
  console.log('PASS: ten-point history retained original values, sequence and boot identity across reboot');
} catch (error) {
  report.error = error.message;
  process.exitCode = 1;
  console.error(error.message);
} finally {
  fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
}
