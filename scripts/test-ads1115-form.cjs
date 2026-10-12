'use strict';
const fs = require('fs'), path = require('path'), crypto = require('crypto'), assert = require('assert/strict');
const {spawnSync} = require('child_process');
const os = require('os');
const root = path.resolve(__dirname, '..');
const dependencies = process.env.FASTBEE_DEPENDENCY_ROOT || root;
const {chromium} = require(require.resolve('playwright', {paths:[path.join(dependencies, 'test/browser')]}));
const out = process.argv[2] || path.join(root, 'artifacts', 'ads1115-form-' + new Date().toISOString().replace(/[:.]/g, '-') + '-' + process.pid);
if (fs.existsSync(out)) throw new Error('Fresh evidence directory required');
fs.mkdirSync(out, {recursive:true});
const files = ['web-src/modules/runtime/peripherals.js', 'web-src/modules/runtime/peripheral-save.js',
    'web-src/css/main.css', 'include/core/SensorDriverMetadata.h', 'test/unit/cpp/ads1115_metadata_harness.cpp',
    'web-src/modules/runtime/peripheral-io.js', 'web-src/modules/runtime/periph-exec-sensor.js', 'web-src/modules/runtime/periph-exec-templates.js'];
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(path.join(root, file))).digest('hex');
const inputs = Object.fromEntries(files.map(file => [file, digest(file)]));
const report = {status:'RUNNING', scope:'Offline ADS parameter component and actual save validation; production C++ metadata; no device, publication or network', inputs, cases:[]};
const save = () => fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(report, null, 2));
save();
(async () => {
    let browser, compilerDirectory;
    try {
        compilerDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-ads1115-metadata-'));
        const exe = path.join(compilerDirectory, process.platform === 'win32' ? 'metadata.exe' : 'metadata');
        report.compilerArtifacts = {directory:compilerDirectory, executable:exe};
        const compiled = spawnSync(process.env.CXX || 'g++', ['-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + path.join(root, 'include'), path.join(root, files[4]), '-o', exe], {encoding:'utf8', timeout:60000, env:{...process.env, TEMP:compilerDirectory, TMP:compilerDirectory}});
        assert.equal(compiled.status, 0, compiled.stderr || String(compiled.error));
        const metadata = spawnSync(exe, [], {encoding:'utf8', timeout:10000});
        assert.equal(metadata.status, 0, metadata.stderr);
        const meta = JSON.parse(metadata.stdout);
        fs.writeFileSync(path.join(out, 'metadata.json'), JSON.stringify(meta, null, 2));
        browser = await chromium.launch({headless:true});
        for (const viewport of [{width:1280,height:800}, {width:390,height:844}]) {
            const page = await browser.newPage({viewport});
            const errors = [], requests = [];
            page.on('pageerror', error => errors.push(error.message));
            await page.route('**/*', route => { requests.push(route.request().url()); return route.abort(); });
            await page.setContent('<main style="max-width:760px;margin:12px"><h2>ADS1115 参数</h2><select id="generic-sensor-driver"><option>ADS1115</option></select><input id="generic-sensor-address" value="0x48"><input id="generic-sensor-interval" value="1000"><select id="generic-sensor-channel"><option>voltage</option><option>rawCode</option></select><select id="peripheral-i2c-channel"></select><div id="generic-sensor-driver-params" class="fb-grid-2"></div></main>');
            await page.addStyleTag({content:fs.readFileSync(path.join(root, files[2]), 'utf8')});
            await page.evaluate(() => {
                window.AppState = {registerModule(name, api) { Object.assign(this, api); }};
                window.apiGet = window.apiPost = () => { throw new Error('Device/API access forbidden'); };
            });
            for (const file of files.slice(0,2).concat(files.slice(5))) await page.addScriptTag({content:fs.readFileSync(path.join(root,file),'utf8')});
            await page.evaluate(meta => {
                AppState._getPeripheralDriverMeta = () => meta;
                AppState._renderDriverParams(meta, {}, true);
                window.adsMeta = meta;
                window.collectAds = () => { const data = {pins:'21,22'}; const error = AppState._collectI2cDriverConfig(data, 38); return {data,error}; };
            }, meta);
            const checked = name => { report.cases.push({viewport:viewport.width,name,status:'PASS'}); save(); };
            assert.equal(await page.locator('[data-driver-param]').count(), 6); checked('production-parameter-count');
            assert.equal(await page.locator('#generic-driver-mux').inputValue(), '4'); checked('default-single-ended-input');
            assert.equal(await page.locator('#generic-driver-mux option').count(), 8); checked('eight-input-combinations');
            assert.equal(await page.locator('#generic-driver-mux option[value="7"]').textContent(), 'AIN3 - GND'); checked('human-readable-mux-labels');
            assert((await page.locator('#generic-sensor-driver-params').textContent()).includes('不代表允许输入电压')); checked('electrical-boundary-visible');
            let result = await page.evaluate(() => collectAds());
            assert.equal(result.error, ''); assert.equal(result.data.fullScaleMv, 4096); checked('default-save-payload');
            await page.locator('#generic-driver-mux').selectOption('7');
            await page.locator('#generic-driver-dataRate').selectOption('8');
            await page.locator('#generic-driver-calibrationGain').fill('2');
            await page.locator('#generic-driver-calibrationOffset').fill('-1');
            result = await page.evaluate(() => collectAds());
            assert.equal(result.error, ''); assert.equal(result.data.mux, 7); assert.equal(result.data.calibrationOffset, -1); checked('configured-save-payload');
            await page.locator('#generic-sensor-interval').fill('100');
            assert((await page.evaluate(() => collectAds())).error.includes('300')); checked('slow-rate-budget-enforced');
            await page.locator('#generic-sensor-interval').fill('1000');
            await page.evaluate(() => AppState._renderDriverParams(adsMeta, {}, false));
            assert.equal(await page.locator('#generic-driver-mux').inputValue(), '7'); checked('rerender-keeps-unsaved-input');
            await page.locator('#generic-driver-calibrationGain').fill('0');
            assert((await page.evaluate(() => collectAds())).error); checked('zero-calibration-rejected');
            await page.locator('#generic-driver-calibrationGain').fill('2');
            await page.locator('#generic-driver-calibrationOffset').fill('');
            assert((await page.evaluate(() => collectAds())).error); checked('empty-offset-rejected');
            await page.locator('#generic-driver-calibrationOffset').fill('-1');
            await page.locator('#generic-sensor-address').fill('0x47');
            assert((await page.evaluate(() => collectAds())).error); checked('unsupported-address-rejected');
            await page.locator('#generic-sensor-address').fill('0x48');
            await page.evaluate(() => AppState._renderDriverParams(adsMeta, {}, true));
            assert.equal(await page.locator('#generic-driver-calibrationGain').inputValue(), '1'); checked('reset-restores-default');
            await page.evaluate(() => AppState._renderDriverParams(adsMeta, {mux:2,fullScaleMv:512,dataRate:64,calibrationGain:1.25,calibrationOffset:0.2}, true));
            result = await page.evaluate(() => collectAds());
            assert.equal(result.error, ''); assert.equal(result.data.mux, 2); assert.equal(result.data.fullScaleMv, 512); checked('persisted-edit-roundtrip');
            await page.evaluate(() => AppState._renderDriverParams(adsMeta, {dataRate:8}, true));
            for (const mux of [4,5,6,7]) await page.locator('[data-ads-mux="' + mux + '"]').check();
            assert.equal(await page.locator('#generic-driver-scanMask').inputValue(), '240'); checked('four-mux-mask-from-checkboxes');
            assert.equal(await page.locator('#generic-sensor-interval').getAttribute('min'), '1200');
            assert((await page.evaluate(() => collectAds())).error.includes('1200')); checked('whole-scan-budget-enforced');
            assert.equal(await page.locator('[data-ads-mux="0"]').isDisabled(), true); checked('fifth-choice-disabled');
            await page.locator('#generic-sensor-interval').fill('1200');
            result = await page.evaluate(() => collectAds());
            assert.equal(result.error, ''); assert.equal(result.data.scanMask, 240); checked('scan-save-payload');
            const expected = ['voltage_mux4','voltage_mux5','voltage_mux6','voltage_mux7'];
            assert.deepEqual(await page.locator('#generic-sensor-channel option').evaluateAll(nodes => nodes.map(n => n.value)), expected); checked('active-scan-channels');
            const consumers = await page.evaluate(() => {
                const config = {id:'adc',type:38,enabled:true,params:{driver:'ADS1115',scanMask:240,mode:0}};
                AppState._pePeripherals = [config]; AppState._peTypeCatalog = [{value:38,driverDescriptors:[adsMeta]}];
                AppState._populatePeripheralDiagnosticChannels(config, 'ADS1115');
                return {
                    diagnostic:Array.from(document.querySelectorAll('#peripheral-i2c-channel option'), n=>n.value),
                    action:AppState._getConfiguredSensorDetails('adc').channels.map(c=>c.name),
                    template:AppState._getPeriphExecTemplateTarget('configured-sensor','adc').channels.map(c=>c.name)
                };
            });
            for (const channels of Object.values(consumers)) assert.deepEqual(channels, expected); checked('diagnostic-action-template-consumers');
            await page.evaluate(() => {
                const field = document.getElementById('generic-driver-scanMask');
                field.value = '31'; field.dispatchEvent(new Event('change'));
            });
            assert((await page.evaluate(() => collectAds())).error.includes('最多')); checked('imported-five-channel-mask-rejected');
            await page.evaluate(() => AppState._renderDriverParams(adsMeta, {scanMask:144,dataRate:128}, true));
            assert.deepEqual(await page.locator('#generic-sensor-channel option').evaluateAll(nodes => nodes.map(n => n.value)), ['voltage_mux4','voltage_mux7']); checked('sparse-mask-reload-preserves-stable-names');
            await page.locator('[data-ads-mux="4"]').uncheck(); await page.locator('[data-ads-mux="7"]').uncheck();
            assert.deepEqual(await page.locator('#generic-sensor-channel option').evaluateAll(nodes => nodes.map(n=>n.value)), ['voltage','rawCode']);
            assert.equal(await page.locator('#generic-driver-mux').isDisabled(), false); checked('zero-mask-restores-legacy');
            await page.screenshot({path:path.join(out, 'form-' + viewport.width + '.png'),fullPage:true});
            const layout = await page.evaluate(() => ({width:document.documentElement.clientWidth,
                scroll:document.documentElement.scrollWidth, note:document.querySelector('#generic-sensor-driver-params p').getBoundingClientRect().width}));
            assert(layout.scroll <= layout.width + 1 && layout.note > Math.min(viewport.width - 30, 700)); checked('warning-full-width-without-overflow');
            assert.deepEqual(errors, []); assert.deepEqual(requests, []); checked('no-script-error-or-network');
            await page.close();
        }
        report.inputsStable = files.every(file => digest(file) === inputs[file]);
        assert(report.inputsStable); report.status = 'PASS';
    } catch(error) { report.status = 'FAIL'; report.error = error.stack; process.exitCode = 1; }
    finally {
        try { if (browser) await browser.close(); }
        catch(error) { report.status = 'FAIL'; report.browserCleanupError = error.stack; process.exitCode = 1; }
        try {
            if (compilerDirectory) {
                const preserved = path.join(out, 'metadata-compiler-artifacts');
                const inventory = directory => {
                    const entries = {};
                    const walk = (current, prefix = '') => {
                        for (const entry of fs.readdirSync(current, {withFileTypes:true})) {
                            const relative = prefix + entry.name, file = path.join(current, entry.name);
                            assert(!entry.isSymbolicLink(), 'Compiler artifacts must be ordinary files');
                            if (entry.isDirectory()) walk(file, relative + '/');
                            else { assert(entry.isFile()); entries[relative] = crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'); }
                        }
                    };
                    walk(directory); return entries;
                };
                const inputs = inventory(compilerDirectory);
                fs.cpSync(compilerDirectory, preserved, {recursive:true, errorOnExist:true, force:false});
                assert.deepEqual(inventory(preserved), inputs);
                assert.deepEqual(inventory(compilerDirectory), inputs);
                Object.assign(report.compilerArtifacts, {preservedDirectory:preserved, filesSha256:inputs, preservationVerified:true});
            }
        } catch(error) { report.status = 'FAIL'; report.compilerPreservationError = error.stack; process.exitCode = 1; }
        report.finishedAt = new Date().toISOString(); save();
    }
    console.log(JSON.stringify({status:report.status,cases:report.cases.length,report:path.join(out,'report.json'),error:report.error}));
})();
