'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');
const {parse} = require('acorn');
const {minify_sync} = require('terser');
const {createRequire} = require('node:module');
const {chromium} = require('../browser/node_modules/playwright');
const root = path.resolve(__dirname, '../..');
const helper = require('../../scripts/web-private-properties');
const m = helper.MAPPING;
const records = helper.readAuditSources(root);

function checkAuditor() {
    // The original 289 mappings are an append-only published contract. A future
    // extension cannot rename already reviewed members by inserting/reordering.
    const first289 = JSON.stringify(Object.entries(m).slice(0, 289));
    assert.equal(crypto.createHash('sha256').update(first289).digest('hex'),
        'b60edad74f8c8e208c775af334eada0eecb55d4a8d25a2664048f482f08874f8');
    assert.equal(m._restoreRtcBootSeed, '$p38');
    assert.equal(helper.auditPrivateRuntimeSources(records, root).members, Object.keys(m).length);
    assert.equal(new Set(Object.values(m)).size, Object.keys(m).length);
    for (const key of ['_peModalBound', '_peEventsBound', '_peEventCategories', '_peEventCategoriesFetchedAt',
        '_peEventCategoriesPromise', '_renderSdRecorderPanel', '_renderThermostatPanel', '_initIrManagePanel',
        '_initRfManagePanel', '_peLazyLoadToken', '_h1ConfigSession', '_category']) assert.equal(m[key], undefined, key);
    const external = path.join(root, 'web-src/js/new-consumer.js');
    assert.throws(() => helper.auditPrivateRuntimeSources([...records, {file: external,
        text: 'AppState._restoreRtcBootSeed({});'}], root), /External private member consumer/);
    assert.throws(() => helper.auditPrivateRuntimeSources([...records, {file: external,
        text: 'AppState._pePeripherals = [];'}], root), /External private member consumer/);
    assert.throws(() => helper.auditPrivateRuntimeSources([...records, {file: external,
        text: 'AppState._pePer\\u0069pherals = [];'}], root), /External private member consumer/);
    assert.throws(() => helper.auditPrivateRuntimeSources([...records, {file: external,
        text: 'const prefix="_pe"; AppState[prefix+"Peripherals"]=[];'}], root), /External reflected private prefix/);
    for (const text of ['<script>AppState["_pe"+suffix]</script>',
        '<button onclick="AppState[&#95;pe+suffix]()">', '<script>AppState["\\x5fpe"+suffix]</script>'])
        assert.throws(() => helper.auditPrivateRuntimeSources([...records, {
            file: path.join(root, 'web-src/pages/new.html'), text}], root), /External reflected private prefix/);
    assert.throws(() => helper.auditPrivateRuntimeSources([...records, {file: path.join(root, 'include/new-wire.h'),
        text: 'const char* wire="' + m._pePeripherals + '";'}], root), /Generated private name already exists/);
    assert.throws(() => helper.auditPrivateRuntimeSources([...records, {file: path.join(root, 'data/config/new-wire.json'),
        text: '{"\\u0024' + m._pePeripherals.slice(1) + '":0}'}], root), /Generated private name already exists/);
    const altered = records.map(record => record.file.endsWith(path.join('runtime', 'periph-exec.js'))
        ? {...record, text: record.text + '\nAppState[window.unknownKey]();'} : record);
    assert.throws(() => helper.auditPrivateRuntimeSources(altered, root), /Unreviewed computed AppState access/);
    const conditional = records.map(record => record.file.endsWith(path.join('runtime', 'periph-exec-templates.js'))
        ? {...record, text: record.text.replace("actionType === 40 ? '_renderSdRecorderPanel'", 'actionType === 40 ? arbitraryKey')}
        : record);
    assert.throws(() => helper.auditPrivateRuntimeSources(conditional, root), /Unreviewed computed AppState access/);
    const first = helper.createPrivateRuntimeMinifier(root, records), second = helper.createPrivateRuntimeMinifier(root, records);
    const names = Object.values(helper.FAMILY_FILES).flat();
    const published = new Map();
    for (const name of names) {
        const source = fs.readFileSync(path.join(root, 'web-src/modules/runtime', name), 'utf8');
        published.set(name, first.minify(name, source));
    }
    for (const name of names.slice().reverse()) {
        const source = fs.readFileSync(path.join(root, 'web-src/modules/runtime', name), 'utf8');
        assert.equal(second.minify(name, source), published.get(name), 'fixed map must be order-independent: ' + name);
    }
}

