'use strict';
const fs=require('node:fs'),os=require('node:os'),path=require('node:path'),http=require('node:http'),{spawn}=require('node:child_process'),assert=require('node:assert/strict');
const [app,root,chromium]=process.argv.slice(2),tmp=fs.mkdtempSync(path.join(os.tmpdir(),'firefly-connect-ui-'));
fs.mkdirSync(path.join(tmp,'store'),{mode:0o700});
const log=fs.openSync(path.join(tmp,'server.log'),'w');
let output='',chrome,socket,id=0,on=false;const pending=new Map(),exceptions=[],requests=[];
const mock=http.createServer((req,res)=>{let body='';req.on('data',b=>body+=b);req.on('end',()=>{
 requests.push([req.method,req.url,body]);if(req.method==='POST')on=JSON.parse(body).on;
 const data=JSON.stringify({on});res.writeHead(200,{'Content-Length':Buffer.byteLength(data),'Content-Type':'application/json'});res.end(data);
});});
const server=spawn(app,[path.join(tmp,'store'),path.join(root,'config/empty-home.json')],{stdio:['ignore','pipe',log]});server.stdout.on('data',b=>output+=b);
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function until(fn,label){for(let i=0;i<300;i++){if(await fn())return;await delay(100);}throw Error('Timeout '+label);}
function send(method,params={}){return new Promise((resolve,reject)=>{const current=++id;pending.set(current,{resolve,reject});socket.send(JSON.stringify({id:current,method,params}));});}
async function js(expression){const r=await send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;}
async function idle(){await until(()=>js('!operation'),'idle');}
async function click(id){await idle();await js(`document.getElementById(${JSON.stringify(id)}).click()`);await idle();}
async function button(scope,text,index=0){await idle();await js(`(()=>{const b=[...document.querySelectorAll(${JSON.stringify(scope)}+' button')].filter(b=>b.textContent===${JSON.stringify(text)})[${index}];if(!b)throw Error('button missing');b.click();})()`);await idle();}
async function fill(scope,label,value,index=0){await js(`(()=>{const e=[...document.querySelectorAll(${JSON.stringify(scope)}+' [aria-label]')].filter(e=>e.getAttribute('aria-label')===${JSON.stringify(label)})[${index}];if(!e)throw Error('field missing: '+${JSON.stringify(label)});e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}));})()`);await idle();}
(async()=>{
 await new Promise(r=>mock.listen(0,'127.0.0.1',r));
 await until(()=>output.includes('DEMO_LISTEN'),'listener');const url=output.match(/DEMO_LISTEN (http:\/\/[^\s]+)/)[1];
 async function api(route,body,method){const response=await fetch(url+route,{method:method||(body?'POST':'GET'),headers:{'Content-Type':'application/json','X-Firefly-Admin':'1'},body:body?JSON.stringify(body):undefined});const result=await response.json();assert.equal(response.status,200,JSON.stringify(result));return result;}
 chrome=spawn(chromium,['--ozone-platform=x11','--no-sandbox','--disable-gpu','--disable-dev-shm-usage','--no-first-run','--remote-debugging-port=0','--user-data-dir='+path.join(tmp,'chrome'),'about:blank'],{stdio:['ignore',log,log]});
 const portFile=path.join(tmp,'chrome','DevToolsActivePort');await until(()=>fs.existsSync(portFile),'Chrome');
 const port=fs.readFileSync(portFile,'utf8').split('\n')[0],pages=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();
 socket=new WebSocket(pages.find(p=>p.type==='page').webSocketDebuggerUrl);await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
 socket.onmessage=e=>{const r=JSON.parse(e.data);if(r.method==='Runtime.exceptionThrown')exceptions.push(r.params);if(r.id){const p=pending.get(r.id);pending.delete(r.id);r.error?p.reject(r.error):p.resolve(r.result);}};
 await send('Page.enable');await send('Runtime.enable');await send('Page.navigate',{url});
 await until(()=>js(`typeof draft!=='undefined'&&draft&&!operation`),'UI');
 // Actual existing setup UI creates the empty-home semantic entities.
 await click('setup-start');
 await js(`document.getElementById('copilot-text').value='Создай детскую комнату. Там добавь телевизор только с включением и выключением, также добавь свет только с включением и выключением.';updateSetupCounter()`);
 await click('copilot-propose');await click('setup-apply');
 assert.equal(await js('setupActive'),false);
 const before=await api('/api/config');
 await js(`(()=>{const card=[...document.querySelectorAll('#devices .card')].find(c=>c.querySelector('h3')?.textContent==='Свет');[...card.querySelectorAll('button')].find(b=>b.textContent==='Настроить подключение').click();})()`);await idle();
 const scope='#connection-content';
 await button(scope,'По сети');await button(scope,'Создать подключение');
 await fill(scope,'Название подключения','ESP32 Test');
 await fill(scope,'Адрес / base URL',`http://127.0.0.1:${mock.address().port}`);
 await button(scope,'Проверить соединение');assert.equal(requests.length,0);
 assert.ok(await js(`document.querySelector('#connection-content [role=status]').textContent.includes('TCP')`));
 // Configure ON and OFF through visible controls (fixed body available in Simple mode).
 for(let index=0;index<2;index++) {
  await button(scope,'Настроить',index);
  await fill(scope,'Путь','/relay/1');
  const power=await js(`(()=>{const b=draft.bindings[draft.bindings.length-1],a=draft.actions.find(a=>a.id===b.action_id);return a.assignments[0].value;})()`);
  await fill(scope,'Тело запроса · JSON',JSON.stringify({on:power}));
 }
 await button(scope,'Настроить Observe');
 await fill(scope,'Путь','/relay/1',1);
 const observeLabel=await js(`Array.from(document.querySelectorAll('#connection-content [aria-label]')).find(e=>e.getAttribute('aria-label').startsWith('JSON-поле ответа')).getAttribute('aria-label')`);
 await fill(scope,observeLabel,'on');
 await click('connection-preview');assert.equal((await api('/api/config')).hash,before.hash);assert.equal(requests.length,0);
 const candidate=await js('JSON.parse(JSON.stringify(draft))');
 await click('connection-cancel');assert.equal(await js('(draft.connections||[]).length'),0);assert.equal((await api('/api/config')).hash,before.hash);
 // Reopen a reviewed draft, prove Apply is gated on matching Preview.
 await js(`draft=${JSON.stringify(candidate)};modified();render();`);
 await js(`(()=>{const card=[...document.querySelectorAll('#devices .card')].find(c=>c.querySelector('h3')?.textContent==='Свет');[...card.querySelectorAll('button')].find(b=>b.textContent==='Настроить подключение').click();})()`);await idle();
 await click('connection-apply');assert.equal((await api('/api/config')).hash,before.hash);
 await click('connection-preview');await click('connection-apply');
 const applied=await api('/api/config');assert.notEqual(applied.hash,before.hash);assert.equal(requests.length,0);
 const target=applied.configuration.bindings[0].target_id;
 const definition=applied.configuration.devices.find(d=>d.id===target);
 for(const enabled of [true,false]) {
  const result=await api('/api/test-command',{text:`${enabled?'Включи':'Выключи'} ${definition.name.toLowerCase()} в детской`,execute:true,production:true});
  assert.equal(result.execution?.success,true,JSON.stringify(result));assert.equal(result.safety,'PASS');assert.equal(result.language_route,'INDEX_EXACT');
  assert.equal(JSON.parse(requests.at(-1)[2]).on,enabled);
  await js('status()');await button('#device-state','Прочитать состояние');
  const state=(await api('/api/devices/state')).devices.find(d=>d.targetId===target).properties;
  assert.equal(Object.values(state)[0].knowledge,'confirmed');assert.equal(Object.values(state)[0].value,enabled);
 }
 assert.equal(requests.length,4);assert.deepEqual(exceptions,[]);
 console.log('PASS Chromium connectivity: empty home via setup, new HTTP connection, safe probe, ON/OFF body forms, Observe, Preview/Cancel/Apply, real runtime HTTP and Confirmed state');
})().catch(e=>{console.error(e);console.error(fs.readFileSync(path.join(tmp,'server.log'),'utf8').slice(-3000));process.exitCode=1;}).finally(async()=>{
 if(socket)socket.close();mock.closeAllConnections();await new Promise(r=>mock.close(r));
 for(const p of [chrome,server])if(p&&p.exitCode===null&&p.signalCode===null){const done=new Promise(r=>p.once('exit',r));p.kill(p===chrome?'SIGTERM':'SIGINT');await Promise.race([done,delay(10000)]);if(p.exitCode===null&&p.signalCode===null){p.kill('SIGKILL');process.exitCode=1;}}
 fs.closeSync(log);if(!process.exitCode)fs.rmSync(tmp,{recursive:true,force:true});
});
