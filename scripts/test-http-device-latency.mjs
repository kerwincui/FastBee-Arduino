// Read-only, sequential HTTP diagnostics. No login or configuration mutations.
// node scripts/test-http-device-latency.mjs --url=http://192.168.5.54 --rounds=3 --gap=1500 --output=artifacts/http-latency.json
import http from 'node:http';
import https from 'node:https';
import fs from 'node:fs';
import path from 'node:path';
import { performance } from 'node:perf_hooks';
import { setTimeout as delay } from 'node:timers/promises';

const args = Object.fromEntries(process.argv.slice(2).map(arg => {
  const equal = arg.indexOf('=');
  return [arg.slice(2, equal < 0 ? undefined : equal), equal < 0 ? true : arg.slice(equal + 1)];
}));
const origin = new URL(args.url || 'http://127.0.0.1');
if (!['http:', 'https:'].includes(origin.protocol) || origin.username || origin.password) {
  throw new Error('Use an HTTP(S) origin without credentials');
}
const rounds = Number(args.rounds || 3);
const gap = Number(args.gap || 1500);
if (!Number.isInteger(rounds) || rounds < 1 || rounds > 10 || !Number.isFinite(gap) || gap < 400 || gap > 10000) {
  throw new Error('rounds must be 1..10 and gap must be 400..10000 milliseconds');
}
const output = path.resolve(args.output || 'artifacts/http-device-latency.json');
const evidence = { timestamp: new Date().toISOString(), origin: origin.origin,
  mode: 'sequential-read-only', rounds, gapMs: gap, requests: [], completed: false };
const rounded = value => value == null ? null : Math.round(value * 10) / 10;

function request(route, round) {
  return new Promise(resolve => {
    const began = performance.now();
    const sample = { round, route, status: null, bytes: 0, dnsMs: null, connectMs: null, ttfbMs: null, totalMs: null };
    let body = '';
    let settled = false;
    const finish = error => {
      if (settled) return;
      settled = true;
      sample.totalMs = rounded(performance.now() - began);
      if (error) sample.error = error.message;
      if (route === '/api/health' && !error) {
        try {
          const json = JSON.parse(body);
          const health = json.data ?? json;
          sample.memory = health.memory;
          sample.freeHeap = health.freeHeap;
          sample.uptimeMs = health.timestamp;
        } catch { sample.error = 'Health response was not JSON'; }
      }
      evidence.requests.push(sample);
      resolve(sample);
    };
    const transport = origin.protocol === 'https:' ? https : http;
    const req = transport.get(new URL(route, origin), { agent: false,
      headers: { Connection: 'close', 'Accept-Encoding': 'gzip' } }, response => {
      sample.status = response.statusCode;
      sample.ttfbMs = rounded(performance.now() - began);
      response.on('data', chunk => {
        sample.bytes += chunk.length;
        if (route === '/api/health' && body.length < 65536) body += chunk;
      });
      response.on('end', () => finish());
      response.on('error', finish);
      response.on('aborted', () => finish(new Error('Response aborted')));
    });
    req.on('socket', socket => {
      socket.on('lookup', () => { sample.dnsMs = rounded(performance.now() - began); });
      socket.on('connect', () => { sample.connectMs = rounded(performance.now() - began); });
    });
    req.setTimeout(15000, () => req.destroy(new Error('Request timeout')));
    req.on('error', finish);
  });
}

try {
  for (let round = 0; round <= rounds; ++round) {
    const health = await request('/api/health', round);
    const level = health.memory?.guardLevel;
    if (health.status !== 200 || health.error || ['SEVERE', 'CRITICAL'].includes(level)) {
      evidence.stopReason = `Health gate: ${health.error || level || health.status}`;
      break;
    }
    if (round === rounds) { evidence.completed = true; break; }
    for (const route of ['/pages/logs.html', '/css/main.css', '/api/system/capabilities']) {
      await delay(gap);
      const sample = await request(route, round + 1);
      if (sample.error || sample.status !== 200) {
        evidence.stopReason = `Request failed: ${route}`;
        break;
      }
    }
    if (evidence.stopReason) break;
    await delay(gap);
  }
} finally {
  const resources = evidence.requests.filter(row => row.route !== '/api/health');
  const timings = resources.map(row => row.totalMs).sort((a, b) => a - b);
  evidence.summary = { requests: evidence.requests.length,
    allHttp200: evidence.requests.every(row => row.status === 200 && !row.error),
    resourceMaxTotalMs: timings.at(-1) ?? null,
    resourceP95TotalMs: timings.length ? timings[Math.ceil(timings.length * 0.95) - 1] : null,
    allRequestsMaxMs: Math.max(...evidence.requests.map(row => row.totalMs)),
    note: 'Health snapshots do not exclude transient pressure between requests; correlate serial telemetry.' };
  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.writeFileSync(output, JSON.stringify(evidence, null, 2) + '\n');
  console.log(JSON.stringify({ output, completed: evidence.completed, stopReason: evidence.stopReason, ...evidence.summary }));
  if (!evidence.completed || !evidence.summary.allHttp200) process.exitCode = 1;
}
