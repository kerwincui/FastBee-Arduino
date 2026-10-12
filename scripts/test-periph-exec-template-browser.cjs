'use strict';
const fs = require('fs'), path = require('path'), crypto = require('crypto'), assert = require('assert/strict');
const root = path.resolve(__dirname, '..');
const {chromium} = require(require.resolve('playwright', {paths:[path.join(root, 'test/browser')]}));
const out = process.argv[2] || path.join(root,'artifacts','periph-exec-templates-'+new Date().toISOString().replace(/[:.]/g,'-')+'-'+process.pid);
if (!out || fs.existsSync(out)) throw new Error('Fresh evidence directory required');
fs.mkdirSync(out, {recursive:true});
const files = ['web-src/pages/modals.html', 'web-src/css/main.css', ...['periph-exec','periph-exec-form',
    'periph-exec-modbus','periph-exec-sensor','periph-exec-templates'].map(n=>'web-src/modules/runtime/'+n+'.js')];
const sources = files.map(file=>({file, text:fs.readFileSync(path.join(root,file),'utf8'),
    sha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(root,file))).digest('hex')}));
const report = {status:'RUNNING',startedAt:new Date().toISOString(),scope:'Offline production modal, CSS and modules; mock configuration, no device/API/save',
    sources:sources.map(({file,sha256})=>({file,sha256})),cases:[]};
