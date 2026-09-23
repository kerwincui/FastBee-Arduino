/** Live acceptance of panel API contracts; never dispatches peripheral actions.
 * Credentials and configuration remain in memory. --exercise-config saves the
 * identical panel layout once to exercise persistence and revision conflicts.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

const args = process.argv.slice(2);
const option = (name) => args.find((arg) => arg.startsWith(`${name}=`))?.slice(name.length + 1);
function validatePanelChoice(item) {
    assert.equal(typeof item.kind, 'string');
    assert.equal(typeof item.type, 'string');
    assert.equal(typeof item.label, 'string');
    if (item.kind === 'data' || item.kind === 'weather') {
        assert.equal(typeof item.sourceRef, 'string');
    } else if (item.kind === 'control') {
        assert.equal(typeof item.controlRef, 'string');
        assert.ok(['switch', 'action', 'number'].includes(item.type));
    } else if (item.kind === 'system') {
        assert.equal(typeof item.systemActionId, 'string');
    } else {
        assert.fail(`unsupported panel choice kind: ${item.kind}`);
    }
}
if (args.includes('--contract-only')) {
    validatePanelChoice({ kind: 'control', type: 'number', label: '目标温度', controlRef: 'remote.target_temp' });
    assert.throws(() => validatePanelChoice({ kind: 'control', type: 'text', label: '无效', controlRef: 'remote.invalid' }));
    console.log('screen-panel device contract: PASS');
    process.exit(0);
}
const base = option('--url');
if (!base || !/^https?:\/\//.test(base)) throw new Error('Use --url=http://<device> [--report=<file>] [--exercise-config]');
const origin = new URL(base).origin;
const rows = [];
let session = '';
let exercisedConfig = false;
let configExerciseOutcome = args.includes('--exercise-config') ? 'not-run' : 'not-requested';
const delay = () => new Promise((resolve) => setTimeout(resolve, 500));
async function request(route, body, authenticated = true) {
    await delay();
    const headers = {};
    if (authenticated && session) headers.Authorization = `Bearer ${session}`;
    const isLogin = route === '/api/auth/login';
    if (body !== undefined) headers['Content-Type'] = isLogin ? 'application/x-www-form-urlencoded' : 'application/json';
    if (isLogin) headers['X-FastBee-Api-Client'] = 'screen-panel-acceptance';
    const started = Date.now();
    const response = await fetch(`${origin}${route}`, {
        method: body === undefined ? 'GET' : 'POST', headers,
        body: body === undefined ? undefined : isLogin ? new URLSearchParams(body).toString() : JSON.stringify(body),
        signal: AbortSignal.timeout(15000), redirect: 'error',
    });
    const text = await response.text();
    let json;
    try { json = JSON.parse(text); } catch { json = null; }
    rows.push({ route, method: body === undefined ? 'GET' : 'POST', authenticated,
        status: response.status, milliseconds: Date.now() - started, bytes: Buffer.byteLength(text) });
    return { status: response.status, json, data: json?.data ?? json };
}
function success(result) {
    assert.equal(result.status, 200);
    assert.equal(result.json?.success, true);
    return result.data;
}

let passed = false;
try {
    const caps = success(await request('/api/system/capabilities', undefined, false));
    for (const name of ['screenPanel', 'screenUi', 'pointHistory', 'remoteResources']) {
        assert.equal(typeof caps[name], 'boolean', `missing capability ${name}`);
    }
    const login = await request('/api/auth/login', {
        username: process.env.FASTBEE_USERNAME || 'admin',
        password: process.env.FASTBEE_PASSWORD || 'admin123',
    }, false);
    assert.equal(login.status, 200, 'login status');
    session = login.json?.sessionId ?? login.data?.sessionId;
    assert.ok(session, 'login did not yield a session');
    const routes = [
        ['screenPanel', '/api/screen-panel/config'],
        ['screenPanel', '/api/screen-panel/state'],
        ['screenPanel', '/api/screen-panel/choices?pageSize=2'],
        ['pointHistory', '/api/periph-exec/history/config'],
        ['remoteResources', '/api/remote-resources/config'],
    ];
    let panel;
    for (const [cap, route] of routes) {
        const unauthorized = await request(route, undefined, false);
        assert.ok(caps[cap] ? [401, 403].includes(unauthorized.status) : [404, 501].includes(unauthorized.status), `${route}: capability/auth mismatch`);
        if (!caps[cap]) {
            const unavailable = await request(route);
            assert.ok([404, 501].includes(unavailable.status), `${route}: disabled capability must not dispatch to a parent route after login`);
            continue;
        }
        const data = success(await request(route));
        if (route === '/api/screen-panel/config') {
            panel = data;
            assert.ok(Number.isInteger(data.revision));
            assert.ok(Array.isArray(data.config?.pages));
        }
        if (route.includes('/choices')) {
            assert.ok(Array.isArray(data.items));
            assert.ok(data.items.length <= 2);
            for (const item of data.items) {
                validatePanelChoice(item);
            }
        }
        if (route === '/api/periph-exec/history/config') {
            assert.equal(typeof data.config?.enabled, 'boolean');
            assert.ok(Array.isArray(data.config?.sourceAllowlist));
        }
        if (route === '/api/remote-resources/config') {
            assert.ok(Array.isArray(data.remoteChannels));
            assert.ok(Array.isArray(data.weatherSources));
            for (const weather of data.weatherSources) {
                for (const key of ['apiKey', 'authHeader']) {
                    assert.ok(!weather[key] || /^\*{3,}$/.test(weather[key]), 'weather secrets must be redacted');
                }
            }
        }
    }
    if (caps.pointHistory) {
        assert.equal((await request('/api/periph-exec/history?sourceRef=bad%2Fref')).status, 400);
        success(await request('/api/periph-exec/history?sourceRef=device.uptime'));
    }
    if (panel) {
        const rejected = await request('/api/screen-panel/config', { expectedRevision: panel.revision, config: panel.config }, false);
        assert.ok([401, 403].includes(rejected.status), 'unauthenticated layout save');
        if (args.includes('--exercise-config')) {
            // Developer mode is an admission gate, so a disabled device may
            // reject this before schema validation. In that case no write-side
            // persistence or conflict checks are attempted.
            const invalid = structuredClone(panel.config);
            invalid.unrecognizedProperty = true;
            const invalidResult = await request('/api/screen-panel/config', {
                expectedRevision: panel.revision, config: invalid
            });
            if (invalidResult.status === 403) {
                configExerciseOutcome = 'skipped-developer-disabled';
            } else {
                assert.equal(invalidResult.status, 400);
                const saved = success(await request('/api/screen-panel/config', {
                    expectedRevision: panel.revision, config: panel.config
                }));
                assert.equal(saved.revision, panel.revision + 1);
                assert.deepEqual(saved.config, panel.config, 'same-layout save changed configuration');
                assert.equal((await request('/api/screen-panel/config', {
                    expectedRevision: panel.revision, config: panel.config
                })).status, 409);
                const restored = success(await request('/api/screen-panel/config'));
                assert.deepEqual(restored.config, panel.config);
                assert.equal(restored.persisted, true);
                exercisedConfig = true;
                configExerciseOutcome = 'completed';
            }
        }
    }
    passed = true;
} finally {
    session = '';
    const report = { baseUrl: origin, at: new Date().toISOString(), passed,
        configExerciseRequested: args.includes('--exercise-config'), exercisedConfig,
        configExerciseOutcome, checks: rows };
    const destination = option('--report');
    if (destination) {
        fs.mkdirSync(path.dirname(path.resolve(destination)), { recursive: true });
        fs.writeFileSync(destination, `${JSON.stringify(report, null, 2)}\n`);
    }
    console.log(`${origin}: ${passed ? 'PASS' : 'FAIL'} (${rows.length} requests)`);
}
