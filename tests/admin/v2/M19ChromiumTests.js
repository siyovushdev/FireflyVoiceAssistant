'use strict';
// Browser happy path uses only visible controls. Test-only PCM/KWS/VAD/STT and
// scripted semantic model; real C++ store, API, wake loop, safety and HTTP transport.
const fs = require('node:fs'), os = require('node:os'), path = require('node:path');
const http = require('node:http'), crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const assert = require('node:assert/strict');
const [app, worker, root, chromium, artifacts] = process.argv.slice(2);
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'firefly-m19-'));
fs.mkdirSync(artifacts, { recursive: true });
fs.rmSync(path.join(artifacts, 'results.json'), {force:true});
const log = fs.openSync(path.join(artifacts, 'server.log'), 'w');
let server, chrome, socket, url, proof, output = '', nextId = 0, voiceId = 0, pageClosing = false;
let power = false, fault = '', tcpConnections = 0, tcpBytes = 0;
const pending = new Map(), requests = [], hardware = [], exceptions = [], external = [], results = [], diagnosticBusyReads = [], cancelledDiagnosticReads = [];
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const mock = http.createServer((req, res) => {
    let body = ''; req.on('data', bytes => body += bytes);
    req.on('end', () => {
        hardware.push({ method: req.method, path: req.url, body });
        let status = fault === 'unavailable' ? 500 : req.url === '/relay/1' ? 200 : 404;
        if (req.method === 'POST' && status === 200) {
            try { const data = JSON.parse(body); assert.deepEqual(Object.keys(data), ['on']); assert.equal(typeof data.on, 'boolean'); power = data.on; }
            catch { status = 400; }
        }
        const reply = fault === 'malformed' ? '{' : fault === 'wrong_type' ? '{"on":"true"}' : JSON.stringify({ on: power });
        res.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(reply) }); res.end(reply);
    });
});
mock.on('connection', connection => { tcpConnections++; connection.on('data', bytes => tcpBytes += bytes.length); });
async function until(check, label, timeout = 20000) {
    const end = Date.now() + timeout;
    while (Date.now() < end) { if (server && server.exitCode !== null) throw Error('C++ server exited: ' + server.exitCode); if (await check()) return; await delay(60); }
    throw Error('Timeout: ' + label);
}
function send(method, params = {}) { return new Promise((resolve, reject) => { const id = ++nextId; pending.set(id, { resolve, reject }); socket.send(JSON.stringify({ id, method, params })); }); }
async function js(expression) { const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true }); if (r.exceptionDetails) throw Error(JSON.stringify(r.exceptionDetails)); return r.result.value; }
async function text() { return js('document.body?.innerText ?? ""'); }
async function click(label, scope = '') {
    const find = `const root = document.querySelector(${JSON.stringify(scope || 'dialog[open]')}) || document.querySelector('main'); const node = [...root.querySelectorAll('button,a')].find(n => n.textContent.trim() === ${JSON.stringify(label)} && n.getClientRects().length && !n.matches(':disabled'));`;
    await until(() => js(`(() => { ${find} return !!node; })()`), 'enabled ' + label);
    await js(`(() => { ${find} node.scrollIntoView({block:'center'}); node.click(); })()`); await delay(80);
}
async function fill(label, value, scope = '') { await js(`(() => { const root = document.querySelector(${JSON.stringify(scope || 'dialog[open]')}) || document.querySelector('main'); const node = [...root.querySelectorAll('label')].find(n => n.textContent.trim().startsWith(${JSON.stringify(label)}) && n.getClientRects().length)?.querySelector('input,textarea'); if (!node) throw Error('Missing field'); node.value = ${JSON.stringify(value)}; node.dispatchEvent(new Event('input',{bubbles:true})); node.dispatchEvent(new Event('change',{bubbles:true})); })()`); }
async function select(label, option, scope = '') { await js(`(() => { const root = document.querySelector(${JSON.stringify(scope || 'dialog[open]')}) || document.querySelector('main'); const node = [...root.querySelectorAll('label')].find(n => n.textContent.trim().startsWith(${JSON.stringify(label)}))?.querySelector('select'); const value = [...node.options].find(n => n.textContent.trim() === ${JSON.stringify(option)})?.value; if (value === undefined) throw Error('Missing option'); node.value = value; node.dispatchEvent(new Event('change',{bubbles:true})); })()`); }
async function api(endpoint, body, expected = 200) {
    const deadline = Date.now() + 5000;
    while (true) {
        const r = await fetch(url + endpoint, { method: body ? 'POST' : 'GET', headers: { 'Content-Type':'application/json', 'Origin':url, 'X-Firefly-Admin':'1' }, body: body ? JSON.stringify(body) : undefined });
        const data = await r.json();
        // Read-only diagnostics may race the browser's explicit Observe/refresh.
        // Retry ONLY the documented busy GET. Never retry an Apply/command/poll.
        if (!body && expected === 200 && r.status === 409 && data.errors?.[0]?.code === 'runtime_busy' && Date.now() < deadline) {
            diagnosticBusyReads.push(endpoint); await delay(80); continue;
        }
        assert.equal(r.status, expected, JSON.stringify({ endpoint, data })); return data;
    }
}
async function screenshot(name) {
    assert.ok(await js('document.documentElement.scrollWidth <= innerWidth + 2'), 'no page horizontal overflow');
    if (await js('!!document.querySelector("dialog[open]")')) assert.ok(await js('document.querySelector("dialog[open]").scrollWidth <= document.querySelector("dialog[open]").clientWidth + 2'), 'no dialog overflow');
    const r = await send('Page.captureScreenshot', { format:'png', captureBeyondViewport:!await js('!!document.querySelector("dialog[open]")') }); fs.writeFileSync(path.join(artifacts, name + '.png'), Buffer.from(r.data, 'base64'));
}
async function navigate(fragment) { await js(`location.hash = ${JSON.stringify('#/' + fragment)}`); await delay(100); }
async function refresh() { await click('Обновить данные ↻', 'main .page-heading'); await until(() => js('!document.querySelector("button.refresh").disabled'), 'refresh done'); }
async function start(store, control, fixtureEnv = {}) {
    output = '';
    server = spawn(app, ['--store', store, '--init-config', path.join(root, 'config/empty-home.json'), '--runtime-worker', worker, '--admin-port', '0'], { stdio:['ignore','pipe',log], env:{...process.env, FIREFLY_M19_VOICE_CONTROL:control, ...fixtureEnv} });
    server.stdout.on('data', bytes => output += bytes);
    await until(() => output.includes('ADMIN_LISTEN'), 'C++ listener'); url = output.match(/ADMIN_LISTEN (http:\/\/\S+)/)[1];
    await until(async () => (await api('/api/status')).runtime_ready, 'runtime READY');
}
async function stop() {
    pageClosing = true;
    if (socket?.readyState === 1) { await send('Page.navigate', {url:'about:blank'}); await delay(60); }
    const child = server; server = null; child.kill('SIGTERM');
    await Promise.race([new Promise(resolve => child.once('exit', resolve)), delay(10000).then(() => { if (child.exitCode === null) throw Error('shutdown timeout'); })]);
    assert.equal(child.exitCode, 0);
    pageClosing = false;
}
async function voice(control, transcript, wake = true) {
    const id = ++voiceId;
    fs.writeFileSync(control + '.tmp', JSON.stringify({id,text:transcript,wake})); fs.renameSync(control + '.tmp', control);
    let result;
    await until(() => { if (!fs.existsSync(control + '.result')) return false; result = JSON.parse(fs.readFileSync(control + '.result')); return result.id === id; }, 'voice fixture consumed once');
    assert.equal(fs.existsSync(control), false, 'consumed fixture cannot replay across restart');
    assert.equal(result.processed, wake ? 1 : 0); assert.equal(result.emitted, wake ? 1 : 0);
    if (wake) { assert.ok(result.states.includes('WAKE_DETECTED')); assert.ok(result.states.includes('CONTINUATION_INLINE')); }
    return result;
}
async function ask(phrase) { await fill('Ваша просьба', phrase); await click('Отправить'); await until(() => js('document.querySelector(".setup-fields").getAttribute("aria-busy") === "false"'), 'setup message done'); }
async function proposal() { await until(() => js('!!document.querySelector(".proposal-card")'), 'proposal visible'); }
async function accepted() { await click('Принять в план'); await until(() => js('!document.querySelector(".proposal-card")'), 'accepted into plan'); }
async function rejectedOwnerProfile() {
    const store = path.join(temp, 'rejected-owner-profile'), control = path.join(temp, 'rejected-voice');
    fs.mkdirSync(store, {mode:0o700});
    const io = hardware.length;
    await start(store, control, {FIREFLY_M19_BAD_DEVICE_PROFILE:'1'});
    await send('Page.navigate', {url:url + '/#/setup'});
    await until(() => js('[...document.querySelectorAll("main button")].some(n => n.textContent === "Начать настройку" && !n.disabled)'), 'negative empty Setup');
    const original = await api('/api/config');
    await click('Начать настройку');
    await until(() => js('!!document.querySelector(".setup-fields") && !document.querySelector(".setup-fields").disabled'), 'negative setup active');
    await ask('Создай зону Кабинет и лампу в кабинете, которую можно включать и выключать'); await proposal();
    const state = await api('/api/setup/status');
    assert.deepEqual(state.configuration.devices[0].spoken_aliases, ['лампа']);
    const warning = await js('[...document.querySelectorAll("[role=alert]")].map(n => n.innerText).join(" ")');
    assert.match(warning, /языковых предложений отклонена/);
    assert.match(warning, /Проверьте речевые формы/);
    assert.match(await text(), /Речевые формы: лампа/);
    assert.equal((await api('/api/config')).hash, original.hash);
    assert.equal(hardware.length, io);
    await screenshot('owner-profile-rejected-warning');
    await click('Отклонить');
    await click('Завершить настройку без Apply');
    await until(async () => !(await api('/api/setup/status')).active, 'negative setup cancelled');
    assert.equal((await api('/api/config')).hash, original.hash);
    assert.equal(hardware.length, io);
    results.push({name:'rejected-owner-profile',aliases:state.configuration.devices[0].spoken_aliases,warning,applied:false,posts:0,gets:0});
    await stop();
}
async function scenario(noZone) {
    const name = noZone ? 'no-zone-mobile' : 'cabinet-desktop';
    const store = path.join(temp, name), control = path.join(temp, name + '-voice'); fs.mkdirSync(store, { mode:0o700 });
    const voiceEvidence = [], knowledge = [];
    const recordVoice = result => voiceEvidence.push({id:result.id,processed:result.processed,emitted:result.emitted,states:result.states,last_voice:result.runtime.last_voice,executions:result.runtime.executions});
    power = false; fault = ''; const firstHardware = hardware.length, firstRequests = requests.length;
    await start(store, control);
    await send('Emulation.setDeviceMetricsOverride', { width:noZone ? 390 : 1280, height:noZone ? 844 : 800, deviceScaleFactor:1, mobile:false });
    await send('Page.navigate', {url:url + '/#/setup'});
    await until(() => js('[...document.querySelectorAll("main button")].some(n => n.textContent === "Начать настройку" && !n.disabled)'), 'empty Setup');
    const original = await api('/api/config'); assert.equal(original.configuration.devices.length, 0); assert.equal(original.configuration.rooms.length, 0);
    await screenshot(name + '-01-empty');
    await click('Начать настройку'); await until(() => js('!!document.querySelector(".setup-fields") && !document.querySelector(".setup-fields").disabled'), 'AI setup editing');
    const token = await js('JSON.parse(sessionStorage.getItem("firefly-v2-setup-owner")).token');
    const state = await api('/api/setup/status');
    await api('/api/setup/start', {async_enter:true}, 409);
    await api('/api/setup/message', {session_id:'other-browser',version:state.version,text:'x'}, 409);
    await api('/api/setup/draft', {session_id:token,version:-1,configuration:original.configuration}, 409);
    const blockedVoice = await voice(control, 'Амина, включи лампу в кабинете');
    recordVoice(blockedVoice);
    assert.equal(blockedVoice.runtime.last_voice, null, 'setup blocks voice before STT');
    const phrase = noZone ? 'Добавь лампу без зоны, которую можно включать и выключать' : 'Создай зону Кабинет и лампу в кабинете, которую можно включать и выключать';
    await ask(phrase); await proposal();
    const semantic = (await api('/api/setup/status')).configuration;
    assert.equal(semantic.devices.length, 1); assert.equal(semantic.actions.length, 2);
    assert.deepEqual(semantic.actions.map(a => a.assignments[0].value).sort(), [false,true]);
    assert.equal(semantic.devices[0].state[0].type, 'boolean');
    assert.equal(semantic.rooms.length, noZone ? 0 : 1); assert.equal(semantic.devices[0].room_id, noZone ? null : semantic.rooms[0].id);
    assert.ok(semantic.devices[0].spoken_aliases.includes('лампу'));
    assert.match(await text(), /Речевые формы: лампа, лампу/);
    if (!noZone) assert.deepEqual(semantic.rooms[0].speech.locations, ['в кабинете']);
    await screenshot(name + '-02-proposal'); await accepted();
    assert.equal((await api('/api/config')).hash, original.hash); assert.equal(hardware.length, firstHardware);
    // Cancel saves a private draft; the same browser restores its accepted plan.
    await click('Завершить настройку без Apply'); await until(async () => !(await api('/api/setup/status')).active, 'cancel restores runtime');
    assert.equal((await api('/api/config')).hash, original.hash); assert.equal(hardware.length, firstHardware);
    await click('Восстановить серверный черновик');
    await until(() => js('!!document.querySelector(".setup-fields") && !document.querySelector(".setup-fields").disabled'), 'accepted draft resumed');
    assert.deepEqual((await api('/api/setup/status')).configuration, semantic);
    assert.equal((await api('/api/config')).hash, original.hash);
    await click('Настроить подключение: Лампа');
    await select('Общее подключение:', 'Создать отдельное подключение для выбранного устройства');
    await ask('Подключи лампу'); assert.match(await text(), /Какой адрес и протокол/);
    const endpoint = `http://127.0.0.1:${mock.address().port}`;
    await ask(endpoint); await ask('POST /relay/1 {"on":true}'); await ask('POST /relay/1 {"on":false}'); await ask('GET /relay/1, поле on'); await proposal();
    const beforeProbeBytes = tcpBytes, beforeProbeConnections = tcpConnections;
    await click('Проверить доступность'); await until(async () => (await text()).includes('TCP доступен'), 'connect-only probe');
    assert.ok(tcpConnections > beforeProbeConnections); assert.equal(tcpBytes, beforeProbeBytes); assert.equal(hardware.length, firstHardware);
    await click('Изменить: Подключение'); await until(() => js('!!document.querySelector("dialog .wizard")'), 'visible wizard');
    await click('HTTP / REST →');
    assert.equal(await js('[...document.querySelectorAll("dialog label")].find(n => n.textContent.startsWith("Endpoint")).querySelector("input").value'), endpoint);
    await screenshot(name + '-03-endpoint'); await click('Далее');
    assert.match(await js('document.querySelector("dialog").innerText'), /Включить/);
    await screenshot(name + '-04-actions'); await click('Далее');
    assert.equal(await js('[...document.querySelectorAll("dialog label")].find(n => n.textContent.startsWith("Top-level поле ответа")).querySelector("input").value'), 'on');
    await screenshot(name + '-05-observe'); await click('Далее'); await click('Сохранить и применить');
    await until(() => js('[...document.querySelectorAll("dialog button")].some(n => n.textContent === "Подтвердить применение" && !n.disabled)'), 'one final review');
    const reviewText = await js('document.querySelector("dialog").innerText');
    for (const pattern of [/Лампа/, /Обращения: лампа; лампу/, /POST http:\/\/127\.0\.0\.1:/, /on = Да/, /on = Нет/, /GET http:\/\/127\.0\.0\.1:/, /on → Питание/]) assert.match(reviewText, pattern);
    assert.equal((await api('/api/config')).hash, original.hash); assert.equal(hardware.length, firstHardware);
    await screenshot(name + '-06-final-review');
    await js('document.querySelector("dialog").scrollTop = document.querySelector("dialog").scrollHeight');
    await delay(80);
    await screenshot(name + '-06b-http-review'); await click('Подтвердить применение');
    await until(async () => (await api('/api/setup/status')).status === 'applied', 'setup final Apply');
    await until(async () => (await text()).includes('Изменения применены. Активная версия проверена.'), 'visible Apply success');
    await click('Назад к плану');
    const applied = await api('/api/config'), active = (await api('/api/config/active')).active;
    assert.equal(applied.hash, active.hash); assert.notEqual(applied.hash, original.hash);
    assert.equal(applied.configuration.connections.length, 1); assert.equal(applied.configuration.bindings.length, 2); assert.equal(applied.configuration.observations.length, 1);
    assert.equal(hardware.length, firstHardware);
    const status = await api('/api/status'), runtime = (await api('/api/dashboard')).advanced;
    assert.equal(status.runtime_ready, true); assert.equal(runtime.voice, 'WAITING_WAKE');
    const device = applied.configuration.devices[0];
    await navigate('devices/' + device.id); await refresh(); await until(() => js('!!document.querySelector("[data-knowledge=unknown]")'), 'initial state Unknown');
    const stateOf = async () => (await api('/api/devices/state')).devices.find(d => d.targetId === device.id).properties.power;
    for (const on of [true,false]) {
        const count = hardware.length;
        const commandText = `${on ? 'включи' : 'выключи'} лампу${noZone ? '' : ' в кабинете'}`;
        const result = await voice(control, 'Амина, ' + commandText);
        recordVoice(result);
        const command = result.runtime.last_voice.command;
        assert.equal(command.execution.success, true, JSON.stringify(command)); assert.equal(command.language_route, 'INDEX_EXACT');
        assert.equal(result.runtime.executions.at(-1).source, 'voice');
        assert.equal(result.runtime.last_voice.wake_prefix_removed, true); assert.equal(result.runtime.last_voice.command_text, commandText);
        assert.equal(hardware.length, count + 1); assert.deepEqual(hardware.at(-1), {method:'POST',path:'/relay/1',body:JSON.stringify({on})});
        assert.equal((await stateOf()).knowledge, 'predicted'); assert.equal((await stateOf()).value, on);
        knowledge.push({step:on?'ON POST':'OFF POST',state:await stateOf()});
        await refresh(); await until(() => js('!!document.querySelector("[data-knowledge=predicted]")'), 'UI Predicted');
        await screenshot(name + (on ? '-07-on-predicted' : '-09-off-predicted'));
        await click('Прочитать состояние', '.operational'); await until(() => js('!!document.querySelector("[data-knowledge=confirmed]")'), 'UI Confirmed');
        assert.equal(hardware.length, count + 2); assert.deepEqual(hardware.at(-1), {method:'GET',path:'/relay/1',body:''});
        assert.equal((await stateOf()).knowledge, 'confirmed'); assert.equal((await stateOf()).value, on);
        knowledge.push({step:on?'ON GET':'OFF GET',state:await stateOf()});
        await screenshot(name + (on ? '-08-on-confirmed' : '-10-off-confirmed'));
    }
    const unsafeStart = hardware.length;
    const suffix = noZone ? '' : ' в кабинете';
    for (const transcript of ['Амина, не включай лампу' + suffix, 'Амина, включи и выключи лампу' + suffix, 'Амина, какая погода', 'Амина, включи лампу в неизвестной комнате']) {
        const result = await voice(control, transcript); recordVoice(result); assert.equal(result.runtime.last_voice.command?.execution ?? null, null, transcript);
    }
    recordVoice(await voice(control, 'Амина, включи лампу' + suffix, false)); assert.equal(hardware.length, unsafeStart, 'unsafe/no-wake voice zero HTTP');
    // Real GET contradicts the last false prediction/confirmation; do not repair it.
    power = true; await click('Прочитать состояние', '.operational'); await until(async () => (await stateOf()).value === true, 'contradictory GET wins');
    knowledge.push({step:'contradictory GET',state:await stateOf()});
    await screenshot(name + '-11-independent-contradiction');
    const failures = [];
    for (const mode of ['malformed','wrong_type','unavailable']) {
        fault = mode; const before = await stateOf(), count = hardware.length;
        await click('Прочитать состояние', '.operational'); await until(async () => (await text()).includes('Чтение не подтвердило состояние'), 'visible Observe failure ' + mode);
        await until(() => js('![...document.querySelectorAll(".operational button")].find(n => n.textContent === "Прочитать состояние").disabled'), 'read completed');
        assert.equal(hardware.length, count + 1); assert.equal(hardware.at(-1).method, 'GET'); assert.deepEqual(await stateOf(), before);
        failures.push({ mode, message:await js('document.querySelector(".operational").innerText') });
    }
    await screenshot(name + '-12-observe-failed'); fault = '';
    const staleCount = hardware.length;
    await api('/api/devices/poll', {target_id:device.id,expected_hash:original.hash}, 422); assert.equal(hardware.length, staleCount);
    const persisted = fs.readFileSync(path.join(store, 'configuration.json'));
    fs.rmSync(control, {force:true}); await stop();
    const beforeRestart = hardware.length; await start(store, control);
    assert.equal((await api('/api/config')).hash, applied.hash); assert.deepEqual(fs.readFileSync(path.join(store,'configuration.json')), persisted);
    assert.equal((await stateOf()).knowledge, 'unknown'); assert.equal(hardware.length, beforeRestart); assert.equal((await api('/api/executions')).executions.length, 0);
    knowledge.push({step:'restart before GET',state:await stateOf()});
    await send('Page.navigate', {url:url + '/#/devices/' + device.id}); await until(() => js('!!document.querySelector("[data-knowledge=unknown]")'), 'restart UI Unknown');
    await screenshot(name + '-13-restart-unknown'); await click('Прочитать состояние', '.operational'); await until(() => js('!!document.querySelector("[data-knowledge=confirmed]")'), 'restart GET confirms');
    assert.equal(hardware.length, beforeRestart + 1); assert.equal((await stateOf()).value, true);
    knowledge.push({step:'restart GET',state:await stateOf()});
    await screenshot(name + '-14-restart-confirmed');
    // Deliberate corruption is confined to this disposable private store.
    // Existing startup recovers Last Known Good; never replay a hardware action.
    await stop();
    fs.writeFileSync(path.join(store, 'configuration.json'), '{invalid-fixture');
    const beforeRecovery = hardware.length;
    await start(store, control);
    const recovered = await api('/api/status');
    assert.equal(recovered.health.config_recovered, true);
    assert.equal((await api('/api/config/active')).active.hash, applied.hash);
    assert.equal((await stateOf()).knowledge, 'unknown');
    assert.equal((await api('/api/executions')).executions.length, 0);
    assert.equal(hardware.length, beforeRecovery, 'LKG recovery performs zero HTTP');
    await send('Page.navigate', {url:url + '/#/devices/' + device.id});
    await until(async () => (await text()).includes('Не все данные удалось обновить'), 'visible degraded store recovery');
    await screenshot(name + '-15-lkg-recovery');
    const browserCalls = requests.slice(firstRequests).filter(r => r.url.startsWith(url + '/') || r.url.includes('/api/'));
    assert.equal(browserCalls.filter(r => r.url.endsWith('/api/setup/apply')).length, 1, 'one final Apply');
    assert.equal(browserCalls.filter(r => /\/api\/admin\/copilot|\/api\/config\/apply|\/api\/test-command|\/api\/devices\/command/.test(r.url)).length, 0, 'voice uses real pipeline, not browser Execute');
    const rows = hardware.slice(firstHardware);
    assert.equal(rows.filter(r => r.method === 'POST').length, 2); assert.equal(rows.filter(r => r.method === 'GET').length, 7);
    results.push({scenario:name, applied:{hash:applied.hash,revision:applied.revision}, zeroIoBeforeApply:true, zeroHttpBytesProbe:true, conflicts:['second-owner','wrong-owner','stale-draft-version','stale-observe-hash'], posts:2, gets:7, hardware:rows, voiceEvidence, knowledge, failures, status, build:runtime.build, recovered, browserCalls});
    console.log('PASS', name, 'empty → AI → accept/cancel/resume → explicit HTTP wizard → one Apply → wake/voice ON/OFF → Observe → failures → restart; POST=2 GET=7');
    await stop();
}
(async () => {
    await new Promise(resolve => mock.listen(0,'127.0.0.1',resolve));
    chrome = spawn(chromium, ['--ozone-platform=x11','--no-sandbox','--disable-gpu','--disable-dev-shm-usage','--disable-background-networking','--no-first-run','--remote-debugging-port=0','--user-data-dir=' + path.join(temp,'chrome'),'about:blank'], {stdio:['ignore',log,log]});
    const portFile = path.join(temp,'chrome','DevToolsActivePort'); await until(() => fs.existsSync(portFile), 'Chromium CDP');
    const port = fs.readFileSync(portFile,'utf8').split('\n')[0], pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    socket = new WebSocket(pages.find(p => p.type === 'page').webSocketDebuggerUrl); await new Promise((resolve,reject) => {socket.onopen=resolve;socket.onerror=reject;});
    socket.onmessage = event => { const message = JSON.parse(event.data);
        if (message.id) {const task=pending.get(message.id);pending.delete(message.id);if(task)message.error?task.reject(message.error):task.resolve(message.result);}
        else if(message.method==='Runtime.exceptionThrown')exceptions.push(message.params);
        else if(message.method==='Page.javascriptDialogOpening')send('Page.handleJavaScriptDialog',{accept:true});
        else if(message.method==='Fetch.requestPaused') {
            const {request,requestId}=message.params;requests.push({url:request.url,method:request.method});
            const closingAtPause = pageClosing;
            const safe=request.url.startsWith(url + '/');if(!safe)external.push(request.url);
            send(safe?'Fetch.continueRequest':'Fetch.failRequest',safe?{requestId}:{requestId,errorReason:'BlockedByClient'}).catch(error=>{
                const record = {request:request.url,method:request.method,protocolError:error};
                // Explicit about:blank shutdown can cancel an already paused
                // diagnostic GET before CDP acknowledges continueRequest.
                // Record that exact lifecycle cancellation; never retry it or
                // exempt a mutation, external request or application exception.
                if (safe && request.method === 'GET' && (pageClosing || closingAtPause) &&
                    request.url.startsWith(url + '/api/') &&
                    [-32000,-32602].includes(error.code) && error.message === 'Invalid InterceptionId.')
                    cancelledDiagnosticReads.push(record);
                else exceptions.push(record);
            });
        }
    };
    await send('Runtime.enable'); await send('Page.enable'); await send('Fetch.enable',{patterns:[{urlPattern:'*'}]});
    await rejectedOwnerProfile(); await scenario(false); await scenario(true);
    assert.deepEqual(external,[]); assert.deepEqual(exceptions,[]);
    proof = {passed:true,model:'scripted; no real Qwen/NPU',voice:'synthetic PCM/KWS/VAD/STT; real runWakeVoiceLoop/processVoice',appSha256:crypto.createHash('sha256').update(fs.readFileSync(app)).digest('hex'),workerSha256:crypto.createHash('sha256').update(fs.readFileSync(worker)).digest('hex'),tcpConnections,external,exceptions,diagnosticBusyReads,cancelledDiagnosticReads,results};
})().catch(async error => {
    console.error(error); if(socket?.readyState===1){try{await screenshot('failure');fs.writeFileSync(path.join(artifacts,'failure.txt'),await text());}catch{}}
    process.exitCode=1;
}).finally(async () => {
    socket?.close(); if(server)await stop(); if(chrome){chrome.kill('SIGTERM');await new Promise(resolve=>chrome.once('exit',resolve));}
    await new Promise(resolve=>mock.close(resolve));fs.closeSync(log);
    await fs.promises.rm(temp,{recursive:true,force:true,maxRetries:10,retryDelay:100});
    if (proof && !process.exitCode) {
        fs.writeFileSync(path.join(artifacts,'results.json'),JSON.stringify(proof,null,2));
        for (const file of ['failure.txt','failure.png']) fs.rmSync(path.join(artifacts,file), {force:true});
    }
});
