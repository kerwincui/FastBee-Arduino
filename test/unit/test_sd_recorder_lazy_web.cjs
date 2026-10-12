'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {chromium} = require('../browser/node_modules/playwright');
const schema = JSON.parse(process.env.FASTBEE_TEST_SD_RECORDER_SCHEMA);
(async () => {
    const browser = await chromium.launch({headless: true});
    try {
        const page = await browser.newPage();
        await page.setContent('<div id="periph-exec-actions"></div><div id="periph-exec-triggers"></div>' +
            '<input id="periph-exec-original-id" value="record"><input id="periph-exec-name" value="SD">' +
            ['enabled','report','record-history','panel-control'].map(id => '<input type="checkbox" id="periph-exec-' + id + '">').join('') +
            '<div id="periph-exec-error"></div><details id="pe-template-wizard"></details>');
        await page.evaluate(() => {
            window.escapeHtml = value => String(value ?? '').replace(/[&<>"']/g,
                c => ({'&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'}[c]));
            window.t = (_, fallback) => fallback;
            window.AppState = {registerModule(_name, members) {Object.assign(this, members);}};
            window.callbacks = []; window.Notification = {error(message) {throw new Error(message);}};
            window.ModuleLoader = {loadModule(name, callback) {callbacks.push({name, callback});}};
            window.fetch = () => {throw new Error('offline editor must not call any API');};
        });
        for (const file of ['web-src/js/state-ui.js', ...['periph-exec', 'periph-exec-modbus', 'periph-exec-form', 'periph-exec-templates']
            .map(name => 'web-src/modules/runtime/' + name + '.js')])
            await page.addScriptTag({content: fs.readFileSync(file, 'utf8')});
        await page.evaluate(schema => {
            Object.assign(AppState, {_peEditorDataReady: true, guardDeveloperModeAction: () => true});
            AppState._peTypeCatalog = [{value: 37, sdRecorderConfig: schema}];
            AppState._pePeripherals = [{id: 'sd', type: 37, enabled: true}, {id: 'off', type: 37, enabled: false},
                {id: 'room', type: 38, enabled: true}];
            AppState._peSensorSources = [{id: 'room_temperature', periphId: 'room', field: 'temperature', label: 'Room'}];
            window.config = {sourceRef: 'room_temperature'};
            for (const field of schema.numbers) config[field.key] = field.defaultValue;
            window.saved = {actionType: 40, targetPeriphId: 'sd', actionValue: JSON.stringify(config), syncDelayMs: 0};
            AppState._createPeriphExecActionElement(saved, 0);
        }, schema);
        const pending = await page.evaluate(() => {
            const action = AppState._collectPeriphExecActions()[0];
            AppState.savePeriphExecRule();
            const before = {action, error: AppState._peLazyModuleError,
                inlineError: document.getElementById('periph-exec-error').textContent,
                loads: callbacks.map(item => item.name)};
            window.oldBlock = document.querySelector('#periph-exec-actions .periph-exec-config-item');
            AppState._rebuildActionBlocksForTriggerChange(); callbacks[0].callback();
            before.rebuilt = AppState._collectPeriphExecActions()[0];
            before.detachedEmpty = !oldBlock.querySelector('.pe-sd-record-sourceRef');
            before.loadCount = callbacks.length;
            return before;
        });
        assert.equal(pending.action.actionType, 40); assert.equal(pending.action.useReceivedValue, false);
        assert.equal(pending.error, 'periph-exec-sensor'); assert.match(pending.inlineError, /加载/);
        assert.deepEqual(pending.loads, ['periph-exec-sensor']); assert.equal(pending.loadCount, 2);
        assert.equal(pending.rebuilt.actionValue, pending.action.actionValue); assert.equal(pending.detachedEmpty, true);
        const failed = await page.evaluate(() => {
            callbacks[1].callback();
            return {loads: callbacks.length, action: AppState._collectPeriphExecActions()[0],
                message: document.querySelector('.pe-action-lazy-host').textContent};
        });
        assert.equal(failed.loads, 2); assert.equal(failed.action.actionValue, pending.action.actionValue);
        assert.match(failed.message, /加载失败/);
        await page.evaluate(() => {
            AppState._rebuildActionBlocksForTriggerChange();
            const select = document.querySelector('.pe-action-type');
            select.value = '0'; AppState.onPeriphExecActionTypeChangeInBlock('0', 0);
        });
        await page.addScriptTag({content: fs.readFileSync('web-src/modules/runtime/periph-exec-sensor.js', 'utf8')});
        const switched = await page.evaluate(() => {
            callbacks[2].callback();
            const result = {type: document.querySelector('.pe-action-type').value,
                recorder: !!document.querySelector('.pe-sd-record-sourceRef')};
            const select = document.querySelector('.pe-action-type');
            select.value = '40'; AppState.onPeriphExecActionTypeChangeInBlock('40', 0);
            result.restored = AppState._collectPeriphExecActions()[0];
            return result;
        });
        assert.equal(switched.type, '0'); assert.equal(switched.recorder, false);
        assert.deepEqual(JSON.parse(switched.restored.actionValue), JSON.parse(pending.action.actionValue));
        const edited = await page.evaluate(() => {
            document.querySelector('.pe-sd-record-sampleSec').value = '60';
            document.querySelector('.pe-sd-record-fileCount').value = '8';
            const select = document.querySelector('.pe-action-type');
            select.value = '38'; AppState.onPeriphExecActionTypeChangeInBlock('38', 0);
            const snapshot = AppState._collectPeriphExecActions()[0];
            select.value = '40'; AppState.onPeriphExecActionTypeChangeInBlock('40', 0);
            AppState._rebuildActionBlocksForTriggerChange();
            const targets = Array.from(document.querySelector('.pe-target-periph').options).filter(o => o.value).map(o => o.value);
            document.querySelector('.pe-sync-delay').value = '100';
            ['enabled','report','record-history','panel-control'].forEach(id => document.getElementById('periph-exec-' + id).checked = true);
            document.querySelector('.pe-sd-record-setup').click();
            return {action: AppState._collectPeriphExecActions()[0], triggers: AppState._collectPeriphExecTriggers(), snapshot, targets,
                flags: ['enabled','report','record-history','panel-control'].map(id => document.getElementById('periph-exec-' + id).checked)};
        });
        assert.equal(edited.snapshot.actionType, 38); assert.equal(edited.snapshot.actionValue, '');
        assert.equal(edited.snapshot.useReceivedValue, false);
        const config = JSON.parse(edited.action.actionValue);
        assert.equal(config.sampleSec, 60); assert.equal(config.fileCount, 8); assert.equal(config.sourceRef, 'room_temperature');
        assert.deepEqual(edited.targets, ['sd']); assert.deepEqual(edited.flags, [false,false,false,false]);
        assert.equal(edited.action.syncDelayMs, 0); assert.equal(edited.triggers.length, 1);
        assert.equal(edited.triggers[0].intervalSec, 1); assert.equal(edited.triggers[0].timerMode, 0);
        const template = await page.evaluate(() => {
            document.getElementById('periph-exec-original-id').value = '';
            AppState._resetPeriphExecTemplateWizard(true);
            const kind = document.getElementById('pe-template-kind'); kind.value = 'sd-record';
            kind.dispatchEvent(new Event('change', {bubbles: true}));
            const target = document.getElementById('pe-template-target'); target.value = 'sd';
            target.dispatchEvent(new Event('change', {bubbles: true}));
            document.getElementById('pe-template-channel').value = 'room_temperature';
            document.getElementById('pe-template-review').click();
            const text = document.getElementById('pe-template-preview').textContent;
            const confirm = document.getElementById('pe-template-confirm'); confirm.checked = true;
            confirm.dispatchEvent(new Event('change', {bubbles: true}));
            document.getElementById('pe-template-apply').click();
            return {text, action: AppState._collectPeriphExecActions()[0],
                enabled: document.getElementById('periph-exec-enabled').checked,
                report: document.getElementById('periph-exec-report').checked};
        });
        assert.match(template.text, /保存启用后才持续写卡/); assert.equal(template.enabled, false); assert.equal(template.report, false);
        assert.equal(template.action.actionType, 40); assert.equal(template.action.targetPeriphId, 'sd');
        console.log('SD recorder real DOM: lazy pending/failure/late callback, edited draft, status38, target filter, setup and disabled template passed');
    } finally {await browser.close();}
})().catch(error => {console.error(error); process.exitCode = 1;});
