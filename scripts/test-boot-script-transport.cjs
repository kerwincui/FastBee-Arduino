// Standalone localhost-only Chromium regression; not part of normal unit tests.
// Run under scripts/firmware-build-lock.ps1; --out must name a new evidence directory.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const http = require('node:http');
const root = path.resolve(__dirname, '..');
const { chromium } = require(path.join(root, 'test/browser/node_modules/@playwright/test'));
const { minifyJS } = require(path.join(root, 'scripts/minify-js'));
const proposal = fs.readFileSync(path.join(root, 'web-src/index.html'), 'utf8');
const scripts = [...proposal.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(match => match[1]);
const records = [];
const outputArg = process.argv.slice(2).find(arg => arg.startsWith('--out='));
const outputDir = path.resolve(root, outputArg ? outputArg.slice(6) : 'artifacts/boot-transport-' + Date.now());
assert(!fs.existsSync(outputDir), 'preserve previous evidence: output directory must be new');
fs.mkdirSync(outputDir, {recursive: true});
const reportPath = path.join(outputDir, 'report.json');
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(check) { const end=Date.now()+6000; while(!check()){assert(Date.now()<end,'deadline');await wait(10);} }

let mode='normal', minified=false, csp=false, events=[], active=0, maximum=0, attempts=new Map();
function documentHtml(){
    const code=scripts.slice(0,3).map(code=>code.replace('}, 8000);','}, 180);').replace('var bootLoginWaitMs = 15000;','var bootLoginWaitMs = 50;'));
    return '<!doctype html><form id="login-form"><input id="username" value="tester"><input id="password" value="fixture"><input id="remember" type="checkbox"><button id="login-button">Login</button></form>' + code.map(code=>'<script nonce="proposal">'+(minified?minifyJS(code):code)+'</script>').join('');
}
const chunks={
    '/js/chunk-1-core.js':'var BootGlobal=41;window.coreCount=(window.coreCount||0)+1;',
    '/js/chunk-2-app.js':'let BootLexical=BootGlobal+1;window.appCount=(window.appCount||0)+1;',
    '/js/chunk-3-state.js':'window.globalResult=BootLexical;window.stateCount=(window.stateCount||0)+1;window.__fastbeeBootReady=true;window.AppState={handleLogin:function(){window.normalLogin=1;}};'
};
const server=http.createServer((req,res)=>{
    const pathname=new URL(req.url,'http://127.0.0.1').pathname;
    if(pathname==='/'){res.setHeader('Content-Type','text/html');if(csp)res.setHeader('Content-Security-Policy',"default-src 'none'; script-src 'nonce-proposal'; connect-src 'self'; style-src 'unsafe-inline'");res.end(documentHtml());return;}
    if(pathname==='/favicon.ico'){res.writeHead(404);res.end();return;}
    active++;maximum=Math.max(maximum,active);
    const item={pathname,url:req.url,at:Date.now(),closed:null,aborted:false};events.push(item);
    res.once('close',()=>{active--;item.closed=Date.now();item.aborted=!res.writableFinished;});
    if(pathname==='/api/auth/login'){res.setHeader('Content-Type','application/json');res.end('{"success":false,"error":"fixture denied"}');return;}
    const attempt=(attempts.get(pathname)||0)+1;attempts.set(pathname,attempt);
    if((mode==='header-stall'||mode==='body-stall')&&pathname==='/js/chunk-1-core.js'&&attempt===1){
        if(mode==='body-stall'){res.setHeader('Content-Type','application/javascript');res.write('window.lateCount=(window.lateCount||0)+1;');}
        setTimeout(()=>{if(!res.destroyed){res.setHeader('Content-Type','application/javascript');res.end('window.lateCount=(window.lateCount||0)+1;');}},700);return;
    }
    if(mode==='mime'&&pathname==='/js/chunk-1-core.js'){res.setHeader('Content-Type','text/html');res.end('window.badMimeCount=1;');return;}
    if(mode==='runtime-error'&&pathname==='/js/chunk-1-core.js'){res.setHeader('Content-Type','application/javascript');res.end('window.partialCount=(window.partialCount||0)+1;throw "fixture primitive execution error";');return;}
    res.setHeader('Content-Type','application/javascript; charset=utf-8');res.setHeader('Cache-Control','max-age=3600');res.end(chunks[pathname]||'');
});

(async()=>{
    await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
    const origin='http://127.0.0.1:'+server.address().port;
    const browser=await chromium.launch({headless:true});
    try{
        for(minified of [false,true])for(mode of ['normal','header-stall','body-stall','mime','runtime-error']){
            csp=true;events=[];active=0;maximum=0;attempts=new Map();
            const context=await browser.newContext({serviceWorkers:'block'});
            const page=await context.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));
            await page.goto(origin,{waitUntil:'domcontentloaded'});
            if(mode!=='normal')await page.locator('#login-button').click();
            if(mode==='normal'||mode.endsWith('stall')){
                await page.waitForFunction(()=>window.__fastbeeBootReady===true,{},{timeout:6000});
                const result=await page.evaluate(()=>({global:window.globalResult,core:window.coreCount,app:window.appCount,state:window.stateCount,late:window.lateCount||0}));
                assert.deepEqual(result,{global:42,core:1,app:1,state:1,late:0});
                assert.equal(errors.length,0);
            }else{
                await page.waitForFunction(()=>window.__fastbeeChunkGaveUp===true,{},{timeout:6000});
                const result=await page.evaluate(()=>({partial:window.partialCount||0,badMime:window.badMimeCount||0,ready:window.__fastbeeBootReady===true}));
                assert.equal(result.badMime,0);assert.equal(result.ready,false);
                assert.equal(result.partial,mode==='runtime-error'?1:0);
                assert.equal(attempts.get('/js/chunk-1-core.js'),mode==='runtime-error'?1:4);
                assert.equal(attempts.get('/js/chunk-2-app.js')||0,0,'do not execute dependencies after failure');
            }
            if(mode!=='normal'){
                await until(()=>events.some(e=>e.pathname==='/api/auth/login'));
                await page.waitForFunction(()=>!document.getElementById('login-button').disabled);
            }
            await wait(750);
            assert.equal(maximum,1,'single active device response');
            const js=events.filter(e=>e.pathname.endsWith('.js'));
            assert(js.every(e=>e.url.includes('v=20260519-1822')),'version query retained');
            if(mode.endsWith('stall')){
                assert.equal(js[0].aborted,true,'stalled response cancelled');
                const login=events.find(e=>e.pathname==='/api/auth/login');
                assert(login.at>=js[0].closed,'fallback follows actual response close');
                assert(login.at-js[0].at<1200,'fallback cannot wait through all boot retries');
                assert.equal(await page.evaluate(()=>window.lateCount||0),0);
            }
            if(mode==='normal'){
                const networkCount=events.filter(e=>e.pathname.endsWith('.js')).length;
                await page.reload({waitUntil:'domcontentloaded'});
                await page.waitForFunction(()=>window.__fastbeeBootReady===true,{},{timeout:6000});
                assert.equal(await page.evaluate(()=>window.globalResult),42);
                assert.equal(events.filter(e=>e.pathname.endsWith('.js')).length,networkCount,'normal HTTP cache preserved across reload');
            }
            records.push({test:'chromium-complete-body-boot',mode,minified,cspNonce:true,status:'PASS',maximumActiveResponses:maximum,events:JSON.parse(JSON.stringify(events)),pageErrors:errors});
            await context.close();
        }
    }finally{await browser.close();server.closeAllConnections();await new Promise(resolve=>server.close(resolve));}
    fs.writeFileSync(reportPath,JSON.stringify({status:'PASS_OFFLINE_BOOT_TRANSPORT',deviceRequests:0,inputSha256:require('node:crypto').createHash('sha256').update(proposal).digest('hex'),tests:records},null,2)+'\n');
    console.log(records.length+' offline boot transport checks passed: '+reportPath);
})().catch(error=>{fs.writeFileSync(reportPath.replace(/\.json$/,'.failed.json'),JSON.stringify({status:'FAIL',error:error.stack,records},null,2));console.error(error);server.closeAllConnections();server.close();process.exitCode=1;});
