'use strict';
const fs=require('node:fs'),os=require('node:os'),path=require('node:path'),{spawn}=require('node:child_process'),assert=require('node:assert/strict');
const [app,root,chromium]=process.argv.slice(2),tmp=fs.mkdtempSync(path.join(os.tmpdir(),'firefly-setup-ui-'));
fs.mkdirSync(path.join(tmp,'store'),{mode:0o700});
const log=fs.openSync(path.join(tmp,'server.log'),'w');
const server=spawn(app,[path.join(tmp,'store'),path.join(root,'config/empty-home.json')],{stdio:['ignore','pipe',log]});
let output='',chrome,socket,id=0;server.stdout.on('data',b=>output+=b);const pending=new Map(),exceptions=[];
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function until(fn,label){for(let i=0;i<300;i++){if(await fn())return;await delay(100);}throw Error('Timeout '+label);}
function send(method,params={}){return new Promise((resolve,reject)=>{const current=++id;pending.set(current,{resolve,reject});socket.send(JSON.stringify({id:current,method,params}));});}
async function js(expression){const r=await send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;}
async function idle(){await until(()=>js('!operation'),'idle');}
async function click(id){await idle();await js(`document.getElementById(${JSON.stringify(id)}).click()`);await idle();}
(async()=>{
 await until(()=>output.includes('DEMO_LISTEN'),'listener');const url=output.match(/DEMO_LISTEN (http:\/\/[^\s]+)/)[1];
 const config=async()=>await(await fetch(url+'/api/config')).json();const before=await config();
 chrome=spawn(chromium,['--ozone-platform=x11','--no-sandbox','--disable-gpu','--disable-dev-shm-usage','--no-first-run','--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream','--remote-debugging-port=0','--user-data-dir='+path.join(tmp,'chrome'),'about:blank'],{stdio:['ignore',log,log]});
 const portFile=path.join(tmp,'chrome','DevToolsActivePort');await until(()=>fs.existsSync(portFile),'Chrome');
 const port=fs.readFileSync(portFile,'utf8').split('\n')[0],pages=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();
 socket=new WebSocket(pages.find(p=>p.type==='page').webSocketDebuggerUrl);await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
 socket.onmessage=e=>{const r=JSON.parse(e.data);if(r.method==='Runtime.exceptionThrown')exceptions.push(r.params);if(r.id){const p=pending.get(r.id);pending.delete(r.id);r.error?p.reject(r.error):p.resolve(r.result);}};
 await send('Page.enable');await send('Runtime.enable');await send('Page.navigate',{url});
 await until(()=>js(`typeof draft!=='undefined'&&draft&&!operation&&sttReady`),'UI/STT');
 assert.equal((await(await fetch(url+'/api/setup/status')).json()).active,false,'page does not pause assistant');
 await click('copilot-propose');assert.ok(await js(`document.getElementById('errors').textContent.includes('Начать настройку')`));
 await click('setup-start');assert.equal(await js('setupActive'),true);
 const blocked=await(await fetch(url+'/api/test-command',{method:'POST',headers:{'Content-Type':'application/json','X-Firefly-Admin':'1'},body:JSON.stringify({text:'включи свет',execute:true})})).json();
 assert.equal(blocked.ok,false);assert.equal(blocked.errors[0].code,'setup_in_progress');
 // Real browser capture lifecycle, test-only generated source. Fill the actual
 // processor to its 60s cap; POST a real PCM WAV through HttpServer/AdminTranscriber.
 await click('copilot-mic');await until(()=>js(`voiceState==='recording'`),'browser microphone');
 await js(`window.recordedTracks=voiceRec.stream.getTracks();window.captureRate=voiceRec.sampleRate;
   const processor=voiceRec.processor, chunk=new Float32Array(4096).fill(0.02);
   for(let i=0;i<Math.ceil(captureRate*60/4096)+1&&voiceRec;i++)processor.onaudioprocess({inputBuffer:{getChannelData:()=>chunk}});`);
 await until(()=>js(`voiceState==='idle'&&!voiceRec&&document.getElementById('copilot-text').value.includes('Создай')`),'transcription at recording cap');
 assert.equal(await js(`recordedTracks.every(t=>t.readyState==='ended')`),true);
 assert.equal((await config()).hash,before.hash,'transcription does not save');
 await js(`document.getElementById('copilot-text').value='😀'.repeat(4001);updateSetupCounter()`);
 assert.equal(await js(`document.getElementById('copilot-text').checkValidity()`),false);
 await js(`document.getElementById('copilot-text').value='Создай детскую комнату. Там добавь телевизор только с включением и выключением, также добавь свет только с включением и выключением.';updateSetupCounter()`);
 await click('copilot-propose');assert.equal(await js('draft.devices.length'),2);
 assert.equal(await js('draft.devices.every(d=>d.action_ids.length===2&&d.state.length===1)'),true);
 assert.equal((await config()).hash,before.hash,'proposal does not save');
 await js(`document.getElementById('copilot-text').value='Свет назови основным. Ещё добавь ночник только с включением и выключением.';updateSetupCounter()`);
 await click('copilot-propose');assert.equal(await js('draft.devices.length'),3);
 assert.ok(await js(`document.getElementById('setup-plan').textContent.includes('Основной свет')`));
 // Reconnect recovers the SAME server draft, not an independent form snapshot.
 await js('dirty=false');await send('Page.reload');await until(()=>js(`typeof setupActive!=='undefined'&&setupActive&&!operation&&draft.devices.length===3`),'reconnect');
 await click('setup-apply');assert.equal(await js('setupActive'),false);
 assert.notEqual((await config()).hash,before.hash);
 await click('setup-start');await click('copilot-mic');await until(()=>js(`voiceState==='recording'`),'second capture');
 await js('window.cancelledTracks=voiceRec.stream.getTracks()');
 await click('setup-apply');assert.equal(await js('setupActive'),true,'cannot restore domestic capture over browser recording');
 await click('setup-cancel');await until(()=>js('!setupActive&&!voiceRec'), 'cancel recording and setup');
 assert.equal(await js(`cancelledTracks.every(t=>t.readyState==='ended')`),true);
 assert.deepEqual(exceptions,[]);
 console.log('PASS setup UI: page read-only, one shared draft, browser 60s WAV cap, editable STT, no autosave, reconnect, explicit Apply, hardware admission blocked');
})().catch(e=>{console.error(e);console.error(fs.readFileSync(path.join(tmp,'server.log'),'utf8').slice(-3000));process.exitCode=1;}).finally(async()=>{
 if(socket)socket.close();
 for(const p of [chrome,server])if(p&&p.exitCode===null&&p.signalCode===null){const done=new Promise(r=>p.once('exit',r));p.kill(p===chrome?'SIGTERM':'SIGINT');await Promise.race([done,delay(10000)]);if(p.exitCode===null&&p.signalCode===null){console.error('Graceful shutdown failed, PID',p.pid);process.exitCode=1;p.unref();}}
 fs.closeSync(log);if(!process.exitCode)fs.rmSync(tmp,{recursive:true,force:true});
});
