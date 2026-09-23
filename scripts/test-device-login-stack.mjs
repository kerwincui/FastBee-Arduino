// Pair with a read-only serial capture of LOGIN_METRICS; no credential/token is saved.
// Successful login, one intentional rejection, then successful login resets the counter.
import fs from 'node:fs';
import path from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
const base = process.argv[2];
const output = path.resolve(process.argv[3] || 'artifacts/device-login-stack.json');
if (!base || !/^https?:\/\//.test(base)) throw new Error('Provide device HTTP(S) origin');
const username = process.env.FASTBEE_USERNAME || 'admin';
const password = process.env.FASTBEE_PASSWORD || 'admin123';
const report = { base, at: new Date().toISOString(), passed: false, attempts: [] };
try {
  for (const [name, supplied, expected] of [
    ['success-before', password, 200],
    ['one-rejected-password', password + '-intentional-stack-test', 401],
    ['success-after', password, 200],
  ]) {
    const started = Date.now();
    const response = await fetch(new URL('/api/auth/login', base), {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded', Connection: 'close' },
      body: new URLSearchParams({ username, password: supplied }),
      signal: AbortSignal.timeout(20000),
    });
    await response.arrayBuffer();
    const row = { name, status: response.status, expected, ms: Date.now() - started };
    report.attempts.push(row);
    console.log(JSON.stringify(row));
    if (response.status !== expected) throw new Error(`Unexpected login status in ${name}; stopping further attempts`);
    await delay(2000);
  }
  report.passed = true;
} catch (error) { report.error = error.message; }
finally {
  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.writeFileSync(output, JSON.stringify(report, null, 2));
  if (!report.passed) process.exitCode = 1;
}