const save=()=>fs.writeFileSync(path.join(out,'report.json'),JSON.stringify(report,null,2));
save();
(async()=>{
    let browser;
    try {
        browser=await chromium.launch({headless:true});
        for(const viewport of [{width:1280,height:800},{width:390,height:844}]) {
            const page=await browser.newPage({viewport});
            const errors=[],requests=[];
            page.on('pageerror',e=>errors.push(e.message));
            await page.route('**/*',route=>{requests.push(route.request().url());return route.abort();});
            const markup=sources[0].text.split('<!-- 外设执行模态窗 -->')[1].split('<!-- 规则脚本模态窗 -->')[0];
            assert(markup.includes('pe-template-wizard'));
            await page.setContent(markup);
            await page.addStyleTag({content:sources[1].text});
            await page.evaluate(()=>{
                window.AppState={registerModule(name,api){Object.assign(this,api);}};
                window.escapeHtml=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
                window.templateApiCalls=[];
                window.apiGet=window.apiPost=(...args)=>{templateApiCalls.push(args);throw new Error('API not allowed');};
            });
            for(const s of sources.slice(2)) await page.addScriptTag({content:s.text});
            await page.evaluate(()=>{
                const a=AppState;
                a._hiddenClass=show=>show?'':' is-hidden';
                a._setSectionVisible=(el,show)=>{if(el)el.classList.toggle('is-hidden',!show);};
                a._getPeriphExecBlock=(id,index)=>document.getElementById(id)?.children[index];
                a.guardDeveloperModeAction=()=>true;
                a._pePeripherals=[{id:'rtc',name:'RTC <safe>',type:50,enabled:true},{id:'off',name:'Disabled',type:50,enabled:false},
                    {id:'sensor',name:'Room',type:38,enabled:true,params:{driverName:'SHT31'}},{id:'relay',type:12,enabled:true}];
                a._peTypeCatalog=[{value:50,implemented:true,featureActive:true},{value:38,implemented:true,featureActive:true,driverDescriptors:[
                    {name:'SHT31',compiled:true,featureActive:true,minIntervalMs:10000,channels:[{name:'temperature',unit:'C',mode:0},{name:'humidity',unit:'%',mode:0}]}]}];
                a._peEditorDataReady=true;
                a._masterTasks=[{name:'Meter <readonly>',enabled:true,slaveAddress:1,functionCode:3,startAddress:10,quantity:2,mappings:[{sensorId:'voltage',regOffset:0,unit:'V'}]}];
                a._peTemplateModbusConfig={enabled:true,transferType:0,master:{tasks:a._masterTasks}};
                a._bindPeriphExecModalEvents();
                a._resetPeriphExecTemplateWizard(true);
                document.getElementById('periph-exec-modal').style.display='flex';
                document.getElementById('pe-template-wizard').open=true;
            });
            const checked=name=>{report.cases.push({name,width:viewport.width,status:'PASS'});save();};
            assert.equal(await page.locator('#pe-template-apply').isDisabled(),true);checked('no-implicit-template-or-apply');
            await page.locator('#pe-template-kind').selectOption('rtc-read');
            assert.deepEqual(await page.locator('#pe-template-target option').evaluateAll(os=>os.map(o=>o.value)),['','rtc','off']);
            assert.equal(await page.locator('#pe-template-target').inputValue(),'');checked('typed-targets-no-default');
            await page.locator('#pe-template-target').selectOption('off');
            await page.locator('#pe-template-review').click();
            assert((await page.locator('#pe-template-preview').textContent()).includes('未启用'));checked('disabled-target-rejected');
            await page.locator('#pe-template-target').selectOption('rtc');
            await page.locator('#pe-template-review').click();
            assert.equal(await page.locator('#periph-exec-actions').locator(':scope > *').count(),0);
            assert.equal(await page.locator('#pe-template-apply').isDisabled(),true);checked('preview-does-not-mutate-rule');
            await page.locator('#pe-template-confirm').check();
            await page.locator('#pe-template-apply').click();
            let result=await page.evaluate(()=>({actions:AppState._collectPeriphExecActions(),triggers:AppState._collectPeriphExecTriggers()}));
            assert.equal(result.actions[0].actionType,36);assert.equal(result.actions[0].targetPeriphId,'rtc');
            assert.equal(result.triggers[0].triggerType,1);assert.equal(result.triggers[0].intervalSec,60);
            assert.equal(await page.locator('#periph-exec-enabled').isChecked(),false);checked('rtc-disabled-draft-uses-real-collector');
            await page.locator('#pe-template-kind').selectOption('configured-sensor');
            await page.locator('#pe-template-target').selectOption('sensor');
            assert.equal(await page.locator('#pe-template-channel').inputValue(),'');checked('channel-explicit-selection');
            await page.locator('#pe-template-channel').selectOption('temperature');
            await page.locator('#pe-template-property').fill('roomTemp');
            await page.locator('#pe-template-interval').fill('9');
            await page.locator('#pe-template-review').click();
            assert((await page.locator('#pe-template-preview').textContent()).includes('10～86400'));checked('driver-minimum-interval');
            await page.locator('#pe-template-interval').fill('60');
            await page.locator('#pe-template-review').click();
            await page.locator('#pe-template-confirm').check();
            await page.locator('#pe-template-property').fill('changed');
            assert.equal(await page.locator('#pe-template-apply').isDisabled(),true);checked('preview-invalidated-on-input');
            await page.locator('#pe-template-property').fill('roomTemp');
            await page.locator('#pe-template-review').click();
            await page.locator('#pe-template-confirm').check();
            await page.locator('#pe-template-apply').click();
            result=await page.evaluate(()=>AppState._collectPeriphExecActions());
            assert.equal(result[0].actionType,19);
            const value=JSON.parse(result[0].actionValue);
            assert.equal(value.channelField,'temperature');assert.equal(value.dataField,'roomTemp');
            assert.equal(value.periphId,'sensor');assert.equal(value.unit,'C');checked('sensor-real-form-roundtrip');
            for(const id of ['enabled','record-history','panel-control'])assert.equal(await page.locator('#periph-exec-'+id).isChecked(),false);
            assert.equal(await page.locator('#periph-exec-report').isChecked(),true);checked('safe-rule-flags');
            await page.evaluate(()=>AppState._rebuildActionBlocksForTriggerChange());
            result=await page.evaluate(()=>AppState._collectPeriphExecActions());
            assert.equal(JSON.parse(result[0].actionValue).dataField,'roomTemp');checked('trigger-rebuild-preserves-channel');
            await page.locator('#pe-template-kind').selectOption('configured-modbus');
            assert.equal(await page.locator('#pe-template-target').inputValue(),'');checked('modbus-explicit-task-index');
            await page.locator('#pe-template-target').selectOption('0');
            await page.locator('#pe-template-review').click();
            assert((await page.locator('#pe-template-preview').textContent()).includes('请先核对'));checked('modbus-requires-instrument-review');
            await page.locator('#pe-template-modbus-reviewed').check();
            await page.locator('#pe-template-review').click();
            await page.locator('#pe-template-confirm').check();
            await page.evaluate(()=>{AppState._masterTasks[0].slaveAddress=2;});
            await page.locator('#pe-template-apply').click();
            assert((await page.locator('#pe-template-preview').textContent()).includes('重新预览'));checked('modbus-stale-task-rejected');
            await page.locator('#pe-template-review').click();
            await page.locator('#pe-template-confirm').check();
            await page.locator('#pe-template-apply').click();
            result=await page.evaluate(()=>AppState._collectPeriphExecActions());
            assert.equal(result[0].actionType,18);assert.equal(result[0].targetPeriphId,'');
            assert.deepEqual(JSON.parse(result[0].actionValue),{poll:[0]});
            assert.equal(await page.locator('#periph-exec-enabled').isChecked(),false);checked('modbus-poll-only-real-form-roundtrip');
            await page.evaluate(()=>AppState._rebuildActionBlocksForTriggerChange());
            result=await page.evaluate(()=>AppState._collectPeriphExecActions());
            assert.deepEqual(JSON.parse(result[0].actionValue),{poll:[0]});checked('modbus-rebuild-never-creates-control-command');
            const overflow=await page.evaluate(()=>({page:document.documentElement.scrollWidth>innerWidth+1,
                modal:document.querySelector('.modal-content').scrollWidth>document.querySelector('.modal-content').clientWidth+1}));
            assert.deepEqual(overflow,{page:false,modal:false});checked('responsive-no-horizontal-overflow');
            await page.screenshot({path:path.join(out,'draft-'+viewport.width+'.png'),fullPage:true});
            // Rebuilding a mixed existing draft must retain both read-only task
            // zero and explicit controls. This only serializes a mock form.
            for (const device of [
                {deviceType:'relay',channelCount:4,control:{d:0,c:3,a:'off'}},
                {deviceType:'pwm',channelCount:4,pwmResolution:16,control:{d:0,c:2,a:'pwm',v:32123}},
                {deviceType:'pid',channelCount:2,control:{d:0,c:1,a:'pid',p:'D',v:0}},
                {deviceType:'motor',control:{d:0,a:'stop'}}
            ]) {
                const roundtrip=await page.evaluate(device=>{
                    AppState._modbusDevices=[{...device,name:'Mock control',enabled:true}];
                    document.getElementById('periph-exec-actions').replaceChildren();
                    const drafts=[
                        {actionType:36,targetPeriphId:'rtc',actionValue:''},
                        {actionType:18,targetPeriphId:'',actionValue:JSON.stringify({poll:[0]})},
                        {actionType:18,targetPeriphId:'modbus:0',actionValue:JSON.stringify({ctrl:[device.control]}),execMode:1,syncDelayMs:125}
                    ];
                    drafts.forEach((draft,index)=>AppState._createPeriphExecActionElement(draft,index));
                    const before=AppState._collectPeriphExecActions();
                    AppState._rebuildActionBlocksForTriggerChange();
                    return {before,after:AppState._collectPeriphExecActions()};
                },device);
                assert.deepEqual(roundtrip.after,roundtrip.before);
                assert.deepEqual(JSON.parse(roundtrip.after[1].actionValue),{poll:[0]});
                assert.deepEqual(JSON.parse(roundtrip.after[2].actionValue),{ctrl:[device.control]});
                assert.equal(roundtrip.after[2].execMode,1);assert.equal(roundtrip.after[2].syncDelayMs,125);
                checked('mixed-action-rebuild-preserves-'+device.deviceType);
            }
            await page.evaluate(()=>{AppState._resetPeriphExecTemplateWizard(false);});
            assert.equal(await page.locator('#pe-template-wizard').isVisible(),false);checked('hidden-when-editing-saved-rule');
            assert.deepEqual(errors,[]);assert.deepEqual(requests,[]);
            assert.equal(await page.evaluate(()=>templateApiCalls.length),0);checked('no-api-or-browser-errors');
            await page.close();
        }
        const drift=sources.filter(s=>crypto.createHash('sha256').update(fs.readFileSync(path.join(root,s.file))).digest('hex')!==s.sha256);
        report.inputsStable=drift.length===0;assert.equal(drift.length,0);report.status='PASS';
    } catch(error){report.status='FAIL';report.error=error.stack;process.exitCode=1;}
    finally {if(browser)await browser.close();report.finishedAt=new Date().toISOString();save();console.log(JSON.stringify(report));}
})();
