'use strict';
const fs=require('fs'),path=require('path'),crypto=require('crypto'),assert=require('assert/strict');
const root=path.resolve(__dirname,'..');
const {chromium}=require(require.resolve('playwright',{paths:[path.join(root,'test/browser')]}));
const out=process.argv[2] || path.join(root,'artifacts','config-migration-preview-'+new Date().toISOString().replace(/[:.]/g,'-')+'-'+process.pid);
if(!out||fs.existsSync(out))throw new Error('Fresh evidence directory required');
fs.mkdirSync(out,{recursive:true});
const files=['web-src/modules/runtime/device-config-transfer.js','web-src/css/main.css','web-src/js/page-loader.js','web-src/modules/runtime/device-config.js'];
const sources=files.map(file=>({file,text:fs.readFileSync(path.join(root,file),'utf8'),sha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(root,file))).digest('hex')}));
const report={status:'RUNNING',startedAt:new Date().toISOString(),scope:'Offline production migration and board forms, mocked HTTP boundaries; no network or device',sources:sources.map(({file,sha256})=>({file,sha256})),cases:[]};
const save=()=>fs.writeFileSync(path.join(out,'report.json'),JSON.stringify(report,null,2));save();
(async()=>{let browser;try{
    browser=await chromium.launch({headless:true});
    for(const viewport of [{width:1280,height:800},{width:390,height:844}]){
        const page=await browser.newPage({viewport}),errors=[],requests=[];
        page.on('pageerror',error=>errors.push(error.message));
        page.on('dialog',dialog=>dialog.accept());
        await page.route('**/*',route=>{requests.push(route.request().url());return route.abort();});
        await page.setContent('<!doctype html><html><body></body></html>');
        await page.addStyleTag({content:sources[1].text});
        await page.addScriptTag({content:sources[2].text});
        await page.evaluate(()=>{
            window.AppState={registerModule(name,methods){Object.assign(this,methods);this.setupDeviceConfigEvents=()=>{};},
                _ensureModalForAction:async id=>PageLoader.mountModal(id),showModal:id=>document.getElementById(id).style.display='flex',hideModal:id=>document.getElementById(id).style.display='none'};
            window.migrationApiCalls=[];window.apiGet=window.apiPost=(...args)=>{migrationApiCalls.push(args);throw new Error('API not allowed');};
        });
        // Load the real parent first: the lazy transfer entrypoint uses its
        // status helper. Keep both source hashes in the evidence report.
        await page.addScriptTag({content:sources[3].text});
        await page.addScriptTag({content:sources[0].text});
        assert.equal(await page.locator('.modal').count(),0,'loading the transfer module must cache without mounting dialogs');
        await page.evaluate(()=>{
            window.migrationFiles=[{name:'network.json',content:JSON.stringify({password:'TOP_SECRET_DO_NOT_RENDER'})},
                {name:'periph_exec.json',content:JSON.stringify({rules:[{enabled:true,actions:[{actionType:37,targetPeriphId:'secret_internal_id'}]}]})},
                {name:'peripherals.json',content:JSON.stringify({peripherals:[{id:'duplicate',enabled:true},{id:'duplicate',enabled:false}]})}];
            window.openMigration=mode=>{window.migrationResult=null;const items=migrationFiles.map(f=>({name:f.name,size:f.content.length,preview:AppState._buildConfigTransferPreview(f)}));
                AppState._showConfigTransferModal(items,mode).then(result=>window.migrationResult={names:result.map(x=>x.name)},error=>window.migrationResult={error:error.message});};
            openMigration('import');
        });
        const checked=name=>{report.cases.push({name,width:viewport.width,status:'PASS'});save();};
        await page.locator('#config-transfer-preview').waitFor({state:'visible'});
        assert.equal(await page.locator('#config-transfer-confirm-btn').isDisabled(),true);checked('import-requires-explicit-review');
        const text=await page.locator('#config-transfer-preview').textContent();
        for(const expected of ['先暂存','下次重启整组应用','目标身份、网络与凭据保持不变','旧固件','重复','自动执行'])assert(text.includes(expected));
        for(const secret of ['TOP_SECRET_DO_NOT_RENDER','secret_internal_id','duplicate'])assert(!text.includes(secret));checked('risk-summary-without-secrets');
        await page.locator('#config-transfer-confirm-btn').evaluate(el=>el.click());
        assert.equal(await page.evaluate(()=>migrationResult),null);checked('no-confirm-before-acknowledgement');
        await page.locator('#config-transfer-ack').check();
        assert.equal(await page.locator('#config-transfer-confirm-btn').isDisabled(),false);
        await page.locator('#config-transfer-list input').first().uncheck();
        assert.equal(await page.locator('#config-transfer-ack').isChecked(),false);
        assert.equal(await page.locator('#config-transfer-confirm-btn').isDisabled(),true);checked('selection-invalidates-review');
        await page.locator('#config-transfer-select-all').check();
        await page.locator('#config-transfer-ack').check();
        const overflow=await page.evaluate(()=>({page:document.documentElement.scrollWidth>innerWidth+1,modal:document.querySelector('.modal-content').scrollWidth>document.querySelector('.modal-content').clientWidth+1}));
        assert.deepEqual(overflow,{page:false,modal:false});checked('mobile-and-desktop-no-overflow');
        await page.screenshot({path:path.join(out,'preview-'+viewport.width+'.png'),fullPage:true});
        await page.locator('#config-transfer-confirm-btn').click();
        assert.deepEqual((await page.evaluate(()=>migrationResult)).names,['network.json','periph_exec.json','peripherals.json']);checked('returns-only-confirmed-selection');
        await page.evaluate(()=>openMigration('export'));
        assert.equal(await page.locator('#config-transfer-preview').isVisible(),false);
        assert.equal(await page.locator('#config-transfer-confirm-btn').isDisabled(),false);
        await page.locator('#config-transfer-confirm-btn').click();checked('export-unchanged-no-ack-required');
        await page.evaluate(()=>openMigration('import'));
        assert.equal(await page.locator('#config-transfer-ack').isChecked(),false);
        await page.locator('#config-transfer-cancel-btn').click();
        assert((await page.evaluate(()=>migrationResult)).error.includes('取消'));checked('reopen-clears-consent-and-cancel-settles');
        await page.evaluate(()=>{migrationResult=null;AppState._showConfigTransferModal([{name:'network.json'}],'import').then(result=>migrationResult=result,error=>migrationResult={error:error.message});});
        assert.equal(await page.locator('#config-transfer-confirm-btn').isDisabled(),true);
        await page.locator('#config-transfer-ack').check();
        assert.equal(await page.locator('#config-transfer-confirm-btn').isDisabled(),true);checked('missing-preview-fails-closed');
        await page.locator('#config-transfer-cancel-btn').click();
        await page.evaluate(()=>{
            const files=[{name:'periph_exec.json',content:JSON.stringify({rules:[{enabled:false,actions:[{actionType:38,targetPeriphId:'PRIVATE_MISSING_SD'}]}]})},
                {name:'peripherals.json',content:JSON.stringify({peripherals:[]})}];
            const items=files.map(f=>({name:f.name,preview:AppState._buildConfigTransferPreview(f)}));
            migrationResult=null;
            AppState._showConfigTransferModal(items,'import',selected=>AppState._buildConfigTransferDependencyPreview(files.filter((_,i)=>selected.includes(items[i]))))
                .then(result=>migrationResult={names:result.map(x=>x.name)},error=>migrationResult={error:error.message});
        });
        await page.locator('#config-transfer-preview').waitFor({state:'visible'});
        await page.locator('#config-transfer-ack').check();
        assert.equal(await page.locator('#config-transfer-confirm-btn').isDisabled(),true);
        const missingText=await page.locator('#config-transfer-preview').textContent();
        assert(missingText.includes('缺失：1'));assert(!missingText.includes('PRIVATE_MISSING_SD'));
        checked('selected-missing-reference-blocks-even-disabled-rule-and-ack');
        await page.locator('#config-transfer-list input').nth(1).uncheck();
        assert.equal(await page.locator('#config-transfer-ack').isChecked(),false);
        assert((await page.locator('#config-transfer-preview').textContent()).includes('尚未解析的引用/来源：1'));
        await page.locator('#config-transfer-ack').check();
        await page.locator('#config-transfer-confirm-btn').click();
        assert.deepEqual((await page.evaluate(()=>migrationResult)).names,['periph_exec.json']);
        checked('deselected-peripheral-file-is-unresolved-not-proven-missing');
        await page.evaluate(()=>{
            const item={name:'network.json',preview:{lines:[],warnings:[]}};
            AppState._showConfigTransferModal([item],'import',()=>{throw new Error('PRIVATE_ERROR');}).catch(()=>{});
        });
        await page.locator('#config-transfer-ack').check();
        assert.equal(await page.locator('#config-transfer-confirm-btn').isDisabled(),true);
        assert(!(await page.locator('#config-transfer-preview').textContent()).includes('PRIVATE_ERROR'));
        await page.locator('#config-transfer-cancel-btn').click();checked('dependency-preview-failure-is-redacted-and-blocking');
        for (const scenario of ['wrong-type','self-pair']) {
            await page.evaluate(scenario=>{
                const files=scenario==='wrong-type'
                    ? [{name:'periph_exec.json',content:JSON.stringify({rules:[{enabled:false,actions:[{actionType:38,targetPeriphId:'PRIVATE_TARGET'}]}]})},
                       {name:'peripherals.json',content:JSON.stringify({peripherals:[{id:'PRIVATE_TARGET',type:50,enabled:false}]})}]
                    : [{name:'peripherals.json',content:JSON.stringify({peripherals:[{id:'PRIVATE_TARGET',type:11,params:{pairedInputId:'PRIVATE_TARGET'}}]})}];
                const items=files.map(f=>({name:f.name,preview:AppState._buildConfigTransferPreview(f)}));
                AppState._showConfigTransferModal(items,'import',selected=>AppState._buildConfigTransferDependencyPreview(files.filter((_,i)=>selected.includes(items[i])))).catch(()=>{});
            },scenario);
            await page.locator('#config-transfer-ack').check();
            assert.equal(await page.locator('#config-transfer-confirm-btn').isDisabled(),true);
            const text=await page.locator('#config-transfer-preview').textContent();
            assert(text.includes(scenario==='wrong-type'?'类型不兼容：1':'引用自身：1'));
            assert(!text.includes('PRIVATE_TARGET'));
            await page.locator('#config-transfer-cancel-btn').click();
            checked('selected-'+scenario+'-blocks-without-rendering-identifiers');
        }
        // Run the production entrypoint and transaction. Only file loading and
        // HTTP responses are replaced; never invoke the obsolete per-file seam.
        await page.evaluate(()=>{
            const input=document.createElement('input');input.id='dev-config-import-file';input.type='file';
            const status=document.createElement('p');status.id='dev-config-transfer-status';
            const button=document.createElement('button');button.id='dev-config-import-btn';
            document.body.append(input,status,button);
            const files=new DataTransfer();files.items.add(new File(['{}'],'bundle.json',{type:'application/json'}));input.files=files.files;
            window.outcomeWrites=[];window.outcomeErrors=[];window.outcomeSuccess=[];window.outcomeInvalidations=0;
            window.Notification={error:value=>outcomeErrors.push(value),success:value=>outcomeSuccess.push(value),warning:()=>{}};
            window.apiInvalidateCache=()=>{outcomeInvalidations++;};
            window.runOutcome=scenario=>{
                outcomeWrites=[];outcomeErrors=[];outcomeSuccess=[];outcomeInvalidations=0;
                const session='0123456789abcdef0123456789abcdef',revision='a'.repeat(64);
                AppState._readSelectedConfigEntries=async()=>scenario==='prohibited'
                    ? [{name:'network.json',content:'{"password":"PRIVATE_PASSWORD"}'}]
                    : [{name:'peripherals.json',content:JSON.stringify({peripherals:[{id:'output',type:12,enabled:true,pins:[2],params:{}}]})},
                       {name:'periph_exec.json',content:JSON.stringify({rules:[{id:'r',enabled:true,actions:[{actionType:0,targetPeriphId:'output'}]}]})}];
                window.apiGetFresh=async url=>{
                    outcomeWrites.push({method:'GET',url});
                    if(url!=='/api/config/migration')throw new Error('Unexpected read');
                    return {success:true,data:{phase:'empty'}};
                };
                window.apiPost=async(url,payload)=>{
                    outcomeWrites.push({method:'POST',url});
                    if(url.endsWith('/begin'))return {success:true,data:{phase:'receiving',session}};
                    if(url.endsWith('/chunk'))return {success:true,data:{session,name:payload.name,received:Number(payload.offset)+new TextEncoder().encode(payload.content).length}};
                    if(url.endsWith('/preview'))return {success:true,data:{session,revision,phase:'reviewed',activation:'next_boot',
                        valid:scenario!=='rejected',error:scenario==='rejected'?'MIGRATION_REFERENCE_MISSING':'',requiresConfirmation:false,issues:[]}};
                    if(url.endsWith('/prepare')){
                        if(scenario==='lost')throw new Error('simulated lost response');
                        return {success:true,data:{session,revision,phase:'prepared',applied:false,restartRequired:true}};
                    }
                    throw new Error('Unexpected mutation');
                };
                window.outcomeDone=false;AppState.importDeviceConfigBundle().then(()=>{outcomeDone=true;});
            };
            runOutcome('prohibited');
        });
        await page.locator('#config-transfer-preview').waitFor({state:'visible'});
        await page.locator('#config-transfer-ack').check();
        assert.equal(await page.locator('#config-transfer-confirm-btn').isDisabled(),true);
        assert((await page.locator('#config-transfer-preview').textContent()).includes('整组迁移不支持'));
        assert(!(await page.locator('#config-transfer-preview').textContent()).includes('PRIVATE_PASSWORD'));
        await page.locator('#config-transfer-cancel-btn').click();
        await page.waitForFunction(()=>outcomeDone);
        assert.deepEqual(await page.evaluate(()=>outcomeWrites),[]);checked('unsupported-file-blocked-before-staging');
        for(const scenario of ['rejected','lost','success']){
            await page.evaluate(scenario=>runOutcome(scenario),scenario);
            await page.locator('#config-transfer-preview').waitFor({state:'visible'});
            assert.equal(await page.locator('#dev-config-import-btn').isDisabled(),true);
            assert.deepEqual(await page.evaluate(()=>outcomeWrites),[]);
            await page.locator('#config-transfer-ack').check();
            await page.locator('#config-transfer-confirm-btn').click();
            await page.waitForFunction(()=>outcomeDone);
            const result=await page.evaluate(()=>({calls:outcomeWrites,errors:outcomeErrors,success:outcomeSuccess,invalidations:outcomeInvalidations}));
            const urls=result.calls.map(call=>call.url);
            assert.deepEqual(urls,['/api/config/migration','/api/config/migration/begin','/api/config/migration/chunk','/api/config/migration/chunk',
                '/api/config/migration/preview',...(scenario==='rejected'?[]:['/api/config/migration/prepare'])]);
            assert.equal(result.invalidations,1);
            const status=await page.locator('#dev-config-transfer-status').textContent();
            if(scenario==='success'){
                assert.deepEqual(result.errors,[]);assert.equal(result.success.length,1);
                assert(status.includes('等待下次重启应用'));assert(!status.includes('已在启动时应用'));
            }else{
                assert.equal(result.errors.length,1);assert.deepEqual(result.success,[]);
                assert(status.includes(scenario==='rejected'?'MIGRATION_REFERENCE_MISSING':'simulated lost response'));
                assert(status.includes('迁移状态'));assert(status.includes('未自动重试、清理或重启'));
            }
            assert.equal(await page.locator('#dev-config-import-btn').isDisabled(),false);
            checked('transaction-'+scenario+'-without-retry-or-reboot');
        }
        // Board review uses real DOM and production mapping, with a fixed
        // active-profile response and preserved target rule file.
        await page.evaluate(()=>{
            window.boardSource=[{name:'peripherals.json',content:JSON.stringify({peripherals:[
                {id:'output',type:12,enabled:true,pins:[2],params:{}},{id:'input',type:11,enabled:true,pins:[3],params:{}}]})}];
            window.boardOriginal=JSON.stringify(boardSource);window.boardWrites=[];
            window.targetRules=JSON.stringify({rules:[{id:'target-rule',enabled:true,actions:[{actionType:0,targetPeriphId:'output'}]}]});
            window.boardSupported=true;
            window.apiGetFresh=async url=>{
                if(url==='/api/boards/profiles')return {success:true,data:{activeProfileSupported:boardSupported,activeProfileId:'fixture',compiledChip:'esp32',profiles:[
                    {id:'fixture',active:true,displayName:'Fixture board',version:'1',pins:[{gpio:18,policy:'recommended'},{gpio:19,policy:'conditional'},{gpio:6,policy:'hard_forbidden'}]}]}};
                if(url==='/api/config/transfer/list')return {success:true,data:{files:[{name:'periph_exec.json'}]}};
                throw new Error('Unexpected board read');
            };
            window.apiPost=async(url,body)=>{boardWrites.push({url,body});throw new Error('Board review must not write');};
            AppState._configTransferFetch=async url=>{
                if(url!=='/api/config/transfer/export?name=periph_exec.json')throw new Error('Unexpected export');
                return {ok:true,text:async()=>targetRules};
            };
            window.openBoard=()=>{window.boardResult=null;AppState._reviewBoardMigration(boardSource)
                .then(result=>boardResult=result,error=>boardResult={error:error.message});};
            openBoard();
        });
        await page.locator('#config-board-migration-modal').waitFor({state:'visible'});
        assert((await page.locator('#config-board-profile').textContent()).includes('Fixture board'));
        const selects=page.locator('#config-board-pins select');assert.equal(await selects.count(),2);
        assert.deepEqual(await selects.evaluateAll(nodes=>nodes.map(node=>node.value)),['','']);
        assert.equal(await page.locator('#config-board-pins option[value="6"]').count(),0);
        await page.locator('#config-board-ack').check();
        assert.equal(await page.locator('#config-board-confirm').isDisabled(),true);
        await selects.nth(0).selectOption('18');await selects.nth(1).selectOption('18');
        await page.locator('#config-board-ack').check();
        assert.equal(await page.locator('#config-board-confirm').isDisabled(),true);
        assert((await page.locator('#config-board-status').textContent()).includes('不能合并'));
        await selects.nth(1).selectOption('19');
        assert.equal(await page.locator('#config-board-ack').isChecked(),false);
        assert.equal(await page.locator('#config-board-confirm').isDisabled(),true);
        checked('board-explicit-mapping-no-forbidden-or-merged-pins-and-consent-reset');
        await page.locator('#config-board-ack').check();
        assert.deepEqual(await page.evaluate(()=>{
            const box=document.querySelector('#config-board-migration-modal .modal-content');
            return {page:document.documentElement.scrollWidth>innerWidth+1,modal:box.scrollWidth>box.clientWidth+1};
        }),{page:false,modal:false});
        await page.screenshot({path:path.join(out,'board-'+viewport.width+'.png'),fullPage:true});
        await page.locator('#config-board-confirm').click();
        await page.waitForFunction(()=>boardResult!==null);
        const mapped=await page.evaluate(()=>boardResult);
        const peripherals=JSON.parse(mapped.files.find(file=>file.name==='peripherals.json').content).peripherals;
        assert.deepEqual(peripherals.map(p=>({id:p.id,pins:p.pins,enabled:p.enabled})),[
            {id:'output',pins:[18],enabled:false},{id:'input',pins:[19],enabled:false}]);
        assert.equal(JSON.parse(mapped.files.find(file=>file.name==='periph_exec.json').content).rules[0].enabled,false);
        assert.deepEqual(mapped.baselines,[{name:'periph_exec.json',content:await page.evaluate(()=>targetRules)},{name:'rule_scripts.json',content:''}]);
        assert.equal(await page.evaluate(()=>JSON.stringify(boardSource)),await page.evaluate(()=>boardOriginal));
        assert.deepEqual(await page.evaluate(()=>boardWrites),[]);checked('board-preserves-source-and-target-rule-baseline-pauses-candidate');
        await page.evaluate(()=>openBoard());
        await page.locator('#config-board-migration-modal').waitFor({state:'visible'});
        assert.equal(await page.locator('#config-board-ack').isChecked(),false);
        await page.locator('#config-board-cancel').click();
        assert((await page.evaluate(()=>boardResult)).error.includes('取消'));
        assert.deepEqual(await page.evaluate(()=>boardWrites),[]);checked('board-cancel-does-not-stage');
        await page.evaluate(()=>{boardSupported=false;openBoard();});
        await page.waitForFunction(()=>boardResult!==null);
        assert((await page.evaluate(()=>boardResult)).error.includes('活动板型'));
        assert.deepEqual(await page.evaluate(()=>boardWrites),[]);checked('unknown-board-rejected-without-writes');
        assert.deepEqual(errors,[]);assert.deepEqual(requests,[]);assert.equal(await page.evaluate(()=>migrationApiCalls.length),0);checked('no-api-no-device-no-browser-errors');
        await page.close();
    }
    report.inputsStable=sources.every(s=>crypto.createHash('sha256').update(fs.readFileSync(path.join(root,s.file))).digest('hex')===s.sha256);
    assert(report.inputsStable);report.status='PASS';
}catch(error){report.status='FAIL';report.error=error.stack;process.exitCode=1;}finally{if(browser)await browser.close();report.finishedAt=new Date().toISOString();save();console.log(JSON.stringify({status:report.status,cases:report.cases.length,error:report.error}));}})();