function captureProductionWriters() {
    // Printer options must preserve the exact parsed program after private
    // mapping. Strip source locations and quote spelling only, not AST values.
    const ast = code => JSON.parse(JSON.stringify(parse(code, {ecmaVersion: 'latest'}),
        (key, value) => ['start','end','raw'].includes(key) ? undefined : value));
    const printer = helper.createPrivateRuntimeMinifier(root, records);
    for (const name of ['periph-exec.js','periph-exec-form.js','periph-exec-modbus.js','peripherals.js','peripheral-io.js']) {
        const family = name.startsWith('periph-exec') ? 'execution' : 'peripherals';
        const source = fs.readFileSync(path.join(root, 'web-src/modules/runtime', name), 'utf8');
        const baseline = minify_sync(source, {compress: {passes: 2},
            mangle: {properties: {regex: new RegExp('^(?:' + helper.PRIVATE_NAMES[family].join('|') + ')$'), keep_quoted: true}},
            ...(name === 'periph-exec-modbus.js' ? {keep_fnames: true} : {}),
            nameCache: {props: {props: Object.fromEntries(Object.entries(m).map(([key,value]) => ['$' + key,value]))}}}).code;
        assert.deepEqual(ast(printer.minify(name, source)), ast(baseline), 'printer changed parsed behavior: ' + name);
    }
    const script = path.join(root, 'scripts/build-web-modules.js'), requireFromBuild = createRequire(script);
    const output = new Map(), publishRoot = path.join(root, 'data/www');
    const checkTarget = target => {
        const relative = path.relative(publishRoot, path.resolve(target));
        assert.ok(!relative.startsWith('..') && !path.isAbsolute(relative), 'unexpected writer target');
        return relative.split(path.sep).join('/');
    };
    // Run the real direct-module writer and real bundled-module writer. Only
    // their file sink is intercepted; all source reads, capability resolution,
    // module selection, compiler options and exact output bytes are production.
    // Calling cleanup/other mutations fails instead of touching shared data.
    const captureFs = new Proxy(fs, {get(target, key) {
        if (key === 'writeFileSync') return (file, content) => output.set(checkTarget(file), String(content));
        if (key === 'mkdirSync') return file => {checkTarget(file);};
        if (/^(?:write|append|unlink|rm|rmdir|rename|chmod|chown|truncate|copy|cp|symlink|link|createWriteStream)/.test(String(key)))
            return () => {throw new Error('Unexpected shared mutation: ' + String(key));};
        return target[key];
    }});
    const mod = {exports: {}};
    const wrapped = vm.runInThisContext('(function(require,module,exports,__filename,__dirname){\n' + fs.readFileSync(script, 'utf8') + '\n})', {filename: script});
    const customRequire = name => name === 'fs' ? captureFs : requireFromBuild(name);
    wrapped(customRequire, mod, mod.exports, script, path.dirname(script));
    mod.exports.syncDirectRuntimeModules();
    mod.exports.buildSubdirBundles();
    for (const name of Object.values(helper.FAMILY_FILES).flat()) {
        const code = output.get('js/modules/' + name);
        assert.ok(code?.length > 0, 'production writer omitted ' + name);
    }
    assert.ok(output.get('js/modules/periph-exec-form.js').includes(m._collectPeriphExecActions));
    assert.ok(output.get('js/modules/periph-exec.js').includes(m._pePeripherals));
    assert.ok(!output.get('js/modules/periph-exec.js').includes('_pePeripherals'));
    return output;
}

