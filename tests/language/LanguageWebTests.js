'use strict';
const fs=require('node:fs'),os=require('node:os'),path=require('node:path'),{spawn}=require('node:child_process'),assert=require('node:assert/strict');
const [app,worker,root,chromium]=process.argv.slice(2),tmp=fs.mkdtempSync(path.join(os.tmpdir(),'firefly-language-ui-'));
fs.mkdirSync(path.join(tmp,'store'),{mode:0o700});
const log=fs.openSync(path.join(tmp,'server.log'),'w');
const admin=spawn(app,['--store',path.join(tmp,'store'),'--init-config',path.join(root,'config/empty-home.json'),'--runtime-worker',worker,'--admin-port','0'],{stdio:['ignore','pipe',log]});
let output='',chrome,socket,id=0;admin.stdout.on('data',b=>output+=b);const pending=new Map(),exceptions=[];
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function until(fn,label){for(let i=0;i<300;i++){if(await fn())return;await delay(100);}throw Error('Timeout '+label);}
function send(method,params={}){return new Promise((resolve,reject)=>{const current=++id;pending.set(current,{resolve,reject});socket.send(JSON.stringify({id:current,method,params}));});}
async function js(expression){const r=await send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;}
async function idle(){await until(()=>js('!operation'),'idle');}
async function click(id){await idle();await js(`document.getElementById(${JSON.stringify(id)}).click()`);await idle();}
(async()=>{
    await until(()=>output.includes('ADMIN_LISTEN')||output.includes('DEMO_LISTEN'),'listener');
    const url=output.match(/(?:ADMIN_LISTEN|DEMO_LISTEN) (http:\/\/[^\s]+)/)[1];
    chrome=spawn(chromium,['--ozone-platform=x11','--no-sandbox','--disable-gpu','--disable-dev-shm-usage','--no-first-run','--remote-debugging-port=0','--user-data-dir='+path.join(tmp,'chrome'),'about:blank'],{stdio:['ignore',log,log]});
    const portFile=path.join(tmp,'chrome','DevToolsActivePort');await until(()=>fs.existsSync(portFile),'Chrome');
    const port=fs.readFileSync(portFile,'utf8').split('\n')[0],pages=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    socket=new WebSocket(pages.find(p=>p.type==='page').webSocketDebuggerUrl);await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
    socket.onmessage=e=>{const r=JSON.parse(e.data);if(r.method==='Runtime.exceptionThrown')exceptions.push(r.params);if(r.id){const p=pending.get(r.id);pending.delete(r.id);r.error?p.reject(r.error):p.resolve(r.result);}};
    await send('Page.enable');await send('Runtime.enable');await send('Page.navigate',{url});
    await until(()=>js(`typeof draft!=='undefined'&&draft&&!operation`),'UI');
    assert.equal(await js('draft.compatibility'),null);assert.equal(await js('draft.devices.length'),0);
    const home=JSON.parse(fs.readFileSync(path.join(root,'tests/language/home.json')));
    await js(`draft=${JSON.stringify(home)};modified();render()`);
    assert.equal(await js(`document.querySelectorAll('#actions .card').length`),3);
    assert.ok(await js(`document.getElementById('actions').textContent.includes('Язык команд')`));
    const hash=(await(await fetch(url+'/api/config')).json()).hash;
    await js(`Array.from(document.querySelectorAll('#actions .card')).find(e=>e.querySelector('input[aria-label=\"Постоянный идентификатор\"]').value==='power_on').querySelectorAll('button').forEach(b=>{if(b.textContent==='Предложить варианты')b.click()})`);
    await idle();
    assert.ok(await js(`document.getElementById('actions').textContent.includes('Предложение ИИ')`));
    assert.equal((await(await fetch(url+'/api/config')).json()).hash,hash);
    await js(`Array.from(document.querySelectorAll('#actions button')).find(e=>e.textContent==='Проверить и добавить в черновик').click()`);await idle();
    await click('validate');assert.equal(await js('validated'),true);
    assert.ok(await js(`document.getElementById('validation').textContent.includes('Примеры:')`));
    await click('save');await click('apply');await click('confirm-apply');
    assert.equal(await js(`document.getElementById('errors').children.length`),0);
    const response=await fetch(url+'/api/test-command',{method:'POST',headers:{'Content-Type':'application/json','X-Firefly-Admin':'1'},body:JSON.stringify({text:'включи свет на кухне',execute:true})});
    const result=await response.json();assert.equal(result.language_route,'INDEX_EXACT');assert.equal(result.safety,'PASS');assert.equal(result.execution.success,true);
    assert.deepEqual(exceptions,[]);
    console.log('PASS Chromium empty-home, language proposal/edit/confirm, no autosave, Validate/Save/Apply, index execution');
})().catch(e=>{console.error(e);console.error(fs.readFileSync(path.join(tmp,'server.log'),'utf8').slice(-3000));process.exitCode=1;}).finally(async()=>{
    if(socket)socket.close();
    for(const p of [chrome,admin])if(p&&p.exitCode===null&&p.signalCode===null){const done=new Promise(r=>p.once('exit',r));p.kill(p===chrome?'SIGTERM':'SIGINT');await Promise.race([done,delay(5000)]);if(p.exitCode===null&&p.signalCode===null){p.kill('SIGKILL');await done;}}
    fs.closeSync(log);fs.rmSync(tmp,{recursive:true,force:true});
});
