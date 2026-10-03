'use strict';
// Real production bundle + C++ API. Happy path exclusively uses visible controls.
const fs = require('node:fs'), os = require('node:os'), path = require('node:path'), http = require('node:http');
const { spawn } = require('node:child_process');
const assert = require('node:assert/strict');
const [app, worker, root, chromium, artifacts] = process.argv.slice(2);
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'firefly-b2-browser-'));
fs.mkdirSync(path.join(temp, 'store'), { mode: 0o700 }); fs.mkdirSync(artifacts, { recursive: true });
const log = fs.openSync(path.join(artifacts, 'server.log'), 'w'), failFile = path.join(temp, 'fail-apply');
let server, chrome, socket, output = '', nextId = 0, url, power = false;
const pending = new Map(), requests = [], external = [], exceptions = [], hardware = [];
let tcpConnections = 0;
const mock = http.createServer((req, res) => { let body = ''; req.on('data', b => body += b); req.on('end', () => { hardware.push({ method: req.method, url: req.url, body }); if (req.method === 'POST') power = JSON.parse(body).on; const response = JSON.stringify({ on: power }); res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(response) }); res.end(response); }); });
mock.on('connection', () => tcpConnections++);
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
    server = spawn(app, ['--store', path.join(temp, 'store'), '--init-config', fixture, '--runtime-worker', worker, '--admin-port', '0'], { stdio: ['ignore', 'pipe', log], env: { ...process.env, FIREFLY_B2_FAIL_APPLY_FILE: failFile } });
    server.stdout.on('data', b => output += b); await until(() => output.includes('ADMIN_LISTEN'), 'C++ server'); url = output.match(/ADMIN_LISTEN (http:\/\/\S+)/)[1];
    await until(async () => (await api('/api/status')).runtime_ready, 'runtime');
    const before = await api('/api/config');
    chrome = spawn(chromium, ['--ozone-platform=x11', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--disable-background-networking', '--no-first-run', '--remote-debugging-port=0', '--user-data-dir=' + path.join(temp, 'chrome'), 'about:blank'], { stdio: ['ignore', log, log] });
    const portFile = path.join(temp, 'chrome', 'DevToolsActivePort'); await until(() => fs.existsSync(portFile), 'Chromium');
    const port = fs.readFileSync(portFile, 'utf8').split('\n')[0]; const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    socket = new WebSocket(pages.find(p => p.type === 'page').webSocketDebuggerUrl); await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
    socket.onmessage = e => { const m = JSON.parse(e.data); if (m.id) { const p = pending.get(m.id); pending.delete(m.id); if (p) m.error ? p.reject(m.error) : p.resolve(m.result); }
        else if (m.method === 'Runtime.exceptionThrown') exceptions.push(m.params);
        else if (m.method === 'Page.javascriptDialogOpening') send('Page.handleJavaScriptDialog', { accept: true });
        else if (m.method === 'Fetch.requestPaused') { const { request, requestId } = m.params; requests.push({ url: request.url, method: request.method, body: request.postData }); const safe = request.url.startsWith(url + '/') || request.url.startsWith('blob:' + url); if (!safe) external.push(request); send(safe ? 'Fetch.continueRequest' : 'Fetch.failRequest', safe ? { requestId } : { requestId, errorReason: 'BlockedByClient' }); }
    };
    await send('Runtime.enable'); await send('Page.enable'); await send('Fetch.enable', { patterns: [{ urlPattern: '*' }] });
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: url + '/v2/' }); await until(async () => (await text()).includes('Пустой стенд'), 'overview');
    console.log('A: zone create/edit; B/C: no-zone device and Цех → Вентилятор');
    await route('zones');
    console.log('Manual acceptance regression: visible examples are not speech values');
    for (const name of ['Склад', 'Лабораторный стенд', 'Цех']) {
        await click('Создать зону'); await ready(); await fill('Название зоны', name);
        const speechFields = await js(`Array.from(document.querySelectorAll('dialog textarea[data-path*="/speech/"]'), n => ({value: n.value, placeholder: n.getAttribute('placeholder'), required: n.getAttribute('aria-required')}))`);
        assert.deepEqual(speechFields, [{value:'', placeholder:null, required:'true'}, {value:'', placeholder:null, required:'true'}]);
        assert.equal(await js(`['zone-mentions-example','zone-locations-example'].every(id => { const n = document.getElementById(id); return n?.getClientRects().length && n.textContent.includes('Пример:'); })`), true, 'examples visible outside empty fields');
        const validationCount = requests.filter(r => r.url.endsWith('/api/config/validate')).length;
        await click('Добавить в план');
        await until(() => js('document.activeElement?.dataset.path?.endsWith("/speech/mentions")'), 'focus first required speech field');
        assert.match(await text(), /Укажите, как обращаться к этой зоне/);
        assert.match(await text(), /Укажите, как обозначать местоположение/);
        assert.doesNotMatch(await text(), /Command location requires|container/);
        assert.equal(await js('document.querySelectorAll("dialog textarea[aria-invalid=true]").length'), 2);
        await click('Перейти к полю или сущности', 'dialog .task-errors li:nth-of-type(2)');
        assert.equal(await js('document.activeElement.dataset.path.endsWith("/speech/locations")'), true, 'error link focuses current task field');
        assert.equal(requests.filter(r => r.url.endsWith('/api/config/validate')).length, validationCount, 'incomplete zone never sent to backend');
        assert.equal(await js(`document.querySelector('dialog input[data-path$="/name"]').value`), name);
        if (name !== 'Цех') { await click('Отмена'); continue; }
        await screenshot('zone-required-speech');
        await fill('Как назвать зону', 'цех'); await insertRaw(' '); await insertRaw('\n'); await insertRaw('цех '); await click('Добавить в план');
        await until(() => js('document.activeElement?.dataset.path?.endsWith("/speech/locations")'), 'focus remaining speech field');
        assert.equal(requests.filter(r => r.url.endsWith('/api/config/validate')).length, validationCount);
        await fill('Как указать местоположение', 'в'); await insertRaw(' '); await insertRaw('цехе'); await stage();
        const submitted = JSON.parse(requests.filter(r => r.url.endsWith('/api/config/validate')).at(-1).body).configuration;
        assert.deepEqual(submitted.rooms.at(-1).speech, { mentions: ['цех'], locations: ['в цехе'] });
    }
    await click('Цех · редактировать'); await ready(); await fill('Как назвать зону', 'цех\nучасток'); await stage();
    await click('Создать зону'); await ready(); await fill('Название зоны', 'Другой участок'); await fill('Как назвать зону', 'цех'); await fill('Как указать местоположение', 'на другом участке'); await click('Добавить в план');
    await until(() => js('document.activeElement?.dataset.path?.endsWith("/speech/mentions")'), 'backend indexed alias error focuses list field');
    await click('Перейти к полю или сущности'); assert.equal(await js('document.activeElement.dataset.path.endsWith("/speech/mentions")'), true);
    await click('Отмена');

    await route('devices');
    for (const [name, alias, zone] of [['Измерительный блок', 'измерительный блок', 'Без зоны'], ['Вентилятор', 'вентилятор', 'Цех']]) { await click('Создать устройство'); await ready(); await fill('Название устройства', name); await fill('Как обращаться к устройству', alias); await insertRaw(' '); await select('Зона', zone); await stage(); }
    assert.equal((await api('/api/config')).hash, before.hash); assert.equal(hardware.length, 0);
    console.log('D/N: wizard, inline error, three viewports, isolated probe');
    await planDevice('Вентилятор', 'Настроить подключение'); await click('HTTP / REST →'); await fill('Название подключения', 'Контроллер цеха'); assert.match(await text(), /http:\/\/192\.168\.68\.120:8765/); assert.match(await text(), /порт 80/); await click('Далее');
    await until(() => js('document.activeElement?.getAttribute("data-path") === "/connection/http/base_url"'), 'inline error focus');
    for (const [width, height] of [[390, 844], [800, 600], [1280, 800]]) { await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false }); await delay(80); assert.equal(await js('document.querySelector("dialog").scrollWidth <= document.querySelector("dialog").clientWidth + 1'), true, 'task horizontal overflow'); await screenshot('wizard-error-' + width); }
    await fill('Endpoint', endpoint); await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 }); await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 }); await delay(100); assert.match(await text(), /ШАГ 2 ИЗ 5/, 'Enter cannot bypass wizard review'); await click('Проверить доступность'); await until(async () => (await text()).includes('TCP-соединение установлено'), 'probe'); assert.equal(hardware.length, 0); assert.ok(tcpConnections >= 1);
    await fill('Endpoint', endpoint + '/'); assert.match(await text(), /Результат проверки устарел/); await fill('Endpoint', endpoint);
    await click('Далее'); await check('Включить'); await check('Выключить');
    const firstMapping = '.mapping-editor:nth-of-type(1)';
    assert.equal(await js(`document.querySelector('${firstMapping} .inline-actions input').value`), '');
    assert.match(await text(), /Введите имя нового поля JSON/);
    await fill('Новое поле JSON', 'on', firstMapping); assert.match(await text(), /Такое поле уже существует/);
    assert.equal(await js(`Array.from(document.querySelectorAll('${firstMapping} button')).find(b => b.textContent === 'Добавить поле JSON').disabled`), true);
    await fill('Новое поле JSON', 'aux', firstMapping); await click('Добавить поле JSON', firstMapping);
    await js(`Array.from(document.querySelectorAll('${firstMapping} .body-row')).find(r => r.querySelector('strong').textContent === 'aux').querySelector('button').click()`);

    await fill('Путь', '/relay/1', '.mapping-editor:nth-of-type(1)'); await fill('Путь', '/relay/1', '.mapping-editor:nth-of-type(2)');
    await click('Далее'); await select('Чтение состояния', 'Есть независимый GET'); await fill('Путь', '/relay/1'); await click('Добавить поле Observe'); await fill('Top-level поле ответа', 'on'); await click('Далее'); assert.match(await text(), /Оборудование командами ещё не проверялось/); assert.match(await js('document.querySelector(".review-connection").innerText'), /Будут использовать подключение: Вентилятор/); await click('Сохранить и применить');
    await until(async () => (await text()).includes('Изменения относительно сохранённой'), 'full review');
    assert.equal((await api('/api/config')).hash, before.hash); assert.equal(hardware.length, 0);
    for (const name of ['Цех', 'Вентилятор', 'Измерительный блок', 'Контроллер цеха']) assert.ok((await text()).includes(name), 'all pending ' + name);
    const simpleReview = await js('document.querySelector("dialog").innerText');
    assert.match(simpleReview, /POST http:\/\/127\.0\.0\.1:/); assert.match(simpleReview, /on = Да/); assert.match(simpleReview, /on → Питание/);
    assert.doesNotMatch(simpleReview, /executor|timeout_ms|Timeout|Content-Type|application\/json|Backend пересчитает|Изменения добавлены в план/);
    await click('Назад к плану'); await js('document.querySelector("header input[type=checkbox]").click()'); await review();
    await js(`Array.from(document.querySelectorAll('.review-change')).find(n => n.querySelector('h4').textContent.startsWith('Настройка действия')).querySelector('summary').click()`);
    assert.match(await js('document.querySelector("dialog").innerText'), /"executor": "http"/);
    assert.match(await js('document.querySelector("dialog").innerText'), /"timeout_ms": 0/);
    await click('Назад к плану'); await js('document.querySelector("header input[type=checkbox]").click()'); await review();

    console.log('I/J: edit invalidates review; Cancel only discards current task');
    await click('Назад к плану'); await planDevice('Вентилятор', 'Редактировать'); await fill('Название устройства', 'Вентилятор цеха'); await stage();
    assert.equal(await js('[...document.querySelectorAll("button")].some(b => b.textContent.trim() === "Подтвердить применение" && !b.disabled)'), false);
    await planDevice('Вентилятор цеха', 'Настроить подключение'); await click('HTTP / REST →'); await fill('Название подключения', 'Отменённое имя'); await click('Отмена');
    await review(); assert.doesNotMatch(await text(), /Отменённое имя/); assert.match(await text(), /Вентилятор цеха/); await screenshot('review'); await apply();
    console.log('F/M: guarded ON/OFF exactly one POST, independent GET confirms');
    await route('devices'); await until(() => js('[...document.querySelectorAll(".device-card h2")].some(n => n.textContent.includes("Вентилятор цеха"))'), 'active device card'); await click('Вентилятор цеха');
    for (const [label, on] of [['Включить', true], ['Выключить', false]]) {
        const count = hardware.length; await click(label, '.operational');
        await until(() => js('!!document.querySelector("[data-knowledge=predicted]")'), 'Predicted');
        assert.equal(hardware.length, count + 1); assert.equal(hardware.at(-1).method, 'POST'); assert.equal(JSON.parse(hardware.at(-1).body).on, on);
        await click('Прочитать состояние', '.operational'); await until(() => js('!!document.querySelector("[data-knowledge=confirmed]")'), 'Confirmed');
        assert.equal(hardware.length, count + 2); assert.equal(hardware.at(-1).method, 'GET');
    }
    await screenshot('device-operational');
    console.log('K: shared connection consumers in review');
    await route('devices'); await planDevice('Измерительный блок', 'Настроить подключение'); await click('HTTP / REST →'); await select('Подключение', 'Контроллер цеха'); await click('Далее'); await check('Включить'); await fill('Путь', '/relay/2'); await click('Далее'); await click('Далее'); await click('Сохранить и применить'); await until(() => js('[...document.querySelectorAll("button")].some(b => b.textContent.trim() === "Подтвердить применение" && !b.disabled)'), 'shared binding review'); await apply();
    await route('connections'); await click('Редактировать общее подключение'); await click('HTTP / REST →'); await fill('Название подключения', 'Общий контроллер'); await click('Далее'); await select('Устройство (необязательно)', 'Вентилятор цеха'); await click('Далее'); assert.match(await text(), /Есть независимый GET/); assert.equal(await js('[...document.querySelectorAll("label")].find(l => l.firstChild?.textContent.trim() === "Путь *").querySelector("input").value'), '/relay/1'); await click('Далее'); await click('Сохранить и применить'); await until(async () => (await text()).includes('Затронуты устройства общего объекта'), 'shared consumers'); assert.match(await text(), /Измерительный блок/); assert.match(await text(), /Вентилятор цеха/); await apply();
    assert.equal((await api('/api/config')).configuration.observations.length, 1, 'Shared editor preserves selected device Observe');
    const beforeSeparate = (await api('/api/config')).configuration;
    await route('connections'); await click('Создать подключение'); await click('HTTP / REST →'); await fill('Название подключения', 'Резервный контроллер'); await fill('Endpoint', endpoint); await click('Далее'); await select('Устройство (необязательно)', 'Вентилятор цеха');
    assert.match(await text(), /Действие сохраняет отдельное подключение/);
    await click('Далее'); assert.match(await text(), /Чтение сохраняет отдельное подключение/); await click('Далее'); await click('Сохранить и применить'); await apply();
    const afterSeparate = (await api('/api/config')).configuration;
    assert.deepEqual(afterSeparate.bindings, beforeSeparate.bindings, 'Selecting another Connection cannot silently rebind actions');
    assert.deepEqual(afterSeparate.observations, beforeSeparate.observations, 'Selecting another Connection cannot silently rebind Observe');
    assert.equal(hardware.length, 4, 'configuration operations never execute');
    console.log('Typed capability editor: integer/number/enum/string, bounds, assignments and parameter');
    await route('devices'); await click('Создать устройство'); await ready(); await fill('Название устройства', 'Лабораторный стенд'); await fill('Как обращаться к устройству', 'лабораторный стенд');
    for (const [i, name, type] of [[2, 'Скорость', 'Целое число'], [3, 'Температура', 'Число'], [4, 'Режим', 'Выбор из списка'], [5, 'Метка', 'Текст']]) {
        await click('Добавить свойство'); const scope = 'dialog .task-content > article.form-section:nth-of-type(' + i + ')';
        await fill('Название свойства', name, scope); await select('Тип значения', type, scope);
        if (i === 2) await fill('Единица измерения', 'об/мин', scope);
        if (i === 3) { await fill('Минимум', '-20', scope); await fill('Максимум', '120', scope); await fill('Единица измерения', '°C', scope); }
        if (i === 4) { await fill('Варианты', 'normal\neco', scope); await insertRaw(' '); }
        if (i === 5) await fill('Максимальная длина', '64', scope);
    }
    await click('Добавить действие'); const actionScope = 'dialog .action-editor:last-of-type';
    await fill('Название действия', 'Установить скорость', actionScope); await fill('Фразы действия', 'скорость', actionScope); await insertRaw(' '); await click('Добавить параметр', actionScope); await fill('Название свойства', 'Скорость', actionScope); await click('Добавить результат', actionScope); await select('Свойство', 'Скорость', actionScope); await select('Источник значения', 'Параметр команды', actionScope); await stage(); await review(); await apply();
    const typed = (await api('/api/config')).configuration.devices.find(d => d.name === 'Лабораторный стенд'); assert.deepEqual(typed.state.map(s => s.type), ['boolean', 'integer', 'number', 'enum', 'string']);
    console.log('E: unfinished imported B blocks full review; scoped probe A still works');
    await route('advanced'); const beforeImport = await api('/api/config'); const invalid = structuredClone(beforeImport.configuration); invalid.connections.push({ id: 'connection_unfinished', name: 'Незавершённое B', type: 'http', enabled: true, timeout_ms: 2000, http: { base_url: '' } });
    await fill('Документ кандидата', JSON.stringify(invalid)); await click('Проверить импорт и добавить в план'); await until(() => js('!!document.querySelector("main section [role=alert]")'), 'inline import validation'); assert.equal(await js('document.querySelectorAll("main .task-errors .text-button").length'), 0, 'no dead navigation for unaddressable imported JSON error');
    await route('connections'); await click('Редактировать общее подключение'); await click('HTTP / REST →'); await click('Проверить доступность'); await until(async () => (await text()).includes('TCP-соединение установлено'), 'probe A despite B'); await click('Отмена');
    await click('Сохранить и применить', 'main .manual-toolbar'); await until(async () => (await text()).includes('Непроверенный импорт блокирует'), 'unfinished import blocks review'); assert.equal(await js('[...document.querySelectorAll("button")].some(b => b.textContent.trim() === "Подтвердить применение" && !b.disabled)'), false); await click('Перейти к полю или сущности'); await until(() => js('location.hash === "#/advanced" && !document.querySelector("dialog[open]")'), 'blocking import navigation'); assert.equal((await api('/api/config')).hash, beforeImport.hash);
    await route('advanced'); assert.match(await js('document.querySelector("main textarea").value'), /connection_unfinished/); await click('Отбросить импорт');
    const imported = structuredClone(beforeImport.configuration); imported.system.name = 'Ручной импорт стенда'; await fill('Документ кандидата', JSON.stringify(imported)); await click('Проверить импорт и добавить в план'); await until(async () => (await text()).includes('Кандидат принят в план'), 'import candidate'); assert.equal((await api('/api/config')).hash, beforeImport.hash); await review(); await apply();
    await route('advanced'); await click('Экспорт сохранённой конфигурации'); await click('Экспорт локального плана');
    await js('[...document.querySelectorAll("main button")].find(b => b.textContent === "Восстановить как кандидат").click()'); await until(async () => (await text()).includes('Кандидат принят в план'), 'restore candidate');
    await click('Перечитать основу и отбросить локальный план'); await until(() => js('!document.querySelector("dialog[open]")'), 'reset candidate'); await click('Reconcile хранилища'); await until(async () => (await text()).includes('Reconcile завершён'), 'recovery');
    console.log('Playground Preview and Mock keep hardware isolated; Production is explicit');
    await js(`document.querySelector('a[href$="#/advanced/playground"]').click()`); await delay(100);
    await fill('Команда', 'выключи вентилятор в цехе'); await click('Preview'); await until(async () => (await text()).includes('Preview · без исполнения'), 'preview'); await click('Mock Execute'); await until(async () => (await text()).includes('Mock Execute · виртуальное состояние'), 'mock'); assert.equal(hardware.length, 4);
    await click('Production Execute · оборудование'); await until(() => hardware.length === 5, 'explicit production playground'); assert.equal(hardware.at(-1).method, 'POST'); assert.equal(JSON.parse(hardware.at(-1).body).on, false);
    console.log('L: Save success / Apply failure from C++ fixture; no blind retry');
    await route('overview'); await click('Изменить название системы'); await fill('Название системы', 'Сохранённый новый стенд'); await stage(); await review();
    const writesBefore = requests.filter(r => r.method === 'PUT' || r.url.endsWith('/api/config/apply')).length;
    const activeBefore = (await api('/api/config/active')).active.hash;
    fs.writeFileSync(failFile, 'fail only Apply'); await click('Подтвердить применение'); await until(async () => (await text()).includes('Версия сохранена, но не активирована'), 'saved not active');
    await delay(1000); assert.equal(requests.filter(r => r.method === 'PUT' || r.url.endsWith('/api/config/apply')).length, writesBefore + 2);
    assert.equal((await api('/api/config/active')).active.hash, activeBefore); assert.notEqual((await api('/api/config')).hash, activeBefore);
    await screenshot('saved-not-active'); assert.equal(hardware.length, 5); assert.deepEqual(external, []); assert.deepEqual(exceptions, []);
    fs.writeFileSync(path.join(artifacts, 'acceptance.json'), JSON.stringify({ passed: true, requests, hardware, tcpConnections, url }, null, 2));
    console.log('PASS B2 visible manual controls, task/review/CAS lifecycle, shared consumers, exact hardware counts, partial Apply, offline and responsive UI');
})().catch(async error => { console.error(error); if (socket) { try { console.error((await text()).slice(-12000)); await screenshot('failure'); } catch {} } process.exitCode = 1; }).finally(async () => {
    if (socket) socket.close(); mock.closeAllConnections(); await new Promise(resolve => mock.close(resolve));
    for (const child of [chrome, server]) { if (!child || child.exitCode !== null || child.signalCode !== null) continue; const done = new Promise(resolve => child.once('exit', resolve)); const timer = setTimeout(() => { child.kill('SIGKILL'); process.exitCode = 1; }, 10000); child.kill('SIGTERM'); await done; clearTimeout(timer); }
    fs.closeSync(log); if (!process.exitCode) fs.rmSync(temp, { recursive: true, force: true });
});