async function browserCheck(output) {
    const browser = await chromium.launch({headless: true});
    try {
        const page = await browser.newPage();
        await page.setContent('<div id="periph-exec-actions"></div><div id="periph-exec-triggers"></div>' +
            '<input id="periph-exec-original-id" value="record"><input id="periph-exec-name" value="SD">' +
            ['enabled','report','record-history','panel-control'].map(id => '<input type="checkbox" id="periph-exec-' + id + '">').join('') +
            '<div id="periph-exec-error"></div><details id="pe-template-wizard"></details>');
        await page.locator('body').evaluate(body => body.insertAdjacentHTML('beforeend',
            '<input type="checkbox" id="ds1302-boot-seed"><input id="ds1302-utc-offset">'));
        await page.evaluate(mapping => {
            window.mapping = mapping;
            window.escapeHtml = value => String(value ?? '').replace(/[&<>"']/g,
                c => ({'&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'}[c]));
            window.t = (_, fallback) => fallback;
            window.AppState = {registerModule(name, members) {ModuleLoader.registerModule(name, members, this);}};
            window.Notification = {error(message) {throw new Error(message);}};
            window.fetch = () => {throw new Error('No network/device operation allowed');};
        }, m);
        await page.addScriptTag({content: fs.readFileSync(path.join(root, 'web-src/js/module-loader.js'), 'utf8')});
        await page.addScriptTag({content: fs.readFileSync(path.join(root, 'web-src/js/state-ui.js'), 'utf8')});
        await page.evaluate(() => {
            window.callbacks = [];
            ModuleLoader.loadModule = (name, callback) => callbacks.push({name, callback});
            AppState.guardDeveloperModeAction = () => true;
        });
        for (const name of ['peripherals.js','peripheral-io.js','peripheral-save.js','periph-exec.js','periph-exec-modbus.js','periph-exec-form.js','periph-exec-templates.js'])
            await page.addScriptTag({content: output.get('js/modules/' + name)});
        const restoredRtc = await page.evaluate(() => {
            const results = [];
            for (const params of [{bootSeed:true,utcOffsetMinutes:480}, {},
                {bootSeed:'true',utcOffsetMinutes:'480'}, {bootSeed:1,utcOffsetMinutes:841},
                {bootSeed:true,utcOffsetMinutes:-840}, {bootSeed:false,utcOffsetMinutes:0}]) {
                AppState[mapping._restoreRtcBootSeed](params);
                results.push({seed:document.getElementById('ds1302-boot-seed').checked,
                    offset:document.getElementById('ds1302-utc-offset').value});
            }
            return {results, originalAbsent:typeof AppState._restoreRtcBootSeed === 'undefined'};
        });
        assert.deepEqual(restoredRtc.results, [{seed:true,offset:'480'}, {seed:false,offset:''},
            {seed:false,offset:''}, {seed:false,offset:''}, {seed:true,offset:'-840'}, {seed:false,offset:'0'}]);
        assert.equal(restoredRtc.originalAbsent, true);
        await page.evaluate(schema => {
            const a = AppState, m = mapping;
            a[m._peEditorDataReady] = true;
            a[m._peTypeCatalog] = [{value: 37, sdRecorderConfig: schema}];
            a[m._pePeripherals] = [{id: 'sd', type: 37, enabled: true}, {id: 'room', type: 38, enabled: true}];
            a[m._peSensorSources] = [{id: 'room_temperature', periphId: 'room', field: 'temperature', label: 'Room'}];
            a[m._periphIoId] = 'independent-peripheral-session';
            window.savedConfig = {sourceRef: 'room_temperature'};
            for (const field of schema.numbers) savedConfig[field.key] = field.defaultValue;
            a[m._createPeriphExecActionElement]({actionType: 40, targetPeriphId: 'sd', actionValue: JSON.stringify(savedConfig)}, 0);
        }, JSON.parse(process.env.FASTBEE_TEST_SD_RECORDER_SCHEMA));
        const pending = await page.evaluate(() => {
            const a = AppState, m = mapping, action = a[m._collectPeriphExecActions]()[0];
            a.savePeriphExecRule();
            const result = {action, error: document.getElementById('periph-exec-error').textContent,
                other: a[m._periphIoId], callbackNames: callbacks.map(item => item.name)};
            window.detached = document.querySelector('#periph-exec-actions .periph-exec-config-item');
            a[m._rebuildActionBlocksForTriggerChange](); callbacks[0].callback();
            result.rebuilt = a[m._collectPeriphExecActions]()[0];
            result.detachedEmpty = !detached.querySelector('.pe-sd-record-sourceRef');
            return result;
        });
        assert.equal(pending.action.actionType, 40); assert.match(pending.error, /加载/);
        assert.deepEqual(pending.callbackNames, ['periph-exec-sensor']);
        assert.equal(pending.other, 'independent-peripheral-session'); assert.equal(pending.detachedEmpty, true);
        assert.equal(pending.rebuilt.actionValue, pending.action.actionValue);
        await page.evaluate(() => callbacks[1].callback());
        assert.match(await page.locator('.pe-action-lazy-host').textContent(), /加载失败/);
        await page.evaluate(() => AppState[mapping._rebuildActionBlocksForTriggerChange]());
        await page.addScriptTag({content: output.get('js/modules/periph-exec-sensor.js')});
        const ready = await page.evaluate(() => {
            callbacks[2].callback();
            document.querySelector('.pe-sd-record-sampleSec').value = '90';
            const select = document.querySelector('.pe-action-type');
            select.value = '38'; AppState.onPeriphExecActionTypeChangeInBlock('38', 0);
            const status = AppState[mapping._collectPeriphExecActions]()[0];
            select.value = '40'; AppState.onPeriphExecActionTypeChangeInBlock('40', 0);
            document.querySelector('.pe-sd-record-setup').click();
            return {action: AppState[mapping._collectPeriphExecActions]()[0], status,
                triggers: AppState[mapping._collectPeriphExecTriggers](),
                rendererPublic: typeof AppState._renderSdRecorderPanel};
        });
        assert.equal(ready.rendererPublic, 'function'); assert.equal(ready.status.actionType, 38); assert.equal(ready.status.actionValue, '');
        assert.equal(JSON.parse(ready.action.actionValue).sampleSec, 90); assert.equal(ready.action.targetPeriphId, 'sd');
        assert.equal(ready.triggers[0].intervalSec, 1); assert.equal(ready.action.useReceivedValue, false);
        const cache = await page.evaluate(async () => {
            let reads = 0;
            window.apiGet = async () => {reads++; return {success: true, data: ['category-a']};};
            const a = AppState, m = mapping;
            const first = await a[m._getPeriphExecEventCategories](false);
            const second = await a[m._getPeriphExecEventCategories](false);
            const before = {first, second, reads, catalog: a._peEventCategories,
                timestamp: a._peEventCategoriesFetchedAt, pending: a._peEventCategoriesPromise};
            a[m._clearPeriphExecEventCatalogCache]();
            before.cleared = a._peEventCategories === null && a._peEventCategoriesFetchedAt === 0 && a._peEventCategoriesPromise === null;
            return before;
        });
        assert.deepEqual(cache.first, ['category-a']); assert.deepEqual(cache.second, cache.first);
        assert.equal(cache.reads, 1); assert.ok(cache.timestamp > 0); assert.equal(cache.pending, null); assert.equal(cache.cleared, true);
    } finally {await browser.close();}
}

(async () => {
    checkAuditor();
    const output = captureProductionWriters();
    await browserCheck(output);
    console.log('Actual direct/bundled writer bytecode: fixed map, collision/reflection guards, order independence, module registration, SD lazy/editor preservation, action38, event-cache strings and cross-family state passed');
})().catch(error => {console.error(error); process.exitCode = 1;});
