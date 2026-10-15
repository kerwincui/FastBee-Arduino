'use strict';

// Exercise the actual public page importer with one selected document. The
// server double models migration acknowledgements, not backend schema parsing;
// backend single-file/session/schema/storage tests remain separate and intact.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

async function runImportCase(kind, mode) {
    const session = '1234567890abcdef'.repeat(2), revision = 'abcdef0123456789'.repeat(4);
    const definitions = {
        device: ['device.json', {deviceId: 'PRIVATE_SOURCE_ID', description: '温度😀', deviceName: 'New'}],
        screen: ['screen_panel.json', {storageVersion: 1, touchUiMigration: 2, revision: 99,
            config: {schemaVersion: 1, displayRef: 'PRIVATE_DISPLAY', pages: []}}],
        history: ['point_history.json', {schemaVersion: 1, revision: 99, enabled: true, sourceAllowlist: ['PRIVATE_SOURCE']}],
        peripheral: ['peripherals.json', {peripherals: [{id: 'PRIVATE_ID', name: 'PRIVATE_NAME',
            type: 12, enabled: false, pins: [4], params: {}}]}],
        script: ['rule_scripts.json', {rules: [{id: 'PRIVATE_ID', name: 'PRIVATE_NAME', enabled: true,
            triggerType: 1, protocolType: 0, scriptContent: '温度=${x}'}]}],
        execution: ['periph_exec.json', {rules: [{id: 'PRIVATE_ID', name: 'PRIVATE_NAME', enabled: false,
            triggers: [], actions: []}]}]
    };
    assert.ok(definitions[kind], 'known configuration kind');
    const [name, config] = definitions[kind];
    const schemaModes = ['unknown_field', 'version', 'old_layout', 'duplicate', 'nul', 'too_many', 'shape', 'too_large'];
    const rejectedSchema = schemaModes.includes(mode);
    if (mode === 'unknown_field') config.secret = 'PRIVATE_SECRET';
    if (mode === 'version') config[kind === 'screen' ? 'storageVersion' : 'schemaVersion'] = 2;
    if (mode === 'old_layout') delete config.touchUiMigration;
    if (mode === 'duplicate') config.sourceAllowlist.push('PRIVATE_SOURCE');
    if (mode === 'nul') config.sourceAllowlist = ['PRIVATE_SOURCE\0SUFFIX'];
    if (mode === 'too_many') {
        if (kind === 'history') config.sourceAllowlist = Array.from({length: 25}, (_, index) => 's' + index);
        else config.rules = Array.from({length: 65}, (_, index) => ({...config.rules[0], id: 'rule-' + index}));
    }
    if (mode === 'shape') config[kind === 'peripheral' ? 'peripherals' : 'rules'] = {};
    if (mode === 'too_large') {
        if (kind === 'peripheral') config.peripherals[0].name = 'x'.repeat(25000);
        else config.rules[0].scriptContent = 'x'.repeat(25000);
    }
    const unicode = ['unicode', 'lost_middle_chunk', 'lost_final_chunk'].includes(mode);
    if (unicode) {
        if (kind === 'device') config.description = '汉字😀'.repeat(2300);
        else config.rules = Array.from({length: 32}, (_, index) => ({...config.rules[0],
            id: 'rule-' + index, name: '汉字😀'.repeat(8)}));
    }
    if (mode === 'local_oversize') config.description = 'x'.repeat(128 * 1024);
    const content = JSON.stringify(config), input = {files: [{name, size: Buffer.byteLength(content), text: async () => content}]};
    const button = {disabled: false}, calls = [], errors = [], successes = [], statuses = [], apiProblems = [];
    const confirmations = [], target = {deviceId: 'TARGET_ID', userId: 'TARGET_USER', productNumber: 7,
        deviceName: 'Old', description: '', futureProtected: {version: 3}};
    const expected = kind === 'device' ? JSON.stringify({...target, deviceName: config.deviceName, description: config.description}) : content;
    let staged = '', selectedSize = 0, chunkCount = 0, invalidations = 0;
    const state = {registerModule(_name, methods) {Object.assign(this, methods);}};
    const response = data => ({success: true, data});
    const ctx = {AppState: state, window: {apiInvalidateCache() {++invalidations;},
        // B4 起共享确认弹窗为异步 window.FBConfirm（Promise<boolean>）；与下方
        // 同步 confirm 桩共用同一确认记录，取消语义保持一致。
        async FBConfirm(options) {confirmations.push(options && options.message ? options.message : String(options)); return mode !== 'cancel';}},
        console: {error() {}, warn() {}, info() {}}, TextEncoder, Blob,
        document: {getElementById(id) {return id === 'dev-config-import-file' ? input : id === 'dev-config-import-btn' ? button : null;}},
        Notification: {success(text) {successes.push(text);}, error(text) {errors.push(text);}, warning(text) {errors.push(text);}},
        setTimeout() {throw new Error('migration import must not schedule retries');},
        confirm(text) {confirmations.push(text); return mode !== 'cancel';},
        // B4 起共享确认弹窗为异步 FBConfirm（Promise<boolean>）：模块内为裸调用
        // （顶层桩），页面会话代码经 window.FBConfirm（window 桩，见上）；两处
        // 与同步 confirm 桩共用同一确认记录，取消语义保持一致。
        async FBConfirm(options) {confirmations.push(options && options.message ? options.message : String(options)); return mode !== 'cancel';},
        apiPostJson() {assert.fail('page migration must not use active revisioned single-file endpoints');},
        apiGetFresh: async url => {
            calls.push({url}); assert.equal(url, '/api/config/migration');
            return response(mode === 'occupied' ? {phase: 'prepared', session} : {phase: 'empty'});
        },
        apiPost: async (url, body) => {
            calls.push({url, body});
            try {
                assert.ok(url.startsWith('/api/config/migration/'), 'no active single-file API fallback');
                if (url.endsWith('/begin')) {
                    const selection = JSON.parse(body.files);
                    assert.deepEqual(selection, [{name, size: Buffer.byteLength(expected)}]);
                    selectedSize = selection[0].size;
                    if (mode === 'lost_begin') throw new Error('begin response lost');
                    return {success: mode === 'begin_numeric' ? 1 : true, data: {
                        phase: mode === 'begin_phase' ? 'prepared' : 'receiving',
                        session: mode === 'invalid_session' ? 'INVALID' : session}};
                }
                assert.equal(body.session, session);
                if (url.endsWith('/chunk')) {
                    assert.equal(body.name, name);
                    assert.equal(Number(body.offset), Buffer.byteLength(staged));
                    assert.ok(Buffer.byteLength(body.content) <= 4096);
                    assert.ok(!/^[\uDC00-\uDFFF]/.test(body.content));
                    assert.ok(!/[\uD800-\uDBFF]$/.test(body.content));
                    staged += body.content; ++chunkCount;
                    if (mode === 'lost_first_chunk' || (mode === 'lost_middle_chunk' && chunkCount === 2)
                        || (mode === 'lost_final_chunk' && Buffer.byteLength(staged) === selectedSize)) throw new Error('staging response lost');
                    if (mode === 'http400' || mode === 'http503') throw Object.assign(new Error('staging rejected'), {status: mode === 'http400' ? 400 : 503});
                    const data = {session, name, received: Buffer.byteLength(staged)};
                    if (mode === 'chunk_session') data.session = '0'.repeat(32);
                    if (mode === 'chunk_name') data.name = 'other.json';
                    if (mode === 'chunk_size') --data.received;
                    if (mode === 'chunk_old') delete data.received;
                    return {success: mode === 'chunk_numeric' ? 1 : mode !== 'chunk_false', data};
                }
                if (url.endsWith('/preview')) {
                    assert.equal(staged, expected);
                    if (mode === 'preview_lost') throw new Error('preview response lost');
                    const data = {session, revision, phase: 'reviewed', valid: !rejectedSchema && mode !== 'preview_rejected',
                        error: rejectedSchema ? 'MIGRATION_SCHEMA_REJECTED' : 'MIGRATION_RULE_REFERENCE', activation: 'next_boot',
                        requiresConfirmation: false, issues: []};
                    if (mode === 'preview_session') data.session = '0'.repeat(32);
                    if (mode === 'preview_revision') data.revision = 'INVALID';
                    if (mode === 'preview_phase') data.phase = 'receiving';
                    if (mode === 'preview_activation') data.activation = 'immediate';
                    if (mode === 'preview_issues') delete data.issues;
                    if (mode === 'unknown_risk' || mode === 'warning') {
                        data.requiresConfirmation = true;
                        data.issues = (mode === 'unknown_risk' ? ['NEW_RISK'] : [
                            'PIN_STRAPPING_ACK_REQUIRED', 'PIN_BOARD_RESOURCE_ACK_REQUIRED', 'PIN_UART0_ACTIVE', 'ADC2_WIFI_ACK_REQUIRED'
                        ]).map(code => ({code, severity: 'warning-requires-confirmation', message: code}));
                    }
                    return response(data);
                }
                if (url.endsWith('/prepare')) {
                    assert.equal(body.revision, revision); assert.equal(body.confirm, 'PREPARE');
                    assert.equal(body.acknowledgements, mode === 'warning' ? '15' : '0');
                    if (mode === 'prepare_lost') throw new Error('prepare response lost');
                    const data = {phase: 'prepared', session, revision, applied: false, restartRequired: true};
                    if (mode === 'prepare_phase') data.phase = 'applied';
                    if (mode === 'prepare_session') data.session = '0'.repeat(32);
                    if (mode === 'prepare_revision') data.revision = '0'.repeat(64);
                    if (mode === 'prepare_applied') data.applied = true;
                    if (mode === 'prepare_restart') data.restartRequired = false;
                    return {success: mode === 'prepare_numeric' ? 1 : mode !== 'prepare_false', data};
                }
                assert.fail('unexpected mutation, including cleanup/reboot: ' + url);
            } catch (error) {
                if (error instanceof assert.AssertionError) apiProblems.push(error);
                throw error;
            }
        }};
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../../web-src/modules/runtime/device-config-transfer.js'), 'utf8'), ctx);
    assert.equal(state._importConfigTransferEntry, undefined, 'retired private single-file importer must not be published');
    state._setConfigTransferStatus = text => statuses.push(text);
    state._configTransferFetch = async url => {
        assert.equal(url, '/api/config/transfer/export?name=device.json');
        return {ok: true, text: async () => JSON.stringify(target)};
    };
    // Real modal DOM/accessibility and explicit checkbox consent have dedicated
    // browser tests; this transport harness preserves the selected entry objects.
    state._showConfigTransferModal = async (items, purpose, dependencyPreview) => {
        assert.equal(purpose, 'import');
        assert.ok(!JSON.stringify(items.map(item => item.preview)).includes('PRIVATE'));
        assert.equal(dependencyPreview(items).errors.length, 0);
        return items;
    };
    if (mode === 'preview') {
        const summary = JSON.stringify(state._buildConfigTransferPreview({name, content}));
        assert.ok(!summary.includes('PRIVATE') && !summary.includes('${x}'));
        if (kind === 'script') assert.match(summary, /下次启动应用.*暂存不改变运行中的转换/);
        if (kind === 'screen' || kind === 'history') assert.equal(state._detectConfigFileName('custom.json', config), name);
        return;
    }
    await state.importDeviceConfigBundle();
    assert.deepEqual(apiProblems, []);
    assert.equal(button.disabled, false); assert.equal(state._configMigrationBusy, false);
    assert.equal(JSON.stringify(config), content, 'migration must not mutate input data');
    assert.ok(calls.every(call => !/\/(discard|quarantine|restart|reboot|import-chunk)$/.test(call.url)));
    const posts = calls.filter(call => call.body), begins = posts.filter(call => call.url.endsWith('/begin'));
    const previews = posts.filter(call => call.url.endsWith('/preview')), prepares = posts.filter(call => call.url.endsWith('/prepare'));
    assert.ok(begins.length <= 1 && previews.length <= 1 && prepares.length <= 1, 'never replay a migration phase');
    if (mode === 'local_oversize') {
        assert.equal(calls.length, 0); assert.equal(invalidations, 0); assert.equal(errors.length, 1); return;
    }
    assert.equal(invalidations, 1);
    if (['success', 'unicode', 'warning'].includes(mode)) {
        assert.equal(errors.length, 0); assert.equal(successes.length, 1); assert.equal(prepares.length, 1);
        assert.equal(staged, expected); assert.match(successes[0], /等待下次重启/);
        assert.equal(confirmations.length, mode === 'warning' ? 6 : 1);
        if (unicode) assert.ok(chunkCount > 2);
        return;
    }
    assert.equal(successes.length, 0); assert.equal(errors.length, 1);
    assert.match(errors[0], /迁移状态.*未自动重试、清理或重启/);
    assert.equal(statuses.at(-1), '配置导入失败：' + errors[0]);
    if (mode === 'occupied') {assert.equal(posts.length, 0); return;}
    if (['lost_begin', 'invalid_session', 'begin_phase', 'begin_numeric'].includes(mode)) {
        assert.equal(posts.length, 1); assert.equal(chunkCount, 0); return;
    }
    if (mode.startsWith('chunk_') || mode.includes('_chunk') || mode.startsWith('http')) {
        assert.equal(previews.length, 0); assert.equal(prepares.length, 0);
        if (mode === 'lost_middle_chunk') assert.equal(chunkCount, 2);
        else if (mode === 'lost_final_chunk') assert.equal(staged, expected);
        else assert.equal(chunkCount, 1);
        return;
    }
    if (mode.startsWith('prepare_')) {assert.equal(prepares.length, 1); return;}
    assert.equal(previews.length, 1); assert.equal(prepares.length, 0);
    if (rejectedSchema) assert.match(errors[0], /MIGRATION_SCHEMA_REJECTED/);
}

module.exports = {runImportCase};
if (require.main === module) runImportCase(process.argv[2], process.argv[3]).catch(error => {
    console.error(error); process.exitCode = 1;
});
