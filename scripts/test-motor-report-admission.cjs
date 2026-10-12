'use strict';
// Offline VM tests of the actual two motor request paths. No control/device I/O.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
const done = (id, state, extra = {}) => ({ success: true, data: {
    operationId: id, pending: false, success: true, reportAdmission: state,
    reportDeliveryVerified: false, ...extra
} });
function fixture(protocol) {
    const nodes = new Map(), calls = [];
    const element = id => {
        if (!nodes.has(id)) nodes.set(id, { textContent: '', dataset: {}, style: {}, isConnected: true });
        return nodes.get(id);
    };
    let respond = () => done(1, 'queued_locally'), now = 1000;
    const app = { _activeDeviceIdx: 0, _modbusCtrlGeneration: 1, _dcLifecycleGeneration: 1 };
    const context = vm.createContext({ AppState: app, document: { getElementById: element },
        Date: { now: () => now }, Math, Number, Promise,
        setTimeout(fn, ms) { now += ms; queueMicrotask(fn); },
        apiPostSilentPriority: async (url, body) => { calls.push({ url, ...body }); return respond(body); }
    });
    context.window = context;
    vm.runInContext(fs.readFileSync(path.join(root, 'web-src/js/utils.js'), 'utf8'), context);
    const file = protocol ? 'protocol/modbus-relay-motor.js' : 'device-control/modbus-ctrl.js';
    vm.runInContext(fs.readFileSync(path.join(root, 'web-src/modules/runtime', file), 'utf8'), context);
    app._dcGetMotorParams = i => ({ slaveAddress: i + 9 });
    app._dcIsLifecycleActive = g => g === app._dcLifecycleGeneration;
    app._dcDelay = async () => true;
    const reportId = i => protocol ? 'modbus-ctrl-motor-report' : 'dc-motor-report-' + i;
    return { app, calls, context, element,
        setResponder(fn) { respond = fn; },
        request(action = 'forward', i = 0) { return protocol ? app._protocolMotorRequest({ slaveAddress: i + 9 }, action) : app._dcMotorRequest(i, action); },
        report(data, waiting) { protocol ? app._protocolMotorReport(data, waiting) : app._dcMotorReport(0, data, waiting); },
        text(i = 0) { return element(reportId(i)).textContent; },
        close() { if (protocol) app._modbusCtrlGeneration++; else app._dcLifecycleGeneration++; }
    };
}
async function main() {
    for (const protocol of [false, true]) {
        const labels = {
            queued_locally: '当时已进入本地队列', retained_locally: '当时已保留，后续结果未知',
            not_retained: '报告未保留', invalid_payload: '报告载荷无效',
            disabled: '应答报告未启用', not_produced: '未生成应答报告'
        };
        for (const [state, label] of Object.entries(labels)) {
            const h = fixture(protocol);
            h.setResponder(() => done(1, state));
            await h.request();
            assert.equal(h.text(), label + '；平台接收未确认');
            assert.equal(h.calls.length, 1, 'report admission never triggers control retry');
        }
        for (const extra of [{ reportAdmission: undefined }, { reportAdmission: 'uploaded' },
            { reportAdmission: '<img src=x onerror=alert(1)>' }, { reportAdmission: 'toString' },
            { reportAdmission: {} }, { reportAdmission: 'pending' }, { reportAdmission: 'not_applicable' },
            { reportDeliveryVerified: true }, { reportDeliveryVerified: 'false' },
            { reportDeliveryVerified: undefined }, { operationId: undefined },
            { operationId: 0 }, { operationId: '1' }, { pending: undefined }, { pending: true },
            { success: undefined }, { success: 'true' }]) {
            const h = fixture(protocol);
            h.report(done(1, 'queued_locally', extra).data);
            assert.equal(h.text(), '报告状态未确认；平台接收未确认', JSON.stringify(extra));
        }
        const legacy = fixture(protocol);
        legacy.setResponder(() => ({ success: true, data: {} }));
        if (protocol) await assert.rejects(legacy.request()); else await legacy.request();
        assert.equal(legacy.text(), '报告状态未确认；平台接收未确认');

        const pending = fixture(protocol);
        let queries = 0;
        pending.setResponder(body => {
            if (body.action !== 'operation') return { success: true, data: { operationId: 8, pending: true } };
            assert.equal(pending.text(), '等待报告结果；平台接收未确认');
            if (++queries === 1) throw { status: 503 };
            return done(8, 'retained_locally');
        });
        await pending.request();
        assert.equal(pending.text(), labels.retained_locally + '；平台接收未确认');
        assert.equal(pending.calls.filter(c => c.action === 'forward').length, 1);
        assert.equal(queries, 2, 'only operation query retried after 503');
        pending.setResponder(() => done(9, 'not_applicable'));
        await pending.request('readStatus');
        assert.equal(pending.text(), labels.retained_locally + '；平台接收未确认', 'readback cannot erase control report');

        const failed = fixture(protocol);
        failed.setResponder(() => done(1, 'not_produced', { success: false, error: 'write failed' }));
        await assert.rejects(failed.request(), /write failed/);
        assert.equal(failed.text(), labels.not_produced + '；平台接收未确认');
        failed.setResponder(() => { throw new Error('transport'); });
        await assert.rejects(failed.request(), /transport/);
        assert.equal(failed.text(), '报告状态未确认；平台接收未确认', 'new unknown request clears old terminal report');

        const mismatch = fixture(protocol);
        mismatch.setResponder(body => body.action === 'operation' ? done(10, 'queued_locally') :
            { success: true, data: { operationId: 9, pending: true } });
        await assert.rejects(mismatch.request(), /编号不匹配/);
        assert.equal(mismatch.text(), '报告状态未确认；平台接收未确认');
        const lost = fixture(protocol);
        lost.setResponder(body => {
            if (body.action === 'operation') throw { status: 404 };
            return { success: true, data: { operationId: 9, pending: true } };
        });
        await assert.rejects(lost.request(), /执行结果未知/);
        assert.equal(lost.text(), '报告状态未确认；平台接收未确认');
        assert.equal(lost.calls.length, 2);

        for (const failOld of [false, true]) {
            const overlap = fixture(protocol);
            let release, reject;
            overlap.setResponder(body => body.action === 'stop' ? done(2, 'retained_locally') :
                new Promise((resolve, fail) => { release = resolve; reject = fail; }));
            const old = overlap.request();
            await assert.rejects(overlap.request(), /正在/);
            assert.equal(overlap.text(), '等待报告结果；平台接收未确认', 'rejected duplicate does not erase in-flight state');
            await overlap.request('stop');
            if (failOld) { reject(new Error('late error')); await assert.rejects(old, /late error/); }
            else { release(done(1, 'queued_locally')); await old; }
            assert.equal(overlap.text(), labels.retained_locally + '；平台接收未确认', 'late older success/error cannot replace stop');
            assert.equal(overlap.calls.length, 2);
        }
        const stale = fixture(protocol);
        let release;
        stale.setResponder(() => new Promise(resolve => { release = resolve; }));
        const old = stale.request();
        stale.close(); stale.report(null);
        release(done(1, 'queued_locally'));
        await assert.rejects(old, /已关闭/);
        assert.equal(stale.text(), '报告状态未确认；平台接收未确认');

        if (!protocol) {
            const independent = fixture(false);
            independent.setResponder(body => body.slaveAddress === 9 ?
                new Promise(resolve => { release = resolve; }) : done(2, 'not_retained'));
            const first = independent.request('forward', 0);
            await independent.request('forward', 1);
            release(done(1, 'queued_locally')); await first;
            assert.equal(independent.text(0), labels.queued_locally + '；平台接收未确认');
            assert.equal(independent.text(1), labels.not_retained + '；平台接收未确认');
            vm.runInContext(fs.readFileSync(path.join(root, 'web-src/modules/runtime/device-control/render.js'), 'utf8'), independent.context);
            assert.match(independent.app._renderDcMotorStatusCard(0), /id="dc-motor-report-0" role="status">报告状态未确认；平台接收未确认/);
        }
    }
    const fragment = fs.readFileSync(path.join(root, 'web-src/pages/fragments/protocol-modbus-rtu.html'), 'utf8');
    assert.match(fragment, /id="modbus-ctrl-motor-report" role="status">报告状态未确认；平台接收未确认/);
    console.log('Motor report admission UI: both production request paths, historical local status, unknown contracts, stop overlap, lifecycle and no command retries.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
