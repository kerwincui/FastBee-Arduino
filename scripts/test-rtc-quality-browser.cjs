'use strict';
const fs=require('fs'),path=require('path'),crypto=require('crypto'),assert=require('assert/strict');
const root=path.resolve(__dirname,'..');
const {chromium}=require(require.resolve('playwright',{paths:[path.join(root,'test/browser')]}));
const out=process.argv[2]||path.join(root,'artifacts','rtc-quality-'+new Date().toISOString().replace(/[:.]/g,'-')+'-'+process.pid);
if(fs.existsSync(out))throw new Error('Fresh evidence directory required');
fs.mkdirSync(out,{recursive:true});
const files=['web-src/pages/fragments/peripheral-io.html','web-src/css/main.css','web-src/modules/runtime/peripheral-io.js'];
const sources=files.map(file=>({file,text:fs.readFileSync(path.join(root,file),'utf8'),sha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(root,file))).digest('hex')}));
const report={status:'RUNNING',startedAt:new Date().toISOString(),scope:'Offline production RTC modal/JS, mocked API boundaries; no real network, RTC writes or devices',sources:sources.map(({file,sha256})=>({file,sha256})),cases:[]};
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
      window.rtcSample=()=>({initialized:true,running:true,quality:'calendar_unverified',timeBasis:'device_calendar_unzoned',formatted:'2026-10-01 10:20:30'});
      window.calls=[];window.rtcGet=async()=>({success:true,data:rtcSample()});window.rtcPost=async()=>({success:true});
      window.apiGet=async(...args)=>{calls.push(['get',...args]);return rtcGet(...args);};
      window.apiPost=async(...args)=>{calls.push(['post',...args]);return rtcPost(...args);};
    });
    await page.addScriptTag({content:sources[2].text});
    await page.evaluate(()=>{
      AppState._periphIoSession={};AppState._periphIoId='rtc-a';AppState._periphIoType=50;AppState._periphIoKind='ds1302';
      document.getElementById('peripheral-io-modal').style.display='flex';
      document.getElementById('peripheral-ds1302-panel').classList.remove('fb-hidden');
      document.getElementById('peripheral-io-title').textContent='DS1302 实时时钟';
      document.getElementById('peripheral-io-name').textContent='RTC 本地模拟';
      AppState._bindPeripheralIoModalEvents();AppState._renderPeripheralDs1302Data(rtcSample());
    });
    const check=name=>{report.cases.push({name,width:viewport.width,status:'PASS'});save();};
    assert((await page.locator('#peripheral-ds1302-running').textContent()).includes('时间源未核验'));
    assert((await page.locator('#peripheral-ds1302-output').textContent()).includes('device_calendar_unzoned'));check('sample-exposes-unverified-source');
    await page.locator('#peripheral-ds1302-datetime').fill('2026-10-01T10:20:30');
    await page.evaluate(()=>AppState._renderPeripheralDs1302Data({...rtcSample(),running:false,quality:'halted'}));
    assert((await page.locator('#peripheral-ds1302-running').textContent()).includes('非运行时钟'));check('halted-calendar-not-current');
    const overflow=await page.evaluate(()=>({page:document.documentElement.scrollWidth>innerWidth+1,modal:document.querySelector('.modal-content').scrollWidth>document.querySelector('.modal-content').clientWidth+1}));
    assert.deepEqual(overflow,{page:false,modal:false});check('no-horizontal-overflow');
    assert(await page.locator('#peripheral-ds1302-panel .peripheral-io-actions').evaluate(row=>{
      const box=row.getBoundingClientRect();
      return Array.from(row.querySelectorAll('button')).every(button=>{
        const rect=button.getBoundingClientRect();return rect.left>=box.left-1&&rect.right<=box.right+1;
      });
    }));check('all-clock-action-buttons-visible');
    await page.screenshot({path:path.join(out,'rtc-'+viewport.width+'.png'),fullPage:true});
    await page.evaluate(()=>{rtcGet=async()=>{throw new Error('simulated offline');};});
    await page.locator('#peripheral-io-refresh-btn').click();
    await page.waitForFunction(()=>!AppState._periphIoBusy);
    assert.equal(await page.locator('#peripheral-ds1302-current').textContent(),'--');
    assert((await page.locator('#peripheral-ds1302-output').textContent()).includes('旧日历不可'));
    assert.equal(await page.locator('#peripheral-ds1302-datetime').inputValue(),'2026-10-01T10:20:30');check('failed-read-clears-sample-keeps-draft');
    await page.evaluate(()=>{rtcPost=()=>new Promise(resolve=>window.finishOldWrite=resolve);});
    await page.locator('#peripheral-ds1302-start-btn').click();
    await page.waitForFunction(()=>typeof finishOldWrite==='function');
    await page.evaluate(()=>{
      AppState.closePeripheralIoModal();
      AppState._periphIoSession={};AppState._periphIoId='rtc-b';AppState._periphIoType=50;AppState._periphIoKind='ds1302';
      document.getElementById('peripheral-io-modal').style.display='flex';
      AppState._clearPeripheralDs1302Reading('new-session');AppState._setPeripheralIoBusy(false);
      finishOldWrite({success:true});
    });
    await page.evaluate(()=>Promise.resolve());
    assert.equal(await page.locator('#peripheral-ds1302-output').textContent(),'new-session');
    assert.deepEqual(await page.evaluate(()=>calls.map(c=>[c[0],c[2].id])),[['get','rtc-a'],['post','rtc-a']]);check('late-write-does-not-touch-new-session');
    await page.evaluate(()=>{rtcPost=async()=>({});});
    await page.locator('#peripheral-ds1302-start-btn').click();await page.waitForFunction(()=>!AppState._periphIoBusy);
    assert((await page.locator('#peripheral-ds1302-output').textContent()).includes('写入结果未确认'));check('ambiguous-write-no-auto-retry');
    assert.deepEqual(errors,[]);assert.deepEqual(requests,[]);check('no-page-errors-or-real-requests');
    await page.close();
  }
  report.inputsStable=sources.every(source=>crypto.createHash('sha256').update(fs.readFileSync(path.join(root,source.file))).digest('hex')===source.sha256);
  assert(report.inputsStable,'Production inputs changed during browser tests');report.status='PASS';
}catch(error){report.status='FAIL';report.error=error.stack||String(error);process.exitCode=1;}
finally{if(browser)await browser.close();report.finishedAt=new Date().toISOString();save();console.log(JSON.stringify({status:report.status,cases:report.cases.length,report:path.join(out,'report.json')}));}})();
