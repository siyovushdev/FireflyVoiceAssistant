// Optional real-browser form test. Node 22 built-ins + installed Chromium; no npm.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const {spawn} = require('node:child_process');
const assert = require('node:assert/strict');
const [app, worker, root, chromium, artifact] = process.argv.slice(2);
const directory = fs.mkdtempSync(path.join(os.tmpdir(),'firefly-ui-'));
fs.mkdirSync(path.join(directory,'store'),{mode:0o700});
const log = fs.openSync(path.join(directory,'process.log'),'w');
const admin = spawn(app,['--store',path.join(directory,'store'),'--init-config',path.join(root,'config/current-m4.json'),'--runtime-worker',worker,'--admin-port','0'],{stdio:['ignore','pipe',log]});
let chrome, socket, nextId=0;
const pending = new Map(), exceptions=[];
const delay = ms => new Promise(r=>setTimeout(r,ms));
async function until(fn,label,timeout=15000){const deadline=Date.now()+timeout;while(Date.now()<deadline){if(await fn())return;await delay(50);}throw new Error('Timeout: '+label);}
function send(method,params={}){const id=++nextId;return new Promise((resolve,reject)=>{pending.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}));});}
async function evaluate(expression){const reply=await send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(reply.exceptionDetails)throw new Error(JSON.stringify(reply.exceptionDetails));return reply.result.value;}
async function click(id){await evaluate(`document.getElementById(${JSON.stringify(id)}).click()`);}
async function fill(section,label,value){await evaluate(`(()=>{const el=Array.from(document.querySelectorAll('#${section} .card:last-child input,#${section} .card:last-child textarea')).find(e=>e.getAttribute('aria-label')===${JSON.stringify(label)});if(!el)throw Error('missing field');el.value=${JSON.stringify(value)};el.dispatchEvent(new Event('input',{bubbles:true}));})()`);}
(async()=>{
    let output='';admin.stdout.on('data',data=>output+=data);
    await until(()=>output.includes('ADMIN_LISTEN'),'admin listener');
    const url=output.match(/ADMIN_LISTEN (http:\/\/[^\s]+)/)[1];
    chrome=spawn(chromium,['--window-size=1280,900','--ozone-platform=x11','--no-sandbox','--disable-gpu','--disable-dev-shm-usage','--no-first-run','--remote-debugging-port=0','--user-data-dir='+path.join(directory,'chrome'),'about:blank'],{stdio:['ignore',log,log]});
    const active=path.join(directory,'chrome','DevToolsActivePort');await until(()=>fs.existsSync(active),'browser listener');
    const port=fs.readFileSync(active,'utf8').split('\n')[0];
    const targets=await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    socket=new WebSocket(targets.find(t=>t.type==='page').webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
    socket.onmessage=message=>{const packet=JSON.parse(message.data);if(packet.method==='Runtime.exceptionThrown')exceptions.push(packet.params);if(packet.id){const request=pending.get(packet.id);pending.delete(packet.id);packet.error?request.reject(packet.error):request.resolve(packet.result);}};
    await send('Runtime.enable');await send('Page.enable');await send('Page.navigate',{url});
    await until(()=>evaluate(`document.querySelectorAll('#rooms .card').length===4 && !document.getElementById('add-room').disabled`),'initial form');
    assert.equal(await evaluate(`document.querySelectorAll('#devices .card').length`),4);
    await click('add-room');
    await fill('rooms','Room ID','room_bathroom');await fill('rooms','Название комнаты','Ванная');
    await fill('rooms','Aliases / mentions (по одному в строке)','ванная\nванна\nванной\nванне');
    await fill('rooms','Locations (по одной фразе в строке)','в ванной\nв ванне');
    await click('add-device');await fill('devices','Target ID','device_bathroom_light');await fill('devices','Название устройства','Свет в ванной');
    await click('validate');await until(()=>evaluate(`document.getElementById('validation').textContent.includes('verified')`),'Validate form');
    assert.equal(await evaluate(`document.querySelectorAll('#errors li').length`),0);
    await click('save');await until(()=>evaluate(`document.getElementById('revision').textContent.includes('revision 2') && !document.getElementById('apply').disabled`),'Save form');
    await click('apply');await until(()=>evaluate(`document.getElementById('validation').textContent.includes('Apply завершён') && !document.getElementById('test').disabled`),'Apply form');
    for(const [text,brightness] of [['В ванной поставь свет на 40 процентов',40],['В ванне поставь свет на 30 процентов',30]]){
        await until(()=>evaluate(`!document.getElementById('test').disabled`),'command idle');
        await evaluate(`document.getElementById('command').value=${JSON.stringify(text)};document.getElementById('test-form').requestSubmit()`);
        await until(()=>evaluate(`(()=>{try{return JSON.parse(document.getElementById('test-result').textContent).input===${JSON.stringify(text)};}catch{return false;}})()`),'Test Command form');
        const result=await evaluate(`JSON.parse(document.getElementById('test-result').textContent)`);
        assert.deepEqual(result.result,{actionId:'action_set_brightness',targetId:'device_bathroom_light',arguments:{brightness}});
        assert.equal(result.route,'deterministic');assert.equal(result.revision,2);
    }
    await until(()=>evaluate(`!document.getElementById('test').disabled`),'command idle');
    await evaluate(`document.getElementById('command').value='я'.repeat(100);document.getElementById('test-form').requestSubmit()`);
    await until(()=>evaluate(`(()=>{try{return JSON.parse(document.getElementById('test-result').textContent).route==='input_rejected';}catch{return false;}})()`),'invalid input shown');
    assert.deepEqual(await evaluate(`JSON.parse(document.getElementById('test-result').textContent).result`),{unsupported:true},'failed command does not replay prior action');
    await until(()=>evaluate(`!document.getElementById('test').disabled`),'command idle');
    await evaluate(`document.getElementById('command').value='В ванной поставь свет на 40 процентов';document.getElementById('test-form').requestSubmit()`);
    await until(()=>evaluate(`(()=>{try{return JSON.parse(document.getElementById('test-result').textContent).result.arguments.brightness===40;}catch{return false;}})()`),'valid command after rejection');
    await until(()=>evaluate(`!document.getElementById('test').disabled`),'command idle');
    await evaluate(`document.getElementById('advanced').open=true`);await click('show-active');
    await until(()=>evaluate(`document.getElementById('active-context').textContent.includes('device_bathroom_light')`),'active generated context');
    assert.deepEqual(exceptions,[],'uncaught browser exceptions');
    if(artifact){fs.mkdirSync(path.dirname(artifact),{recursive:true});const shot=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});fs.writeFileSync(artifact,Buffer.from(shot.data,'base64'));}
    console.log('PASS: Chromium forms → Bathroom + light → Validate → Save → Apply → two typed commands → updated LLM context; no JS exceptions');
})().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
    if(socket)socket.close();if(chrome && chrome.exitCode===null && chrome.signalCode===null){const exited=new Promise(r=>chrome.once('exit',r));chrome.kill('SIGTERM');await exited;}
    if(admin.exitCode===null && admin.signalCode===null){const exited=new Promise(r=>admin.once('exit',r));admin.kill('SIGTERM');await exited;}fs.closeSync(log);
    if(process.exitCode)console.error(fs.readFileSync(path.join(directory,'process.log'),'utf8'));
    fs.rmSync(directory,{recursive:true,force:true});
});
