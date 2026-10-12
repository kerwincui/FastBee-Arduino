'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const session = '0123456789abcdef0123456789abcdef';
const revision = '1234567890abcdef'.repeat(4);

function setup(options = {}) {
    const calls = [], confirmations = [], staged = new Map();
    const app = {registerModule(_name, methods) {Object.assign(this, methods);}},
        document = {getElementById: () => null, querySelectorAll: () => []};
    const preview = {session, revision, phase: 'reviewed', valid: true, activation: 'next_boot',
        requiresConfirmation: false, issues: [], ...options.preview};
    const ctx = {AppState: app, document, window: {}, console, TextEncoder, TextDecoder,
        URL, setTimeout, clearTimeout, setInterval, clearInterval, Notification: {},
        confirm: message => {confirmations.push(message); return options.confirm !== false;},
        apiGetFresh: async url => {calls.push({url}); return {success: true, data: options.state || {phase: 'empty'}};},
        apiPost: async (url, body) => {
            calls.push({url, body});
            if (options.fail === url) throw new Error('response lost');
            if (url.endsWith('/begin')) return {success: true, data: {phase: 'receiving', session}};
            if (url.endsWith('/check-original')) return {success: true, data: {session, name: body.name,
                matched: options.originalMatches !== false, total: Number(body.total), checked: Number(body.offset) + Buffer.byteLength(body.content)}};
            if (url.endsWith('/chunk')) {
                const previous = staged.get(body.name) || '';
                assert.equal(Number(body.offset), Buffer.byteLength(previous));
                assert.ok(Buffer.byteLength(body.content) <= 4096);
                assert.ok(!/^[\uDC00-\uDFFF]/.test(body.content));
                assert.ok(!/[\uD800-\uDBFF]$/.test(body.content));
                staged.set(body.name, previous + body.content);
                return {success: true, data: {session, name: body.name, received: Buffer.byteLength(previous + body.content)}};
            }
            if (url.endsWith('/preview')) return {success: true, data: preview};
            if (url.endsWith('/prepare')) return {success: true, data: {phase: 'prepared', session,
                revision: options.preparedRevision || revision, applied: false, restartRequired: true}};
            if (url.endsWith('/discard')) return {success: true, data: {phase: 'empty'}};
            if (url.endsWith('/quarantine')) return {success: true, data: {phase: 'empty',orphanRetained:true}};
            throw new Error('Unexpected mutation: ' + url);
        }};
    vm.createContext(ctx);
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../../web-src/modules/runtime/device-config-transfer.js'), 'utf8'), ctx);
    app._setConfigTransferStatus = value => {ctx.status = value;};
    app._configTransferFetch = async () => ({ok: true, text: async () => JSON.stringify(options.target || {
        deviceId: 'target-device', productNumber: 7, userId: 'target-user', developerModeEnabled: true,
        deviceName: 'Old', futureProtected: {version: 3}
    })});
    return {app, calls, confirmations, staged, ctx};
}
const entry = () => ({name: 'peripherals.json', content: '{"peripherals":[]}'});

function lazySetup() {
    const loads = [], actions = [], statuses = [], timers = new Map();
    const app = {currentPage: 'device', registerModule(_name, methods) {
        Object.assign(this, methods); this.setupDeviceConfigEvents = () => {};
    }};
    const ctx = {AppState: app, console, setTimeout(fn) {timers.set(1, fn); return 1;},
        clearTimeout(id) {timers.delete(id);}};
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../../web-src/modules/runtime/device-config.js'), 'utf8'), ctx);
    app._loadModule = (name, callback) => loads.push({name, callback});
    app._setConfigTransferStatus = message => statuses.push(message);
    const ready = action => {
        app._configTransferReady = true;
        app[action] = (...args) => actions.push({action, args});
        loads.at(-1).callback();
    };
    return {app, loads, actions, statuses, timers, ready};
}

