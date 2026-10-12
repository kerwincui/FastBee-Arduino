'use strict';
const fs=require('fs'),path=require('path'),crypto=require('crypto'),assert=require('assert/strict');
const root=path.resolve(__dirname,'..');
const {chromium}=require(require.resolve('playwright',{paths:[path.join(root,'test/browser')]}));
const out=process.argv[2]||path.join(root,'artifacts','sd-quality-'+new Date().toISOString().replace(/[:.]/g,'-')+'-'+process.pid);
if(fs.existsSync(out))throw new Error('Fresh evidence directory required');
fs.mkdirSync(out,{recursive:true});
const files=['web-src/pages/fragments/peripheral-io.html','web-src/css/main.css','web-src/modules/runtime/peripheral-io.js'];
const sources=files.map(file=>({file,text:fs.readFileSync(path.join(root,file),'utf8'),sha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(root,file))).digest('hex')}));
const report={status:'RUNNING',startedAt:new Date().toISOString(),scope:'Offline real SD modal/CSS/JS with mocked APIs; no real network, card I/O, device writes or physical acceptance',sources:sources.map(({file,sha256})=>({file,sha256})),cases:[]};
const save=()=>fs.writeFileSync(path.join(out,'report.json'),JSON.stringify(report,null,2));save();
(async()=>{let browser;try{
  browser=await chromium.launch({headless:true});
  for(const viewport of [{width:1280,height:800},{width:390,height:844}]){
    const page=await browser.newPage({viewport}),errors=[],requests=[];
    page.on('pageerror',error=>errors.push(error.message));
    await page.route('**/*',route=>{requests.push(route.request().url());return route.abort();});
    await page.setContent(sources[0].text);await page.addStyleTag({content:sources[1].text});
    await page.evaluate(()=>{
      window.AppState={registerModule(name,methods){Object.assign(this,methods);},
        isDeveloperModeEnabled:()=>true,guardDeveloperModeAction:()=>true,
        _peripheralIoErrorMessage:(error,fallback)=>error.message||fallback,
        hideModal:id=>document.getElementById(id).style.display='none'};
      window.sdSample=()=>({statusAvailable:true,mounted:true,capacityQuality:'last_operation_snapshot',capacityAgeMs:42,cardType:'SDHC',totalBytes:4096,usedBytes:128});
      window.calls=[];window.notices=[];window.Notification={success:(...args)=>notices.push(args)};
      window.sdGet=async()=>({success:true,data:sdSample()});window.sdPost=async()=>({success:true});
      window.apiGet=async(...args)=>{calls.push(['get',...args]);return sdGet(...args);};
      window.apiPost=async(...args)=>{calls.push(['post',...args]);return sdPost(...args);};
    });
    await page.addScriptTag({content:sources[2].text});
    await page.evaluate(()=>{
      AppState._periphIoSession={};AppState._periphIoId='sd-a';AppState._periphIoType=37;AppState._periphIoKind='sd';
      document.getElementById('peripheral-io-modal').style.display='flex';
      document.getElementById('peripheral-sd-panel').classList.remove('fb-hidden');
      document.getElementById('peripheral-io-title').textContent='SD 卡诊断';
      document.getElementById('peripheral-io-name').textContent='SD 本地模拟';
      AppState._bindPeripheralIoModalEvents();AppState._renderPeripheralSdData(sdSample());
    });
    const check=name=>{report.cases.push({name,width:viewport.width,status:'PASS'});save();};
    assert.equal(await page.locator('#peripheral-sd-mounted').textContent(),'已挂载');
    assert((await page.locator('#peripheral-sd-filesystem').textContent()).includes('上次操作 · 42 ms 前'));check('capacity-is-an-aged-snapshot');
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1));check('page-without-horizontal-overflow');
    assert(await page.locator('.modal-content').evaluate(el=>el.scrollWidth<=el.clientWidth+1));check('modal-without-hidden-overflow');
    assert(await page.locator('#peripheral-sd-panel').evaluate(panel=>{
      const box=panel.getBoundingClientRect();
      return Array.from(panel.querySelectorAll('button')).every(button=>{
        const rect=button.getBoundingClientRect();return rect.left>=box.left-1&&rect.right<=box.right+1;
      });
    }));check('sd-action-buttons-fit-panel');
    await page.screenshot({path:path.join(out,'sd-'+viewport.width+'.png'),fullPage:true});
    await page.evaluate(()=>AppState._renderPeripheralSdData({statusAvailable:false,driverError:'SD_BUSY: storage status unavailable'}));
    assert.equal(await page.locator('#peripheral-sd-mounted').textContent(),'状态未确认');
    assert.equal(await page.locator('#peripheral-sd-total').textContent(),'--');
    assert((await page.locator('#peripheral-io-error').textContent()).includes('SD_BUSY'));check('busy-not-unmounted-or-zero');
    await page.locator('#peripheral-sd-content').fill('keep draft');
    await page.evaluate(()=>{sdGet=async()=>{throw new Error('simulated offline');};});
    await page.locator('#peripheral-io-refresh-btn').click();await page.waitForFunction(()=>!AppState._periphIoBusy);
    assert.equal(await page.locator('#peripheral-sd-used').textContent(),'--');
    assert.equal(await page.locator('#peripheral-sd-content').inputValue(),'keep draft');check('failed-refresh-keeps-only-draft');
    await page.evaluate(()=>{sdGet=()=>new Promise(resolve=>window.finishOldRead=resolve);});
    await page.locator('#peripheral-sd-read-btn').click();await page.waitForFunction(()=>typeof finishOldRead==='function');
    await page.evaluate(()=>{AppState._periphIoSession={};AppState._periphIoId='sd-b';document.getElementById('peripheral-sd-content').value='new draft';AppState._setPeripheralIoBusy(false);finishOldRead({success:true,data:{content:'old file'}});});
    await page.evaluate(()=>Promise.resolve());
    assert.equal(await page.locator('#peripheral-sd-content').inputValue(),'new draft');check('late-read-does-not-overwrite-new-draft');
    await page.evaluate(()=>{sdPost=async()=>({});});
    await page.locator('#peripheral-sd-write-btn').click();await page.waitForFunction(()=>!AppState._periphIoBusy);
    assert((await page.locator('#peripheral-io-error').textContent()).includes('结果未确认'));
    assert.equal(await page.evaluate(()=>notices.length),0);check('ambiguous-write-no-success-or-retry');
    await page.evaluate(()=>{sdGet=async()=>({success:true,data:sdSample()});sdPost=async()=>({success:true});calls=[];});
    await page.locator('#peripheral-sd-write-btn').click();await page.waitForFunction(()=>!AppState._periphIoBusy);
    assert.deepEqual(await page.evaluate(()=>calls.map(c=>[c[0],c[2].id])),[['post','sd-b'],['get','sd-b']]);check('confirmed-write-refreshes-same-target-once');
    await page.evaluate(()=>{sdPost=()=>new Promise(resolve=>window.finishOldWrite=resolve);calls=[];notices=[];});
    await page.locator('#peripheral-sd-mount-btn').click();await page.waitForFunction(()=>typeof finishOldWrite==='function');
    await page.evaluate(()=>{AppState._periphIoSession={};AppState._periphIoId='sd-c';AppState._setPeripheralIoBusy(false);AppState._setPeripheralIoOutput('peripheral-sd-output','new-session');finishOldWrite({success:true});});
    await page.evaluate(()=>Promise.resolve());
    assert.equal(await page.locator('#peripheral-sd-output').textContent(),'new-session');
    assert.deepEqual(await page.evaluate(()=>calls.map(c=>[c[0],c[2].id])),[['post','sd-b']]);
    assert.equal(await page.evaluate(()=>notices.length),0);check('late-write-cannot-refresh-new-target');
    assert.deepEqual(errors,[]);assert.deepEqual(requests,[]);check('no-real-network-or-page-errors');await page.close();
  }
  report.inputsStable=sources.every(source=>crypto.createHash('sha256').update(fs.readFileSync(path.join(root,source.file))).digest('hex')===source.sha256);
  assert(report.inputsStable,'Production inputs changed during browser test');report.status='PASS';
}catch(error){report.status='FAIL';report.error=error.stack||String(error);process.exitCode=1;}
finally{if(browser)await browser.close();report.finishedAt=new Date().toISOString();save();console.log(JSON.stringify({status:report.status,cases:report.cases.length,report:path.join(out,'report.json')}));}})();
