'use strict';
// Real production bundle + C++ API. Happy path exclusively uses visible controls.
const fs = require('node:fs'), os = require('node:os'), path = require('node:path'), http = require('node:http');
const { spawn } = require('node:child_process');
const assert = require('node:assert/strict');
const [app, worker, root, chromium, artifacts] = process.argv.slice(2);
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'firefly-b3-browser-'));
fs.mkdirSync(path.join(temp, 'store'), { mode: 0o700 }); fs.mkdirSync(artifacts, { recursive: true });
const entryGate = path.join(temp, 'entry-gate'); fs.writeFileSync(entryGate, 'hold');
const log = fs.openSync(path.join(artifacts, 'server.log'), 'w'), failFile = path.join(temp, 'fail-apply');
let server, chrome, socket, output = '', nextId = 0, url, power = false;
const pending = new Map(), requests = [], external = [], exceptions = [], hardware = [];
let tcpConnections = 0, tcpBytes = 0, holdStt = false, pendingStt = null;
const sttResult = Buffer.from(JSON.stringify({ok:true,text:'Добавь вентилятор в Цех'})).toString('base64');
const mock = http.createServer((req, res) => { let body = ''; req.on('data', b => body += b); req.on('end', () => { hardware.push({ method: req.method, url: req.url, body }); if (req.method === 'POST') power = JSON.parse(body).on; const response = JSON.stringify({ on: power }); res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(response) }); res.end(response); }); });
mock.on('connection', socket => { tcpConnections++; socket.on('data', bytes => tcpBytes += bytes.length); });
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(check, label, ms = 25000) { const end = Date.now() + ms; while (Date.now() < end) { if (await check()) return; await delay(80); } throw Error('Timeout: ' + label); }
function send(method, params = {}) { return new Promise((resolve, reject) => { const id = ++nextId; pending.set(id, { resolve, reject }); socket.send(JSON.stringify({ id, method, params })); }); }
async function js(expression) { const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true }); if (r.exceptionDetails) throw Error(JSON.stringify(r.exceptionDetails)); return r.result.value; }
async function text() { return js('document.body?.innerText ?? ""'); }
// DOM control events; never access Vue internals or assign a configuration candidate.
async function click(label, scope = '') {
    const lookup = `const root = (${JSON.stringify(scope)} ? document.querySelector(${JSON.stringify(scope)}) : null) || document.querySelector('dialog[open]') || document.querySelector('main'); const node = [...root.querySelectorAll('button,a')].find(n => n.textContent.trim().replace(/\\s*↗$/, '') === ${JSON.stringify(label)} && n.getClientRects().length && !n.matches(':disabled'));`;
    await until(() => js(`(() => { ${lookup} return !!node; })()`), 'visible enabled control ' + label);
    await js(`(() => { ${lookup} node.scrollIntoView({block:'center'}); node.click(); })()`); await delay(80);
}
async function fill(label, value, scope = '') { await js(`(() => { const root = (${JSON.stringify(scope)} ? document.querySelector(${JSON.stringify(scope)}) : null) || document.querySelector('dialog[open]') || document.querySelector('main'); const label = [...root.querySelectorAll('label')].find(n => n.textContent.trim().startsWith(${JSON.stringify(label)}) && n.getClientRects().length); const control = label?.querySelector('input,textarea'); if (!control) throw Error('Field missing: ' + ${JSON.stringify(label)}); control.focus(); control.value = ${JSON.stringify(value)}; control.dispatchEvent(new Event('input', {bubbles:true})); control.dispatchEvent(new Event('change', {bubbles:true})); })()`); }
async function insertRaw(text) {
    const before = await js('document.activeElement.value');
    await send('Input.insertText', { text }); await delay(60);
    assert.equal(await js('document.activeElement.value'), before + text, 'typing preserves trailing spaces and newlines');
}
async function select(label, option, scope = '') { await js(`(() => { const root = (${JSON.stringify(scope)} ? document.querySelector(${JSON.stringify(scope)}) : null) || document.querySelector('dialog[open]') || document.querySelector('main'); const node = [...root.querySelectorAll('label')].find(n => n.textContent.trim().startsWith(${JSON.stringify(label)}))?.querySelector('select'); const value = [...node.options].find(o => o.textContent.trim() === ${JSON.stringify(option)})?.value; if (value === undefined) throw Error('Option missing: ' + ${JSON.stringify(option)}); node.value = value; node.dispatchEvent(new Event('change', {bubbles:true})); })()`); await delay(50); }
async function check(label) { await js(`(() => { const node = [...document.querySelectorAll('dialog[open] label')].find(n => n.textContent.trim().replace(/\\s*↗$/, '') === ${JSON.stringify(label)})?.querySelector('input[type=checkbox]'); if (!node) throw Error('Checkbox missing'); node.click(); })()`); }
async function route(name) { await js(`document.querySelector('nav a[href$="#/${name}"]').click()`); await until(() => js(`location.hash === '#/${name}'`), name); }
async function ready() { await until(() => js('!document.querySelector("dialog[open] fieldset:disabled")'), 'idle task'); }
async function stage() { await click('Добавить в план'); await until(() => js('!document.querySelector("dialog[open]")'), 'task staged'); }
async function review() { await click('Сохранить и применить', 'main .manual-toolbar'); await until(() => js('[...document.querySelectorAll("dialog[open] button")].some(b => b.textContent.trim() === "Подтвердить применение" && !b.disabled)'), 'review ready'); }
async function apply() { await click('Подтвердить применение'); await until(async () => (await text()).includes('Изменения применены. Активная версия проверена.'), 'applied'); await until(() => js('[...document.querySelectorAll("dialog[open] button")].some(b => b.textContent.trim() === "Назад к плану" && !b.disabled)'), 'post-Apply refresh'); await click('Назад к плану'); }
async function planDevice(name, button) { await js(`(() => { const row = [...document.querySelectorAll('main .panel .inline-actions')].find(n => n.querySelector('strong')?.textContent === ${JSON.stringify(name)}); const b = [...row.querySelectorAll('button')].find(n => n.textContent === ${JSON.stringify(button)}); b.click(); })()`); await until(() => js('!!document.querySelector("dialog[open]")'), 'device task'); }
async function api(endpoint) { const r = await fetch(url + endpoint); assert.equal(r.status, 200); return r.json(); }
async function screenshot(name) { const r = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true }); fs.writeFileSync(path.join(artifacts, name + '.png'), Buffer.from(r.data, 'base64')); }
(async () => {
    await new Promise(resolve => mock.listen(0, '127.0.0.1', resolve));
    const endpoint = `http://127.0.0.1:${mock.address().port}`;
    const config = JSON.parse(fs.readFileSync(path.join(root, 'tests/admin/v2/system.json')));
    config.rooms = []; config.devices = []; config.actions = []; config.system.name = 'Пустой стенд';
    const fixture = path.join(temp, 'empty.json'); fs.writeFileSync(fixture, JSON.stringify(config));
    server = spawn(app, ['--store', path.join(temp, 'store'), '--init-config', fixture, '--runtime-worker', worker, '--admin-port', '0'], { stdio: ['ignore', 'pipe', log], env: { ...process.env, FIREFLY_B2_FAIL_APPLY_FILE: failFile, FIREFLY_B3_ENTRY_GATE: entryGate } });
    server.stdout.on('data', b => output += b); await until(() => output.includes('ADMIN_LISTEN'), 'C++ server'); url = output.match(/ADMIN_LISTEN (http:\/\/\S+)/)[1];
    await until(async () => (await api('/api/status')).runtime_ready, 'runtime');
    const before = await api('/api/config');
    chrome = spawn(chromium, ['--ozone-platform=x11', '--no-sandbox', '--disable-gpu', '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--disable-dev-shm-usage', '--disable-background-networking', '--no-first-run', '--remote-debugging-port=0', '--user-data-dir=' + path.join(temp, 'chrome'), 'about:blank'], { stdio: ['ignore', log, log] });
    const portFile = path.join(temp, 'chrome', 'DevToolsActivePort'); await until(() => fs.existsSync(portFile), 'Chromium');
    const port = fs.readFileSync(portFile, 'utf8').split('\n')[0]; const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    socket = new WebSocket(pages.find(p => p.type === 'page').webSocketDebuggerUrl); await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
    socket.onmessage = e => { const m = JSON.parse(e.data); if (m.id) { const p = pending.get(m.id); pending.delete(m.id); if (p) m.error ? p.reject(m.error) : p.resolve(m.result); }
        else if (m.method === 'Runtime.exceptionThrown') exceptions.push(m.params);
        else if (m.method === 'Page.javascriptDialogOpening') send('Page.handleJavaScriptDialog', { accept: true });
        else if (m.method === 'Fetch.requestPaused') { const { request, requestId } = m.params; requests.push({ url: request.url, method: request.method, body: request.postData }); if (request.url.endsWith('/api/admin/transcribe')) { if (holdStt) pendingStt = requestId; else send('Fetch.fulfillRequest', {requestId,responseCode:200,responseHeaders:[{name:'Content-Type',value:'application/json'}],body:sttResult}); return; } const safe = request.url.startsWith(url + '/') || request.url.startsWith('blob:' + url); if (!safe) external.push(request); send(safe ? 'Fetch.continueRequest' : 'Fetch.failRequest', safe ? { requestId } : { requestId, errorReason: 'BlockedByClient' }); }
    };
    await send('Runtime.enable'); await send('Page.enable'); await send('Fetch.enable', { patterns: [{ urlPattern: '*' }] });
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: url + '/' }); await until(async () => (await text()).includes('Пустой стенд'), 'overview');

    await route('setup');
    assert.equal(requests.filter(r => r.url.endsWith('/api/setup/start')).length, 0, 'opening setup never starts ownership');
    assert.equal((await api('/api/setup/status')).active, false);
    await click('Начать настройку'); await until(async () => (await api('/api/setup/status')).active, 'setup active');
    await until(() => js('!!JSON.parse(sessionStorage.getItem("firefly-v2-setup-owner") || "{}").token'), 'owner persisted before entry finishes');
    const entryToken = await js('JSON.parse(sessionStorage.getItem("firefly-v2-setup-owner")).token');
    assert.equal((await api('/api/setup/status')).status, 'entering');
    assert.match(await text(), /Подготавливаем локальный AI/);
    assert.ok(await js('document.querySelector(".setup-fields").disabled'));
    assert.ok(await js('document.querySelector("main .manual-toolbar button").disabled'));
    assert.equal(await js('[...document.querySelectorAll("main button")].some(b => b.textContent === "Начать настройку" && !b.disabled)'),false);
    await send('Page.reload');
    await until(async () => (await text()).includes('Подготавливаем локальный AI'), 'reload preparation state');
    await until(() => js('!!document.querySelector(".setup-fields")'), 'same owner after reload');
    assert.equal(await js('JSON.parse(sessionStorage.getItem("firefly-v2-setup-owner")).token'),entryToken);
    assert.doesNotMatch(await text(), /другим браузером|принадлежит другому браузеру/);
    // A separate page without opener has independent sessionStorage.
    const peer = await send('Target.createTarget',{url:url+'/#/setup'});
    const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const peerSocket = new WebSocket(targets.find(t=>t.id===peer.targetId).webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{peerSocket.onopen=resolve;peerSocket.onerror=reject;});
    let peerSeq=0; const peerPending=new Map();
    peerSocket.onmessage=e=>{const m=JSON.parse(e.data);if(m.id){peerPending.get(m.id)?.(m.result);peerPending.delete(m.id);}};
    const peerJs=expression=>new Promise(resolve=>{const id=++peerSeq;peerPending.set(id,r=>resolve(r.result?.value));peerSocket.send(JSON.stringify({id,method:'Runtime.evaluate',params:{expression,returnByValue:true}}));});
    await until(async()=>String(await peerJs('document.body.innerText')).includes('Доступен только просмотр'),'second tab read-only');
    assert.equal(await peerJs('sessionStorage.getItem("firefly-v2-setup-owner")'),null);
    assert.equal(await peerJs('!!document.querySelector(".setup-fields")'),false);
    await send('Target.closeTarget',{targetId:peer.targetId});peerSocket.close();
    assert.equal((await api('/api/config')).hash,before.hash);
    assert.equal(hardware.length,0);
    assert.equal(requests.filter(r=>r.url.endsWith('/api/setup/apply')).length,0);
    fs.unlinkSync(entryGate);
    await until(() => js('!!document.querySelector(".setup-fields") && !document.querySelector(".setup-fields").disabled'), 'automatic entry completion');
    async function ask(value) { await until(() => js('[...document.querySelectorAll("main label")].some(l => l.textContent.startsWith("Ваша просьба") && l.getClientRects().length)'), 'composer ready'); await fill('Ваша просьба', value); await click('Отправить'); await until(() => js('!document.querySelector("main .setup-screen [role=status]")?.textContent.includes("Локальный AI")'), 'AI done'); await delay(150); }
    async function proposal() { await until(() => js('!!document.querySelector(".proposal-card")'), 'proposal card'); }
    async function accept(physical = false) { await click(physical ? 'Принять подключение в план' : 'Принять в план'); await until(() => js('!document.querySelector(".proposal-card")'), 'proposal accepted'); }
    console.log('A: atomic semantic request, pending review, reload, checkpoint reject');
    await ask('Создай Детскую, добавь туда Свет и Телевизор. Оба только включаются и выключаются.'); await proposal();
    let draft = (await api('/api/setup/status')).configuration;
    assert.equal(draft.rooms.length, 1); assert.equal(draft.devices.length, 2); assert.ok(draft.devices.every(d => d.action_ids.length === 2)); assert.equal(draft.bindings.length, 0);
    assert.deepEqual(draft.rooms[0].speech, { mentions:["детская"], locations:["в детской"] });
    assert.equal((await api('/api/config')).hash, before.hash); assert.equal(hardware.length, 0);
    await send('Page.reload'); await until(async () => (await text()).includes('Неподтверждённое предложение восстановлено'), 'reload requires human review'); await proposal();
    await click('Отклонить'); await until(() => js('!document.querySelector(".proposal-card")'), 'reject'); assert.equal((await api('/api/setup/status')).configuration.devices.length, 0);
    await ask('Создай Детскую, добавь туда Свет и Телевизор. Оба только включаются и выключаются.'); await proposal(); await accept();
    console.log('Voice component fixture (not live STT): explicit Send, closed tracks, ignored late result');
    await js(`(() => { const original = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices); window.testTracks = []; navigator.mediaDevices.getUserMedia = async constraints => { const stream = await original(constraints); window.testTracks.push(...stream.getTracks()); return stream; }; [...document.querySelectorAll('summary')].find(n=>n.textContent==='Локальный голосовой ввод').click(); })()`);
    const messagesBeforeVoice = requests.filter(r=>r.url.endsWith('/api/setup/message')).length;
    await click('Записать голос'); await until(async () => (await text()).includes('Запись ·'), 'recording timer');
    assert.equal(await js('document.querySelector("main .manual-toolbar button").disabled'), true);
    await delay(300); await click('Остановить и распознать'); await until(() => js('[...document.querySelectorAll("main label")].find(l=>l.textContent.startsWith("Ваша просьба")).querySelector("textarea").value === "Добавь вентилятор в Цех"'), 'editable transcript');
    assert.equal(requests.filter(r=>r.url.endsWith('/api/setup/message')).length,messagesBeforeVoice);
    await fill('Ваша просьба','Ручная правка transcript');
    holdStt = true; await click('Записать голос'); await until(async () => (await text()).includes('Запись ·'), 'second recording'); await delay(250); await click('Остановить и распознать'); await until(()=>pendingStt!==null,'pending transcription');
    await route('zones'); assert.ok(await js('window.testTracks.every(t=>t.readyState === "ended")'));
    await send('Fetch.fulfillRequest',{requestId:pendingStt,responseCode:200,responseHeaders:[{name:'Content-Type',value:'application/json'}],body:sttResult}); pendingStt=null; holdStt=false;
    await route('setup'); await until(()=>js('!!document.querySelector(".setup-screen textarea")'),'composer returned');
    assert.equal(await js('document.querySelector(".setup-screen textarea").value'),'Ручная правка transcript');
    console.log('B: non-home zones, explicit context and standalone device');
    for (const zone of ['Цех упаковки', 'Склад', 'Лабораторный стенд']) { await ask('Создай зону ' + zone); await proposal(); await accept(); }
    for (const [name, location] of [['Цех упаковки','в цехе упаковки'],['Склад','на складе'],['Лабораторный стенд','на лабораторном стенде']]) {
      const zone = (await api('/api/setup/status')).configuration.rooms.find(z=>z.name===name);
      assert.deepEqual(zone.speech, {mentions:[name.toLowerCase()], locations:[location]});
    }
    await select('Явный контекст зоны', 'Цех упаковки');
    await ask('Добавь вентилятор, только включение и выключение.'); await proposal(); assert.match(await text(), /Цех упаковки/); await accept();
    await select('Явный контекст зоны', 'Контекст не выбран');
    await ask('Добавь контроллер стенда без зоны.'); await proposal();
    assert.match(await text(), /Контроллер стенда · Без зоны\. Возможности: ещё не заданы/); await accept();
    draft = (await api('/api/setup/status')).configuration;
    const standalone = draft.devices.find(d => d.name === 'Контроллер стенда'); assert.equal(standalone.room_id, null); assert.equal(standalone.action_ids.length, 0); assert.equal(draft.rooms.length, 4);
    await click('Настроить подключение: Контроллер стенда'); await click('Настроить подключение вручную'); await until(() => js('!!document.querySelector("dialog .wizard")'), 'non-power B2 fallback'); await click('Отмена');
    console.log('D: insufficient facts never call Copilot; follow-up accumulates explicit contract');
    await click('Настроить подключение: Вентилятор');
    await select('Общее подключение:', 'Создать отдельное подключение для выбранного устройства');
    const copilotBefore = requests.filter(r => r.url.endsWith('/api/admin/copilot')).length;
    await ask('Подключи насос к контроллеру.'); assert.match(await text(), /Какой адрес и протокол/);
    await ask('как обычно'); assert.equal(requests.filter(r => r.url.endsWith('/api/admin/copilot')).length, copilotBefore);
    await ask(endpoint); await ask('POST /relay/1 {"on":true}'); await ask('POST /relay/1 {"on":false}'); await ask('GET /relay/1, поле on'); await proposal();
    assert.match(await text(), /Подключение из явных фактов/);
    assert.equal(requests.filter(r => r.url.endsWith('/api/admin/copilot') || r.url.endsWith('/api/admin/copilot/accept')).length,0);
    assert.equal((await api('/api/setup/status')).configuration.bindings.length,0,'physical proposal is not yet accepted');
    await send('Page.reload'); await until(async () => (await text()).includes('Неподтверждённое предложение восстановлено'), 'physical reload'); await proposal();
    const physicalOwner = await js('JSON.parse(sessionStorage.getItem("firefly-v2-setup-owner")).token'), physicalBase = await api('/api/setup/status');
    const racedPhysical = await fetch(url+'/api/setup/draft',{method:'POST',headers:{'Content-Type':'application/json','X-Firefly-Admin':'1',Origin:url},body:JSON.stringify({session_id:physicalOwner,version:physicalBase.version,configuration:physicalBase.configuration})});
    assert.equal(racedPhysical.status,200); await click('Принять подключение в план'); await until(async ()=>(await text()).includes('План изменён другой вкладкой'),'physical stale version');
    assert.equal((await api('/api/setup/status')).configuration.bindings.length,0);
    await click('Перечитать серверный черновик для review'); await until(()=>js('!document.querySelector(".proposal-card")'),'stale proposal cleared');
    await ask(endpoint); await proposal();
    assert.equal(requests.filter(r=>r.url.endsWith('/api/admin/copilot')).length,0);
    assert.equal(requests.filter(r => r.url.endsWith('/api/setup/message') && /127\.0\.0\.1|relay\/1/.test(r.body)).length, 0);
    assert.equal((await api('/api/config')).hash, before.hash); assert.equal(hardware.length, 0);
    await click('Изменить: Подключение'); await until(()=>js('!!document.querySelector("dialog .wizard")'),'manual physical edit');
    await click('HTTP / REST →');
    assert.equal(await js('[...document.querySelectorAll("dialog label")].find(l=>l.textContent.startsWith("Endpoint")).querySelector("input").value'),endpoint,'B2 receives explicit proposal values');
    await click('Отмена'); assert.equal((await api('/api/setup/status')).configuration.bindings.length,0);
    await ask(endpoint); await proposal();
    // Fault fixture enters an unrelated incomplete connection through the visible import form.
    await route('advanced'); const invalid = structuredClone((await api('/api/setup/status')).configuration);
    invalid.connections = [...(invalid.connections ?? []), { id:'unfinished', name:'Незавершённое B', type:'http', enabled:true, timeout_ms:2000, http:{base_url:''} }];
    await fill('Документ кандидата', JSON.stringify(invalid)); await click('Проверить импорт и добавить в план'); await until(() => js('!!document.querySelector("main [role=alert]")'), 'invalid import');
    await route('setup');
    await click('Проверить доступность'); await until(async () => (await text()).includes('TCP доступен'), 'TCP probe'); assert.equal(hardware.length, 0); assert.ok(tcpConnections > 0); assert.equal(tcpBytes,0,'probe sends exactly zero HTTP bytes');
    await accept(true);
    await click('Сохранить и применить', 'main .manual-toolbar'); await until(async () => (await text()).includes('Непроверенный импорт блокирует'), 'unrelated incomplete task blocks final Apply');
    await click('Перейти к полю или сущности'); await click('Отбросить импорт'); await route('setup');
    console.log('C: one explicit final setup Apply, zero hardware; independent read confirms');
    await click('Сохранить и применить', 'main .manual-toolbar'); await until(() => js('[...document.querySelectorAll("dialog button")].some(b => b.textContent === "Подтвердить применение" && !b.disabled)'), 'setup final review');
    assert.equal(hardware.length, 0); assert.equal((await api('/api/config')).hash, before.hash);
    assert.match(await text(),/on = Да/); assert.match(await text(),/on = Нет/); assert.match(await text(),/on → Питание/);
    await screenshot('explicit-final-review'); await click('Подтвердить применение'); await until(async () => (await api('/api/setup/status')).status === 'applied', 'setup Apply'); assert.equal(hardware.length, 0);
    assert.equal(requests.filter(r => r.method === 'PUT' || r.url.endsWith('/api/config/apply')).length, 0, 'setup lifecycle cannot be bypassed');
    await click('Назад к плану');
    const applied = (await api('/api/config')).configuration; const fan = applied.devices.find(d => d.name === 'Вентилятор');
    await route('devices'); await js(`document.querySelector('a[href$="/devices/${fan.id}"]').click()`); await delay(100);
    await click('Прочитать состояние'); await until(() => hardware.length === 1, 'explicit observe'); assert.equal(hardware[0].method, 'GET'); await until(async () => (await text()).includes('Подтверждено'), 'confirmed evidence');
    console.log('F: new session after semantic/physical Apply; stable rename retains contract');
    await route('setup'); await click('Начать настройку'); await until(() => js('!!document.querySelector(".context-chips")'), 'new session composer'); await select('Явный контекст зоны', 'Цех упаковки');
    await ask('Переименуй Вентилятор в зоне упаковки в Вытяжку.'); await proposal();
    // Same-owner second-tab write advances the real server version: intentional conflict, not happy-path candidate injection.
    const ownerToken = await js('JSON.parse(sessionStorage.getItem("firefly-v2-setup-owner")).token'); const raced = await api('/api/setup/status');
    const conflict = await fetch(url + '/api/setup/draft', { method:'POST',headers:{'Content-Type':'application/json','X-Firefly-Admin':'1',Origin:url},body:JSON.stringify({session_id:ownerToken,version:raced.version,configuration:raced.configuration}) }); assert.equal(conflict.status,200);
    await click('Принять в план'); await until(async () => (await text()).includes('План изменён другой вкладкой'), 'version conflict'); assert.equal(hardware.length,1);
    await click('Перечитать серверный черновик для review'); await until(async () => (await text()).includes('Неподтверждённое предложение восстановлено'), 'explicit rereview'); await accept();
    const renamed = (await api('/api/setup/status')).configuration; assert.equal(renamed.devices.find(d => d.id === fan.id).name, 'Вытяжка'); assert.deepEqual(renamed.bindings, applied.bindings); assert.deepEqual(renamed.actions, applied.actions);
    console.log('Contextual AI assist: real editable values, cancel checkpoint, device and action language');
    await route('zones'); await click('Создать зону'); await fill('Название зоны', 'Цех'); await click('✨ Заполнить с AI');
    await until(() => js('document.querySelector("dialog textarea[aria-describedby=zone-mentions-example]")?.value.includes("Цех")'), 'real speech values');
    assert.equal(await js('document.querySelector("dialog textarea[aria-describedby=zone-mentions-example]").value'), 'Цех');
    assert.equal(await js('document.querySelector("dialog textarea[aria-describedby=zone-locations-example]").value'), 'в цехе');
    await fill('Как назвать зону', 'цех\nучасток'); await click('Отказаться от AI-полей'); assert.equal(await js('document.querySelector("dialog textarea[aria-describedby=zone-mentions-example]").value'), '');
    await click('✨ Заполнить с AI'); await until(async () => (await text()).includes('✨ Предложено AI'), 'AI label'); await click('Принять AI-поля'); await click('Отмена');
    assert.equal((await api('/api/setup/status')).configuration.rooms.length, 4, 'cancel local task preserves accepted plan');
    await route('devices'); await js(`[...document.querySelectorAll('main .panel .inline-actions')].find(n => n.querySelector('strong')?.textContent === 'Вытяжка').querySelector('button').click()`); await ready();
    await click('✨ Заполнить с AI', 'dialog .ai-assist'); await until(async () => (await text()).includes('✨ Предложено AI'), 'device assist'); await click('Принять AI-поля');
    await click('✨ Заполнить с AI', 'dialog .action-editor .ai-assist'); await until(async () => (await text()).includes('✨ Предложено AI'), 'action assist'); await click('Принять AI-поля'); await click('Отмена');
    assert.equal(hardware.length, 1);
    console.log('Shared connection: explicit choice and every consumer in deterministic proposal');
    await route('advanced'); const shared = structuredClone((await api('/api/setup/status')).configuration);
    const light = shared.devices.find(d => d.name === 'Свет');
    for (const a of light.action_ids) {
        const action = shared.actions.find(item => item.id === a);
        shared.bindings.push({ ...structuredClone(shared.bindings.find(b => b.target_id === fan.id)), target_id:light.id, action_id:a, http:{method:'POST',path:'/relay/1',body:{on:action.assignments[0].value},expected_status:200} });
    }
    await fill('Документ кандидата', JSON.stringify(shared)); await click('Проверить импорт и добавить в план'); await until(async () => (await text()).includes('Кандидат принят в план'), 'shared imported candidate');
    await route('setup'); await click('Настроить подключение: Вытяжка');
    await select('Общее подключение:', 'Изменить общее подключение для всех этих устройств');
    await ask('http://127.0.0.1:8765 Включить POST /relay/1 {"on":true}. Выключить POST /relay/1 {"on":false}. Состояние GET /relay/1, поле on.'); await proposal();
    const impacts = await js('[...document.querySelectorAll(".proposal-card p")].filter(p => p.textContent.startsWith("Все затронутые устройства:")).map(p=>p.textContent).join(" ")');
    assert.match(impacts,/Вытяжка/); assert.match(impacts,/Свет/); await click('Отклонить');
    await click('Завершить настройку без Apply'); await until(async () => !(await api('/api/setup/status')).active,'cancel physical phase');
    await click('Начать настройку'); await until(() => js('!!document.querySelector(".context-chips")'), 'fresh semantic session');
    console.log('E: malformed response retains checkpoint and blocks Apply; recovery is explicit');
    await route('setup'); await ask('сломанный запрос'); await until(async () => (await api('/api/setup/status')).message_incomplete, 'incomplete protocol');
    assert.equal((await api('/api/config')).configuration.devices.find(d => d.id === fan.id).name, 'Вентилятор');
    assert.equal(await js('[...document.querySelectorAll("main .manual-toolbar button")].some(b => !b.disabled)'), false);
    await click('Завершить настройку без Apply'); await until(async () => !(await api('/api/setup/status')).active, 'explicit cancel');
    await until(async () => !(await text()).includes('Локальный AI / проверка…'), 'idle after cancellation');
    for (const [width, height] of [[390,844],[800,600],[1280,800]]) {
        await send('Emulation.setDeviceMetricsOverride', { width,height,deviceScaleFactor:1,mobile:false });
        assert.ok(await js('document.documentElement.scrollWidth <= innerWidth + 2'), 'setup fits viewport'); await screenshot('setup-' + width);
    }
    assert.equal(hardware.length, 1); assert.equal(requests.filter(r=>/\/api\/admin\/copilot/.test(r.url)).length,0,'connectivity never invokes Copilot or its acceptance'); assert.equal(external.length, 0); assert.equal(exceptions.length, 0);
    fs.writeFileSync(path.join(artifacts, 'results.json'), JSON.stringify({ passed:true, cases:['A','B','C','D','E-malformed','F','contextual','no-zone','non-home','reload'], hardware, tcpConnections, tcpBytes, connectivityAiCalls:0, external, requests:requests.map(r=>({url:r.url,method:r.method})) }, null, 2));
    console.log('PASS B3 visible AI setup controls; zero hardware before Apply; explicit poll=1 GET; offline');
})().catch(async error => { console.error(error); if (socket) { try { console.error((await text()).slice(-12000)); await screenshot('failure'); } catch {} } process.exitCode = 1; }).finally(async () => {
    if (socket) socket.close(); mock.closeAllConnections(); await new Promise(resolve => mock.close(resolve));
    for (const child of [chrome, server]) { if (!child || child.exitCode !== null || child.signalCode !== null) continue; const done = new Promise(resolve => child.once('exit', resolve)); const timer = setTimeout(() => { child.kill('SIGKILL'); process.exitCode = 1; }, 10000); child.kill('SIGTERM'); await done; clearTimeout(timer); }
    fs.closeSync(log); if (!process.exitCode) fs.rmSync(temp, { recursive: true, force: true });
});
