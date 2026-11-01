'use strict';
// Own disposable Chrome profile + real C++ HTTP server. No mocked HTTP replies.
const fs=require('node:fs'),path=require('node:path'),os=require('node:os');
const {spawn}=require('node:child_process');const assert=require('node:assert/strict');
const [app,root,chromePath,out]=process.argv.slice(2);fs.mkdirSync(out,{mode:0o700});
const tmp=fs.mkdtempSync('/tmp/firefly-operational-browser-');fs.chmodSync(tmp,0o700);
const cfg=JSON.parse(fs.readFileSync(path.join(root,'tests/admin/v2/system.json')));
fs.writeFileSync(path.join(tmp,'initial.json'),JSON.stringify(cfg),{mode:0o600});fs.mkdirSync(path.join(tmp,'store'),{mode:0o700});
const log=fs.openSync(path.join(out,'server.log'),'w'),chromeLog=fs.openSync(path.join(out,'chrome.log'),'w');
let appProcess,chrome,ws,url,number=0;const pending=new Map(),requests=[],errors=[],external=[];
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
async function until(test,label){const end=Date.now()+20000;while(Date.now()<end){if(await test())return;await sleep(75);}throw Error('Timeout: '+label);}
function send(method,params={}){return new Promise((resolve,reject)=>{const id=++number;pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}));});}
async function js(expression){const r=await send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;}
async function click(label,scope='dialog[open]'){await js(`(() => {const root=document.querySelector(${JSON.stringify(scope)});const b=[...root.querySelectorAll('button')].find(b=>b.textContent.trim()===${JSON.stringify(label)});if(!b||b.disabled)throw Error('No active button '+${JSON.stringify(label)});b.click();})()`);}
async function api(endpoint,body,method){const r=await fetch(url+endpoint,{method:method||(body?'POST':'GET'),headers:{'Content-Type':'application/json','X-Firefly-Admin':'1'},body:body?JSON.stringify(body):undefined});const j=await r.json();assert.ok(r.ok,JSON.stringify(j));return j;}
const identity=c=>({revision:c.revision,hash:c.hash});
async function task(){await js(`document.querySelector('[data-testid="voice-settings"]').click()`);await until(()=>js(`!!document.querySelector('dialog[open] [data-testid="voice-responses"]')`),'voice task');}
async function prefs(mode,questions){await js(`(() => {const s=document.querySelector('[data-testid="voice-responses"]');s.value=${JSON.stringify(mode)};s.dispatchEvent(new Event('change',{bubbles:true}));const q=document.querySelector('[data-testid="voice-questions"]');if(q.checked!==${questions})q.click();})()`);}
async function stage(){await click('Добавить в план');await until(()=>js(`!document.querySelector('dialog[open]')`),'staged');}
async function review(){await click('Сохранить и применить','main .manual-toolbar');await until(()=>js(`[...document.querySelectorAll('dialog[open] button')].some(b=>b.textContent.trim()==='Подтвердить применение'&&!b.disabled)`),'review');}
async function close(){if(ws)try{ws.close();}catch{}if(chrome){chrome.kill('SIGTERM');await new Promise(r=>chrome.once('exit',r));}if(appProcess){appProcess.stdin.end();await Promise.race([new Promise(r=>appProcess.once('exit',r)),sleep(5000).then(()=>appProcess.kill('SIGKILL'))]);}fs.closeSync(log);fs.closeSync(chromeLog);fs.rmSync(tmp,{recursive:true,force:true});}
(async()=>{try{
 appProcess=spawn(app,[path.join(tmp,'store'),path.join(tmp,'initial.json')],{stdio:['pipe','pipe',log]});let output='';appProcess.stdout.on('data',b=>output+=b);
 await until(()=>output.includes('\n'),'fixture');url=JSON.parse(output.split('\n')[0]).url;
 const before=await api('/api/config'),originalPid=(await api('/api/status')).worker_pid;
 chrome=spawn(chromePath,['--headless=new','--no-sandbox','--disable-gpu','--disable-background-networking','--no-first-run','--no-default-browser-check','--remote-debugging-port=0','--user-data-dir='+path.join(out,'profile'),'about:blank'],{stdio:['ignore',chromeLog,chromeLog]});
 const portFile=path.join(out,'profile/DevToolsActivePort');await until(()=>fs.existsSync(portFile),'Chrome');
 const port=fs.readFileSync(portFile,'utf8').split('\n')[0];const pages=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();
 ws=new WebSocket(pages.find(p=>p.type==='page').webSocketDebuggerUrl);await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
 ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id){const p=pending.get(m.id);pending.delete(m.id);if(p)m.error?p.reject(m.error):p.resolve(m.result);}else if(m.method==='Runtime.exceptionThrown')errors.push(m.params);else if(m.method==='Fetch.requestPaused'){const {request,requestId}=m.params;requests.push(request);if(request.url.startsWith(url+'/'))send('Fetch.continueRequest',{requestId});else{external.push(request.url);send('Fetch.failRequest',{requestId,errorReason:'BlockedByClient'});}}else if(m.method==='Page.javascriptDialogOpening')send('Page.handleJavaScriptDialog',{accept:true});};
 await send('Runtime.enable');await send('Page.enable');await send('Fetch.enable',{patterns:[{urlPattern:'*'}]});
 await send('Page.navigate',{url:url+'/#/overview'});await until(()=>js(`!!document.querySelector('[data-testid="voice-settings"]')`),'overview');
 await task();await prefs('brief',true);await click('Отмена');await until(()=>js(`!document.querySelector('dialog[open]')`),'cancel');assert.equal((await api('/api/config')).hash,before.hash);
 await task();await prefs('detailed',true);await stage();assert.equal((await api('/api/config')).hash,before.hash);
 await review();const reviewText=await js(`document.querySelector('dialog[open]').innerText`);assert.match(reviewText,/Голосовые ответы и вопросы/);assert.match(reviewText,/подробно/);assert.match(reviewText,/вопросы о работе: включены/);
 await click('Подтвердить применение');await until(async()=>{const s=await api('/api/config/active');return s.active?.configuration.system.voice_dialogue?.responses==='detailed';},'applied');
 await until(()=>js(`[...document.querySelectorAll('dialog[open] button')].some(b=>b.textContent.trim()==='Назад к плану'&&!b.disabled)`),'after apply');await click('Назад к плану');
 assert.equal(requests.filter(r=>r.method==='PUT'&&new URL(r.url).pathname==='/api/config').length,1);
 assert.equal(requests.filter(r=>r.method==='POST'&&new URL(r.url).pathname==='/api/config/apply').length,1);
 assert.equal((await api('/api/status')).worker_pid,originalPid);
 await js('window.__fireflyTestOldDocument=true');await send('Page.reload');await until(()=>js(`!window.__fireflyTestOldDocument&&document.readyState==='complete'&&!!document.querySelector('[data-testid="voice-settings"]')`),'reload');await task();assert.equal(await js(`document.querySelector('[data-testid="voice-responses"]').value`),'detailed');assert.equal(await js(`document.querySelector('[data-testid="voice-questions"]').checked`),true);
 await prefs('off',false);await stage();await review();
 const current=await api('/api/config'),other=structuredClone(current.configuration);other.system.name='Другая вкладка';await api('/api/config',{configuration:other,expected:identity(current)},'PUT');
 await click('Подтвердить применение');await until(()=>js(`document.querySelector('dialog[open]')?.innerText.includes('измен')||document.querySelector('dialog[open]')?.innerText.includes('верси')`),'CAS rejection');await sleep(300);
 assert.equal(requests.filter(r=>r.method==='PUT'&&new URL(r.url).pathname==='/api/config').length,1,'stale review must not Save');assert.equal((await api('/api/config/active')).active.configuration.system.voice_dialogue.responses,'detailed');assert.equal((await api('/api/config')).configuration.system.name,'Другая вкладка');
 await send('Page.navigate',{url:url+'/#/diagnostics'});await until(()=>js(`[...document.querySelectorAll('main button')].some(b=>b.textContent.trim()==='Остановить речь')`),'stop control');await click('Остановить речь','main');await until(()=>js(`document.body.innerText.includes('Запрошена остановка речи')`),'stop reply');
 assert.equal(requests.filter(r=>r.method==='POST'&&new URL(r.url).pathname==='/api/voice/stop').length,1);
 assert.equal((await api('/api/executions')).executions.length,0);assert.equal(external.length,0);assert.equal(errors.length,0,JSON.stringify(errors));
 const shot=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});fs.writeFileSync(path.join(out,'diagnostics.png'),Buffer.from(shot.data,'base64'));
 fs.writeFileSync(path.join(out,'report.json'),JSON.stringify({status:'PASS',samePid:originalPid,checks:['Cancel unchanged','staged unchanged','readable Review','one Save→Apply','hot same-PID activation','reload persistence','CAS no retry','speech stop control','zero execution'],requests,errors,external},null,2));console.log('PASS Chromium: real C++ HTTP, settings Cancel/CAS/one Save→Apply, persisted reload, same PID, audio stop, zero execution');
}finally{await close();}})().catch(e=>{console.error(e);process.exitCode=1;});
