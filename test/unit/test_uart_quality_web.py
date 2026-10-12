"""Prepared UART UI regressions, actual JS with offline transport boundaries."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert=require('assert/strict'),fs=require('fs'),vm=require('vm');
const text={},calls=[],notices=[],elements=new Map(),cleared=[];
const el=id=>{if(!elements.has(id))elements.set(id,{value:'',classList:{toggle(){},add(){},remove(){}}});return elements.get(id);};
el('peripheral-uart-mode').value='text';el('peripheral-uart-payload').value='hello';el('peripheral-uart-poll-interval').value='500';
const app={registerModule(name,methods){Object.assign(this,methods);}};
const sample=()=>({localReady:true,peerVerified:false,port:1,framing:'8N1',rxAvailable:0,bytesSent:5,bytesReceived:2,length:2,text:'OK',hex:'4F 4B',readQuality:'bytes'});
let get=async()=>({success:true,data:sample()}),post=async()=>({success:true});
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/peripheral-io.js','utf8'),{
    AppState:app,document:{getElementById:el},TextEncoder,console,
    Notification:{success:value=>notices.push(value)},clearInterval:id=>cleared.push(id),
    apiGet:async(...args)=>{calls.push(['get',...args]);return get(...args);},
    apiPost:async(...args)=>{calls.push(['post',...args]);return post(...args);}
});
app._setPeripheralIoText=(id,value)=>text[id]=value;
app._setPeripheralIoError=value=>text.error=value;
app._setPeripheralIoBusy=value=>app._periphIoBusy=value;
app.isDeveloperModeEnabled=()=>true;
app._periphIoId='uart-a';app._periphIoType=1;app._periphIoKind='uart';app._periphIoSession={};app._periphIoPollTimer=123;
app._periphUartTextLog='HISTORY';app._periphUartHexLog='01';
const quality=()=>text['peripheral-uart-read-quality'];
const cases={
    received(){app._renderPeripheralIoReading(sample());assert.equal(app._periphUartTextLog,'HISTORY\nOK');assert(quality().includes('未验证完整协议帧'));assert(text['peripheral-uart-connected'].includes('对端未验证'));},
    empty(){app._renderPeripheralIoReading({...sample(),length:0,text:'',hex:'',readQuality:'no_data'});assert.equal(app._periphUartTextLog,'HISTORY');assert(quality().includes('无新字节'));},
    inconsistent(){for(const changed of [{length:-1},{length:129},{length:0},{readQuality:'trusted'},{readQuality:'no_data'},{length:1.5}]){
        assert.throws(()=>app._renderPeripheralIoReading({...sample(),...changed}));assert.equal(app._periphUartTextLog,'HISTORY');}},
    legacy(){const value=sample();delete value.readQuality;delete value.localReady;value.connected=true;app._renderPeripheralIoReading(value);assert(quality().includes('旧固件'));assert(text['peripheral-uart-connected'].includes('本地就绪'));},
    status(){app._renderPeripheralIoReading(sample());const before=app._periphUartTextLog,old=quality();app._renderPeripheralIoReading({localReady:false,rxAvailable:0});assert.equal(app._periphUartTextLog,before);assert.equal(quality(),old);assert.equal(text['peripheral-uart-connected'],'本地未就绪');},
    failure_stops_polling:async()=>{get=async()=>{throw new Error('UART_READ_FAILED');};await app.refreshPeripheralIo(true);
        assert.equal(calls.length,1);assert.equal(app._periphUartTextLog,'HISTORY');assert(quality().includes('可能已消费'));assert.equal(el('peripheral-uart-poll-interval').value,'0');assert.deepEqual(cleared,[123]);assert.equal(app._periphIoBusy,false);},
    missing_length:async()=>{get=async()=>({success:true,data:{localReady:true}});await app.refreshPeripheralIo(true);assert(text.error.includes('长度未确认'));assert.equal(app._periphUartTextLog,'HISTORY');assert.equal(calls.length,1);},
    pending:async()=>{let resolve;get=()=>new Promise(r=>resolve=r);const work=app.refreshPeripheralIo(true);assert(quality().includes('读取中'));assert.equal(app._periphUartTextLog,'HISTORY');resolve({success:true,data:sample()});await work;assert(quality().includes('本次 2'));},
    late_read:async()=>{let resolve;get=()=>new Promise(r=>resolve=r);const work=app.refreshPeripheralIo(true);app._periphIoSession={};app._periphIoBusy=true;app._periphUartTextLog='NEW';resolve({success:true,data:sample()});await work;assert.equal(app._periphUartTextLog,'NEW');assert.equal(app._periphIoBusy,true);},
    late_send:async()=>{let resolve;post=()=>new Promise(r=>resolve=r);const work=app.sendPeripheralUart();app._periphIoSession={};app._periphIoId='uart-b';app._periphIoBusy=true;resolve({success:true});await work;assert.equal(calls.length,1);assert.equal(calls[0][2].id,'uart-a');assert.equal(notices.length,0);assert.equal(app._periphIoBusy,true);},
    late_send_failure:async()=>{let reject;post=()=>new Promise((r,j)=>reject=j);const work=app.sendPeripheralUart();app._periphIoSession={};text.error='NEW';reject(new Error('OLD'));await work;assert.equal(text.error,'NEW');assert.equal(cleared.length,0);assert.equal(calls.length,1);},
    send_success:async()=>{get=async()=>({success:true,data:{localReady:true,rxAvailable:0}});await app.sendPeripheralUart();assert.equal(calls.length,2);assert.equal(calls[1][1],'/api/peripherals/status');assert(notices[0].includes('本地驱动已接受'));assert(notices[0].includes('未验证'));assert.equal(app._periphUartTextLog,'HISTORY');},
    send_unconfirmed:async()=>{post=async()=>({message:'ambiguous'});await app.sendPeripheralUart();assert.equal(calls.length,1);assert.equal(notices.length,0);assert(text.error.includes('可能已部分生效'));assert.equal(el('peripheral-uart-poll-interval').value,'0');},
    unauthorized:async()=>{app.isDeveloperModeEnabled=()=>false;await app.sendPeripheralUart();assert.equal(calls.length,0);},
    bounded_history(){for(let n=0;n<2000;n++)app._renderPeripheralIoReading(sample());assert(app._periphUartTextLog.length<=8192&&app._periphUartHexLog.length<=8192);}
};
Promise.resolve(cases[process.argv[1]]()).catch(error=>{console.error(error);process.exitCode=1;});
'''


class UartQualityWebTests(unittest.TestCase):
    def run_case(self, name):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run([node, '-e', SCRIPT, name], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _name in ('received', 'empty', 'inconsistent', 'legacy', 'status', 'failure_stops_polling',
              'missing_length', 'pending', 'late_read', 'late_send', 'late_send_failure',
              'send_success', 'send_unconfirmed', 'unauthorized', 'bounded_history'):
    setattr(UartQualityWebTests, 'test_' + _name, lambda self, name=_name: self.run_case(name))


if __name__ == '__main__':
    unittest.main()
