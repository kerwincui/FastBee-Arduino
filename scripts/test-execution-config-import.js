'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {runImportCase} = require('../test/unit/config_migration_web_harness.cjs');

async function bundleOutcome(mode) {
    const statuses = [], errors = [], successes = [], calls = [], apiProblems = [];
    const staged = new Map();
    const session = '1234567890abcdef'.repeat(2), revision = 'abcdef0123456789'.repeat(4);
    let selection = [], prepares = 0, chunks = 0;
    let invalidations = 0;
    const input = {files: [{name: 'bundle.json', size: 10}]}, button = {disabled: false};
    const state = {registerModule(name, methods) { Object.assign(this, methods); this.setupDeviceConfigEvents = () => {}; }};
    const window = {apiInvalidateCache() { invalidations++; if (mode === 'cache-failure') throw new Error('cache failed'); }};
    const targetDevice = {deviceId: 'TARGET_ID', userId: 'TARGET_USER', productNumber: 7,
        deviceName: 'Target name', futureProtected: {version: 3}};
    const sandbox = {window, AppState: state, TextEncoder, Blob, console: {error() {}, warn() {}},
        document: {getElementById(id) { return id === 'dev-config-import-file' ? input : id === 'dev-config-import-btn' ? button : null; }},
        Notification: {error(value) { errors.push(value); }, success(value) { successes.push(value); }, warning() {}},
        confirm: () => mode !== 'review-cancel',
        // B4 起共享确认弹窗为异步 FBConfirm（Promise<boolean>），与 confirm 桩同语义。
        FBConfirm: async () => mode !== 'review-cancel',
        apiGetFresh: async url => {
            calls.push({url});
            assert.equal(url, '/api/config/migration');
            return {success: true, data: mode === 'occupied' ? {phase: 'prepared', session} : {phase: 'empty'}};
        },
        apiPost: async (url, payload) => {
            calls.push({url, payload});
            try {
                assert.ok(url.startsWith('/api/config/migration/'), 'whole import must use migration HTTP boundary');
                if (url.endsWith('/begin')) {
                    selection = JSON.parse(payload.files);
                    assert.ok(selection.length > 0 && selection.every(item => item.size > 0));
                    if (mode === 'begin-unknown') throw new Error('lost begin response');
                    return {success: true, data: {phase: 'receiving', session}};
                }
                assert.equal(payload.session, session);
                if (url.endsWith('/chunk')) {
                    const previous = staged.get(payload.name) || '';
                    assert.equal(Number(payload.offset), Buffer.byteLength(previous));
                    assert.ok(selection.some(file => file.name === payload.name));
                    assert.ok(Buffer.byteLength(payload.content) <= 4096);
                    assert.ok(!/^[\uDC00-\uDFFF]/.test(payload.content));
                    assert.ok(!/[\uD800-\uDBFF]$/.test(payload.content));
                    staged.set(payload.name, previous + payload.content);
                    ++chunks;
                    // Simulate an accepted staging write whose reply is lost.
                    // The browser cannot classify this as active config saved.
                    if (mode === 'chunk-unknown' && chunks === 2) throw new Error('lost staging response');
                    return {success: true, data: {session, name: payload.name,
                        received: Buffer.byteLength(staged.get(payload.name)) + (mode === 'chunk-mismatch' ? 1 : 0)}};
                }
                if (url.endsWith('/preview')) {
                    assert.equal(staged.size, selection.length);
                    for (const file of selection) assert.equal(Buffer.byteLength(staged.get(file.name)), file.size);
                    if (mode === 'preview-unknown') throw new Error('lost preview response');
                    return {success: true, data: {session, phase: 'reviewed', revision,
                        valid: mode !== 'preview-rejected', error: mode === 'preview-rejected' ? 'MIGRATION_RULE_REFERENCE' : '',
                        activation: 'next_boot', requiresConfirmation: false, issues: []}};
                }
                if (url.endsWith('/prepare')) {
                    assert.equal(payload.revision, revision);
                    assert.equal(payload.confirm, 'PREPARE');
                    assert.equal(payload.acknowledgements, '0');
                    ++prepares;
                    if (mode === 'prepare-unknown') throw new Error('lost prepare response');
                    return {success: true, data: {phase: 'prepared', session, revision,
                        applied: mode === 'prepare-mismatch', restartRequired: true}};
                }
                assert.fail('unexpected mutation: ' + url); // includes discard, reboot and active per-file import
            } catch (error) {
                if (error instanceof assert.AssertionError) apiProblems.push(error);
                throw error;
            }
        }};
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../web-src/modules/runtime/device-config-transfer.js'), 'utf8'), sandbox);
    const files = [{name: 'periph_exec.json', content: '{"rules":[]}'},
        {name: 'peripherals.json', content: '{"peripherals":[]}'},
        {name: 'device.json', content: '{"deviceName":"Source name","deviceId":"SOURCE_ID","userId":"SOURCE_USER"}'}];
    if (mode === 'missing-dependency') files[0].content = JSON.stringify({rules:[{enabled:false,actions:[{actionType:38,targetPeriphId:'missing'}]}]});
    if (mode === 'duplicates' || mode === 'choose-duplicate') files.push({name:'device.json',content:'{"deviceName":"Selected copy","deviceId":"IGNORED_SOURCE"}'});
    if (mode === 'unsupported') files.push({name:'network.json',content:'{}'});
    state._setConfigTransferStatus = value => statuses.push(value);
    state._readSelectedConfigEntries = async () => { if (mode === 'parse') throw new Error('invalid JSON'); return files; };
    state._showConfigTransferModal = async items => {
        if (mode === 'cancel') throw new Error('用户取消');
        return mode === 'choose-duplicate' ? [items[3]] : items;
    };
    state._configTransferFetch = async url => {
        calls.push({url});
        assert.equal(url, '/api/config/transfer/export?name=device.json');
        return {ok: true, text: async () => JSON.stringify(targetDevice)};
    };
    assert.equal(state._importConfigTransferEntry, undefined, 'retired single-file importer must not be published');
    const original = JSON.stringify(files);
    await state.importDeviceConfigBundle();
    assert.deepEqual(apiProblems, []);
    assert.equal(button.disabled, false);
    assert.equal(state._configMigrationBusy, false);
    assert.equal(JSON.stringify(files), original);
    assert.ok(calls.every(call => !/\/(discard|restart|reboot|import-chunk)$/.test(call.url)));
    assert.ok(calls.filter(call => call.url.endsWith('/begin')).length <= 1);
    assert.ok(prepares <= 1);
    if (['duplicates', 'missing-dependency', 'unsupported', 'parse', 'cancel'].includes(mode)) {
        assert.equal(calls.length,0);assert.equal(invalidations,0);assert.equal(successes.length,0);
        assert.equal(errors.length, mode === 'cancel' ? 0 : 1);
        if (mode === 'duplicates') assert.match(errors[0], /重复文件名/);
        if (mode === 'missing-dependency') assert.match(errors[0], /缺失：1/);
        if (mode === 'unsupported') assert.match(errors[0], /不接收|不支持/);
        return;
    }
    assert.equal(invalidations, 1);
    if (['success', 'cache-failure', 'choose-duplicate'].includes(mode)) {
        assert.equal(staged.size, mode === 'choose-duplicate' ? 1 : 3);
        assert.equal(successes.length, 1);assert.equal(errors.length, 0);assert.equal(prepares, 1);
        assert.match(successes[0], /等待下次重启/);
        const stored = JSON.parse(staged.get('device.json'));
        assert.deepEqual(stored, {...targetDevice, deviceName: mode === 'choose-duplicate' ? 'Selected copy' : 'Source name'});
        if (mode !== 'choose-duplicate') {
            assert.equal(staged.get('periph_exec.json'), files[0].content);
            assert.equal(staged.get('peripherals.json'), files[1].content);
        }
        return;
    }
    assert.equal(successes.length, 0);assert.equal(errors.length, 1);
    assert.match(errors[0], /迁移状态.*未自动重试、清理或重启/);
    assert.ok(!errors[0].includes('已确认保存') && !errors[0].includes('未自动回滚'));
    assert.ok(statuses.at(-1).includes(errors[0]));
    if (mode === 'occupied') {
        assert.equal(calls.length, 1);assert.equal(chunks, 0);
    } else if (mode === 'begin-unknown') {
        assert.equal(calls.at(-1).url, '/api/config/migration/begin');assert.equal(chunks, 0);
    } else if (mode === 'chunk-unknown' || mode === 'chunk-mismatch') {
        assert.equal(chunks, mode === 'chunk-unknown' ? 2 : 1);
        assert.equal(calls.at(-1).url, '/api/config/migration/chunk');
    } else if (['preview-rejected', 'preview-unknown', 'review-cancel'].includes(mode)) {
        assert.equal(calls.at(-1).url, '/api/config/migration/preview');assert.equal(prepares, 0);
        assert.equal(staged.size, 3);
        if (mode === 'preview-rejected') assert.match(errors[0], /MIGRATION_RULE_REFERENCE/);
    } else {
        assert.ok(['prepare-unknown', 'prepare-mismatch'].includes(mode));
        assert.equal(calls.at(-1).url, '/api/config/migration/prepare');assert.equal(prepares, 1);
        assert.equal(staged.size, 3);
    }
}

(async () => {
    const managedModes = ['success', 'unicode', 'lost_middle_chunk', 'http503', 'http400', 'preview_rejected', 'chunk_size', 'prepare_applied'];
    const bundleModes = ['success', 'cache-failure', 'cancel', 'parse', 'duplicates', 'choose-duplicate',
        'missing-dependency', 'unsupported', 'occupied', 'begin-unknown', 'chunk-unknown', 'chunk-mismatch',
        'preview-rejected', 'preview-unknown', 'review-cancel', 'prepare-unknown', 'prepare-mismatch'];
    for (const mode of managedModes) await runImportCase('execution', mode);
    for (const mode of bundleModes) await bundleOutcome(mode);
    console.log(`Execution configuration import: ${managedModes.length} single-selection migration + ${bundleModes.length} migration-bundle scenarios passed`);
})().catch(error => { console.error(error); process.exitCode = 1; });