test('device landing module defers transfer and preserves the first action arguments', async () => {
    for (const action of ['exportDeviceConfigBundle', 'importDeviceConfigBundle', 'checkConfigMigration', 'discardConfigMigration']) {
        const s = lazySetup();
        assert.equal(s.app._importMigrationBundle, undefined);
        const work = s.app[action](true);
        assert.equal(s.loads.length, 1); assert.equal(s.loads[0].name, 'device-config-transfer');
        assert.equal(s.actions.length, 0);
        s.ready(action); await work;
        assert.equal(s.actions.length, 1); assert.equal(s.actions[0].action, action);
        assert.deepEqual(s.actions[0].args, action.endsWith('Bundle') ? [true] : []);
        assert.equal(s.timers.size, 0); assert.equal(s.app._configTransferLoading, false);
    }
});

test('duplicate transfer clicks while loading cannot enqueue extra operations', async () => {
    const s = lazySetup(), first = s.app.importDeviceConfigBundle(true);
    await s.app.discardConfigMigration();
    assert.equal(s.loads.length, 1);
    s.ready('importDeviceConfigBundle'); await first;
    assert.equal(s.actions.length, 1);
});

test('late transfer module delivery after navigation or timeout cannot start an operation', async () => {
    for (const mode of ['navigation', 'timeout', 'incomplete']) {
        const s = lazySetup(), pending = s.app.importDeviceConfigBundle(true);
        if (mode === 'navigation') s.app._deviceConfigEpoch = 1;
        if (mode === 'timeout') {s.timers.get(1)(); await pending;}
        if (mode === 'incomplete') s.loads[0].callback();
        else s.ready('importDeviceConfigBundle');
        await pending;
        assert.equal(s.actions.length, 0); assert.equal(s.app._configTransferLoading, false);
        if (mode !== 'navigation') assert.ok(s.statuses.at(-1).includes(mode === 'timeout' ? '超时' : '不完整'));
    }
});

