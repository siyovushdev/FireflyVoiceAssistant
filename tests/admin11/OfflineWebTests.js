// Manual configuration remains available without a model. Real HTTP and Chromium.
'use strict';
const fs=require('node:fs'),os=require('node:os'),path=require('node:path'),{spawn}=require('node:child_process'),assert=require('node:assert/strict');
const [app,root,chromium,artifacts]=process.argv.slice(2);fs.mkdirSync(artifacts,{recursive:true});const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'firefly-offline-admin-'));
fs.mkdirSync(path.join(tmp,'store'),{mode:0o700});const log=fs.openSync(path.join(artifacts,'offline-server.log'),'w');
const processAdmin=spawn(app,['--store',path.join(tmp,'store'),'--init-config',path.join(root,'config/current-m4.json'),'--model',path.join(tmp,'absent.rkllm'),'--admin-port','0'],{stdio:['ignore','pipe',log]});
let output='',chrome,socket,id=0;processAdmin.stdout.on('data',b=>output+=b);const pending=new Map();
const delay=ms=>new Promise(r=>setTimeout(r,ms));async function until(fn,label){for(let i=0;i<300;i++){if(await fn())return;await delay(100);}throw Error(label);}
function send(method,params={}){return new Promise((resolve,reject)=>{const current=++id;pending.set(current,{resolve,reject});socket.send(JSON.stringify({id:current,method,params}));});}
async function js(expression){const r=await send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;}
async function click(id){await until(()=>js('!operation'),'idle');await js(`document.getElementById(${JSON.stringify(id)}).click()`);await until(()=>js('!operation'),'idle');}
(async()=>{
    await until(()=>output.includes('DEMO_LISTEN'),'listener');const url=output.match(/DEMO_LISTEN (http:\/\/[^\s]+)/)[1];const status=await(await fetch(url+'/api/status')).json();assert.equal(status.runtime_ready,false);
    chrome=spawn(chromium,['--ozone-platform=x11','--no-sandbox','--disable-gpu','--no-first-run','--remote-debugging-port=0','--user-data-dir='+path.join(tmp,'chrome'),'about:blank'],{stdio:['ignore',log,log]});
    const portFile=path.join(tmp,'chrome','DevToolsActivePort');await until(()=>fs.existsSync(portFile),'Chrome');const port=fs.readFileSync(portFile,'utf8').split('\n')[0];
    const pages=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();socket=new WebSocket(pages.find(p=>p.type==='page').webSocketDebuggerUrl);await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
    socket.onmessage=e=>{const r=JSON.parse(e.data);if(r.method==='Page.javascriptDialogOpening')send('Page.handleJavaScriptDialog',{accept:true});if(r.id){const p=pending.get(r.id);pending.delete(r.id);r.error?p.reject(r.error):p.resolve(r.result);}};
    await send('Page.enable');await send('Runtime.enable');await send('Page.navigate',{url});await until(()=>js(`typeof draft!=='undefined'&&draft&&!operation`),'UI');
    await click('add-location');await js(`const n=document.getElementById('new-name');n.value='Цех';n.dispatchEvent(new Event('input'));Array.from(document.querySelectorAll('#wizard button')).find(b=>b.textContent==='Предложить с локальным ИИ').click()`);await until(()=>js('!operation'),'AI error');assert.ok(await js(`document.getElementById('errors').innerText.includes('ИИ недоступен')`));await click('wizard-close');
    const oldId=await js('draft.rooms[0].id');await js(`const e=document.querySelector('#rooms .card input[aria-label="Название зоны"]');e.value='Зона А';e.dispatchEvent(new Event('input'));`);
    await click('validate');assert.ok(await js('validated'));await click('save');assert.equal(await js('expected.revision'),2);assert.equal(await js('draft.rooms[0].id'),oldId);
    await click('apply');assert.equal(await js(`document.getElementById('diff-dialog').open`),true);assert.ok(await js(`document.getElementById('diff-list').children.length>=11`));await click('confirm-apply');assert.ok(await js(`document.getElementById('errors').children.length>0`));await click('diff-close');
    await js(`document.getElementById('playground').scrollIntoView()`);const sidebar=await js(`({top:document.querySelector('aside').getBoundingClientRect().top,position:getComputedStyle(document.querySelector('aside')).position,scroll:scrollY})`);
    const screenshot=await send('Page.captureScreenshot',{format:'png',fromSurface:false});fs.writeFileSync(path.join(artifacts,'offline-manual.png'),Buffer.from(screenshot.data,'base64'));
    fs.writeFileSync(path.join(artifacts,'offline-evidence.json'),JSON.stringify({status,stored:await(await fetch(url+'/api/config')).json(),sidebar},null,2));
    console.log('PASS offline Chromium: AI error, manual Validate/Save, stable rename, initial Apply diff, fail-closed model admission',sidebar);
})().catch(e=>{console.error(e);process.exitCode=1;}).finally(async()=>{
    if(socket)socket.close();for(const p of [chrome,processAdmin])if(p&&p.exitCode===null&&p.signalCode===null){const done=new Promise(r=>p.once('exit',r));p.kill(p===chrome?'SIGTERM':'SIGINT');await done;}
    fs.closeSync(log);fs.rmSync(tmp,{recursive:true,force:true});
});