test('bundle stages bytes, reviews, and prepares once without active import or reboot', async () => {
    const s = setup(), result = await s.app._importMigrationBundle([entry()]);
    assert.equal(result.phase, 'prepared');
    assert.equal(s.staged.get('peripherals.json'), entry().content);
    assert.equal(s.calls.filter(call => call.url.endsWith('/prepare')).length, 1);
    assert.ok(s.calls.every(call => call.url.startsWith('/api/config/migration')));
    assert.equal(s.confirmations.length, 1);
});
test('unsupported and duplicate migration files fail before reading or starting a session', async () => {
    for (const entries of [[], [entry(), entry()], ...['network.json','protocol.json','users.json','auth.json','other.json'].map(name => [{name,content:'{}'}])]) {
        const s = setup();
        await assert.rejects(s.app._importMigrationBundle(entries));
        assert.deepEqual(s.calls, []);
    }
    const s = setup();
    for (const name of ['network.json','protocol.json','users.json','auth.json','other.json']) {
        const preview = s.app._buildConfigTransferDependencyPreview([{name,content:'{"password":"PRIVATE_VALUE"}'}]);
        assert.ok(preview.errors.length > 0);
        assert.ok(!JSON.stringify(preview).includes('PRIVATE_VALUE'));
    }
});
test('target identity and unknown protected fields survive source identity import', async () => {
    const s = setup(), source = {name: 'device.json', content: JSON.stringify({deviceId: 'source',
        productNumber: 99, userId: 'other', developerModeEnabled: false, futureProtected: {}, deviceName: 'New'})};
    const original = source.content;
    await s.app._importMigrationBundle([source]);
    const stored = JSON.parse(s.staged.get('device.json'));
    assert.deepEqual(stored, {deviceId: 'target-device', productNumber: 7, userId: 'target-user',
        developerModeEnabled: true, deviceName: 'New', futureProtected: {version: 3}});
    assert.equal(source.content, original);
});
test('UTF8 offsets preserve Chinese and supplementary characters at chunk edges', async () => {
    const s = setup(), content = JSON.stringify({peripherals: [], note: '汉😀'.repeat(1600)});
    await s.app._importMigrationBundle([{name: 'peripherals.json', content}]);
    assert.equal(s.staged.get('peripherals.json'), content);
});
test('occupied journal never starts another upload', async () => {
    const s = setup({state: {phase: 'prepared', session}});
    await assert.rejects(s.app._importMigrationBundle([entry()]), /先查询/);
    assert.equal(s.calls.length, 1);
});
test('incomplete unprepared journal is explicitly preserved with reviewed digest and no deletion or retry', async () => {
    const options={state:{phase:'incomplete',canQuarantine:true,orphanRevision:revision}};
    const s=setup(options);await s.app.discardConfigMigration();
    assert.equal(s.calls.length,2);assert.equal(s.calls[1].url,'/api/config/migration/quarantine');
    assert.equal(s.calls[1].body.revision,revision);assert.equal(s.calls[1].body.confirm,'QUARANTINE');
    assert.ok(s.ctx.status.includes('原证据未删除'));assert.equal(s.confirmations.length,1);
    for(const next of [{...options,confirm:false},{...options,fail:'/api/config/migration/quarantine'},
        {state:{phase:'incomplete',canQuarantine:false}},{state:{phase:'blocked',canQuarantine:true,orphanRevision:revision}}]) {
        const failure=setup(next);await failure.app.discardConfigMigration();
        assert.ok(failure.calls.filter(call=>call.url.endsWith('/quarantine')).length<=1);
        assert.ok(!failure.calls.some(call=>call.url.endsWith('/discard')));
    }
});
test('server reference rejection retains staged data and never prepares', async () => {
    const s = setup({preview: {valid: false, error: 'MIGRATION_DISPLAY_REFERENCE_MISSING'}});
    await assert.rejects(s.app._importMigrationBundle([entry()]), /MIGRATION_DISPLAY_REFERENCE_MISSING/);
    assert.ok(!s.calls.some(call => call.url.endsWith('/prepare') || call.url.endsWith('/discard')));
});
test('each returned risk requires confirmation and only reviewed bits are sent', async () => {
    const s = setup({preview: {requiresConfirmation: true, issues: [
        {severity: 'warning-requires-confirmation', code: 'PIN_STRAPPING_ACK_REQUIRED', message: 'boot pin'},
        {severity: 'warning-requires-confirmation', code: 'PIN_UART0_ACTIVE', message: 'console'}]}});
    await s.app._importMigrationBundle([entry()]);
    assert.equal(s.confirmations.length, 4);
    assert.equal(s.calls.find(call => call.url.endsWith('/prepare')).body.acknowledgements, '5');
});
test('unknown risks and cancellation stop before ready and preserve evidence', async () => {
    for (const options of [{confirm: false}, {preview: {requiresConfirmation: true,
        issues: [{severity: 'warning-requires-confirmation', code: 'NEW_RISK'}]}}]) {
        const s = setup(options);
        await assert.rejects(s.app._importMigrationBundle([entry()]));
        assert.ok(!s.calls.some(call => call.url.endsWith('/prepare') || call.url.endsWith('/discard')));
    }
});

test('dormant conflicts are displayed and never prepared even after acknowledgement', async () => {
    const s = setup({preview: {issues: [{severity: 'warning', code: 'DORMANT_GPIO_CONFLICT', ownerId: 'output-a', message: 'pin collision'}]}});
    await assert.rejects(s.app._importMigrationBundle([entry()]), /output-a.*DORMANT_GPIO_CONFLICT/);
    assert.ok(!s.calls.some(call => call.url.endsWith('/prepare')));
});

test('preserved target rules are compared with immutable baseline before uploading', async () => {
    const s = setup(), original = {name: 'periph_exec.json', content: '汉😀'.repeat(900)};
    await s.app._importMigrationBundle([entry()], [original, {name: 'rule_scripts.json', content: ''}]);
    const checks = s.calls.filter(call => call.url.endsWith('/check-original'));
    assert.equal(checks.filter(call => call.body.name === original.name).map(call => call.body.content).join(''), original.content);
    assert.equal(checks.at(-1).body.total, '0');
    assert.ok(s.calls.indexOf(checks.at(-1)) < s.calls.findIndex(call => call.url.endsWith('/chunk')));
    for (const options of [{originalMatches: false}, {fail: '/api/config/migration/check-original'}]) {
        const failed = setup(options);
        await assert.rejects(failed.app._importMigrationBundle([entry()], [original]));
        assert.ok(!failed.calls.some(call => /\/(chunk|prepare|discard)$/.test(call.url)));
    }
});

const peripheralEntry = peripherals => ({name: 'peripherals.json', content: JSON.stringify({peripherals})});
const gpio = (id, pin) => ({id, name: id, type: 1, enabled: true, pins: [pin], params: {ioSource: 0}});
const board = {pins: [{gpio: 18, policy: 'recommended'}, {gpio: 19, policy: 'conditional'}, {gpio: 6, policy: 'hard_forbidden'}]};
const catalog = {activeProfileSupported: true, activeProfileId: 'board-a', compiledChip: 'esp32', profiles: [
    {id:'board-a', version:'1', active:true, displayName:'Board A'}]};

test('reusable plan removes known secrets including encoded action JSON and keeps logical bindings', () => {
    const s = setup(), entries = [peripheralEntry([{...gpio('sensor', 2),type:38, params:{driver:'ADS1115',password:'PRIVATE_PASS'}}]),
        {name:'periph_exec.json',content:JSON.stringify({rules:[{id:'rule',enabled:true,actions:[{actionType:19,targetPeriphId:'sensor',
            actionValue:JSON.stringify({channelField:'voltage',api_key:'PRIVATE_KEY'})}]}]})}];
    const before = JSON.stringify(entries), plan = s.app._buildReusableConfigPlan(entries,catalog);
    assert.equal(JSON.stringify(entries),before);
    assert.ok(!JSON.stringify(plan).includes('PRIVATE_'));
    assert.equal(plan.scope,'reusable-plan');assert.equal(plan.sanitized,true);
    assert.equal(plan.sanitization.clearedFields,2);assert.equal(plan.sanitization.customTextReviewRequired,true);
    assert.equal(plan.exportedAt,undefined);
    assert.equal(JSON.stringify(plan.requirements.driverIds),'["ADS1115"]');
    assert.equal(JSON.parse(plan.files[0].content).peripherals[0].enabled,false);
    const rule=JSON.parse(plan.files[1].content).rules[0];
    assert.equal(rule.id,'rule');assert.equal(rule.enabled,false);assert.equal(rule.actions[0].targetPeriphId,'sensor');
});

test('reusable plan rejects identities, opaque scripts/sends, unknown boards, and missing wiring', () => {
    const s=setup();
    for(const name of ['device.json','network.json','protocol.json','auth.json','users.json','rule_scripts.json','ir_keys.json'])
        assert.throws(()=>s.app._buildReusableConfigPlan([entry(),{name,content:'{}'}],catalog));
    for(const type of [10,15,29]) assert.throws(()=>s.app._buildReusableConfigPlan([entry(),
        {name:'periph_exec.json',content:JSON.stringify({rules:[{id:'r',enabled:false,actions:[{actionType:type,actionValue:'PRIVATE'}]}]})}],catalog));
    assert.throws(()=>s.app._buildReusableConfigPlan([entry()],{...catalog,activeProfileSupported:false}));
    assert.throws(()=>s.app._buildReusableConfigPlan([{name:'features.json',content:'{"features":{}}'}],catalog));
});

test('reusable plan file decoding retains mandatory wiring review through mixed selections', async () => {
    const s=setup(), bundle=s.app._buildReusableConfigPlan([peripheralEntry([gpio('out', 18)])],catalog);
    const files=await s.app._readSelectedConfigEntries([{name:'plan.json',text:async()=>JSON.stringify(bundle)},
        {name:'features.json',text:async()=>'{}'}]);
    assert.equal(files.requiresBoardMigration,true);assert.equal(files.length,2);
    assert.equal(files[0].planRequirements.sourceBoard.id,'board-a');
    assert.equal(files[1].planRequirements,undefined);
    assert.match(s.app._buildConfigTransferDependencyPreview(files.slice(0,1)).lines.join(';'),/board-a/);
    assert.ok(!s.app._buildConfigTransferDependencyPreview(files.slice(1)).lines.join(';').includes('board-a'));
    await assert.rejects(s.app._readSelectedConfigEntries([{name:'plan.json',text:async()=>JSON.stringify({...bundle,version:2})}]));
    for (const requirements of [undefined, {...bundle.requirements,sourceBoard:{id:'<script>',version:'1',chip:'ESP32'}},
        {...bundle.requirements,driverIds:['x'.repeat(97)]}, {...bundle.requirements,actionTypes:[1.5]},
        {...bundle.requirements,targetPreflightRequired:false}]) {
        await assert.rejects(s.app._readSelectedConfigEntries([{name:'plan.json',text:async()=>JSON.stringify({...bundle,requirements})}]));
    }
    assert.equal(s.calls.length,0,'metadata parsing must not make target requests');
});

test('plan exporter omits private files before fetching, verifies original bytes, and always wraps single file', async () => {
    for(const changed of [false,true]) {
        const s=setup(), reads=[],downloads=[],errors=[];
        const original=peripheralEntry([gpio('output',2)]).content;
        s.ctx.apiGetFresh=async url=>({success:true,data:url==='/api/boards/profiles'?catalog:
            {files:[{name:'peripherals.json'},{name:'network.json'},{name:'device.json'},{name:'auth.json'}]}});
        s.app._showConfigTransferModal=async items=>items;
        s.app._configTransferFetch=async url=>{reads.push(url);return {ok:true,text:async()=>changed&&reads.length===2?'{}':original};};
        s.ctx.window.location={origin:'http://fixture.invalid'};
        s.ctx.Blob=Blob;s.ctx.setTimeout=()=>{};
        s.ctx.URL=class extends URL {static createObjectURL(blob){downloads.push(blob);return 'blob:fixture';}static revokeObjectURL(){}};
        s.ctx.document.createElement=()=>({click(){},remove(){}});s.ctx.document.body={appendChild(){}};
        s.ctx.Notification={success(){},error:message=>errors.push(message)};
        await s.app.exportDeviceConfigBundle(true);
        assert.equal(reads.length,2);assert.ok(reads.every(url=>new URL(url,'http://fixture.invalid').searchParams.get('name')==='peripherals.json'));
        assert.equal(downloads.length,changed?0:1);assert.equal(errors.length,changed?1:0);
        if(!changed) assert.equal(JSON.parse(await downloads[0].text()).scope,'reusable-plan');
        assert.deepEqual(s.calls,[]);
    }
});

test('board remap preserves logical references and source bytes while pausing all rules and peripherals', () => {
    const s = setup(), entries = [peripheralEntry([gpio('output', 2), {...gpio('input', 3), params: {pairedInputId: 'output'}}]),
        {name: 'periph_exec.json', content: JSON.stringify({rules: [{id: 'rule', enabled: true, actions: [{actionType: 0, targetPeriphId: 'output'}]}]})},
        {name: 'rule_scripts.json', content: JSON.stringify({rules: [{id: 'script', enabled: true, scriptContent: 'preserved'}]})}];
    const before = JSON.stringify(entries), model = s.app._boardMigrationModel(entries);
    const output = s.app._applyBoardMigrationPins(model, new Map([[2,18],[3,19]]), board);
    assert.equal(JSON.stringify(entries), before);
    const peripherals = JSON.parse(output[0].content).peripherals;
    assert.deepEqual(peripherals.map(p => p.pins), [[18],[19]]);
    assert.ok(peripherals.every(p => p.enabled === false));
    assert.equal(peripherals[1].params.pairedInputId, 'output');
    assert.equal(JSON.parse(output[1].content).rules[0].actions[0].targetPeriphId, 'output');
    assert.equal(JSON.parse(output[1].content).rules[0].enabled, false);
    assert.equal(JSON.parse(output[2].content).rules[0].scriptContent, 'preserved');
    assert.equal(JSON.parse(output[2].content).rules[0].enabled, false);
});

test('board remap keeps screen sentinels and provider address/channel/masks untouched', () => {
    const s = setup(), entries = [peripheralEntry([{...gpio('screen', 2), type: 56,
        params: {pins: {mosi: 3, rst: -1, rgbDataPins: Array(16).fill(255)}, controlBacklightMask: 2}},
        {...gpio('provider', 3), params: {address: 2, channel: 3, providerId: 'bus'}}])];
    const model = s.app._boardMigrationModel(entries);
    const result = JSON.parse(s.app._applyBoardMigrationPins(model, new Map([[2,18],[3,19]]), board)[0].content).peripherals;
    assert.equal(result[0].params.pins.mosi, 19);
    assert.equal(result[0].params.pins.rst, -1);
    assert.deepEqual(result[0].params.pins.rgbDataPins, Array(16).fill(255));
    assert.equal(result[0].params.controlBacklightMask, 2);
    assert.deepEqual(result[1].params, {address: 2, channel: 3, providerId: 'bus'});
});

test('partial/merged/forbidden mappings leave the entire local draft unchanged', () => {
    const s = setup();
    for (const mapping of [new Map([[2,18]]), new Map([[2,18],[3,18]]), new Map([[2,18],[3,6]])]) {
        const model = s.app._boardMigrationModel([peripheralEntry([gpio('a',2),gpio('b',3)])]);
        const before = JSON.stringify(Array.from(model.documents));
        assert.throws(() => s.app._applyBoardMigrationPins(model, mapping, board));
        assert.equal(JSON.stringify(Array.from(model.documents)), before);
    }
});

test('board migration rejects raw GPIO scripts, legacy wiring and unknown named pins', () => {
    const s = setup();
    for (const action of [{actionType: 15, actionValue: 'GPIO 2 HIGH'},
        {actionType: 19, actionValue: '{"periphId":"a","nested":{"pin":2}}'}]) {
        assert.throws(() => s.app._boardMigrationModel([peripheralEntry([gpio('a',2)]),
            {name: 'periph_exec.json', content: JSON.stringify({rules: [{enabled: true, actions: [action]}]})}]));
    }
    assert.throws(() => s.app._boardMigrationModel([peripheralEntry([{...gpio('screen',2),type:56,params:{pins:{futurePin:3}}}])]));
    assert.throws(() => s.app._boardMigrationModel([entry(), entry()]));
    assert.throws(() => s.app._boardMigrationModel([entry(), {name:'network.json',content:'{}'}]));
});
test('lost mutation responses are never retried or followed by cleanup', async () => {
    for (const phase of ['begin', 'chunk', 'prepare']) {
        const url = '/api/config/migration/' + phase, s = setup({fail: url});
        await assert.rejects(s.app._importMigrationBundle([entry()]));
        assert.equal(s.calls.filter(call => call.url === url).length, 1);
        assert.ok(!s.calls.some(call => call.url.endsWith('/discard')));
    }
});
test('ready response must echo the exact reviewed digest', async () => {
    const s = setup({preparedRevision: '0'.repeat(64)});
    await assert.rejects(s.app._importMigrationBundle([entry()]), /准备结果未确认/);
});
test('explicit cleanup is session-bound and never touches an applying journal', async () => {
    const s = setup({state: {phase: 'prepared', session}});
    await s.app.discardConfigMigration();
    const request = s.calls.find(call => call.url.endsWith('/discard'));
    assert.equal(request.body.session, session); assert.equal(request.body.confirm, 'DISCARD');
    const blocked = setup({state: {phase: 'applying', session}});
    await blocked.app.discardConfigMigration();
    assert.ok(!blocked.calls.some(call => call.url.endsWith('/discard')));
});
