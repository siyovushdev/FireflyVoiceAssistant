'use strict';
// Production Vite bundle, actual C++ HTTP/API/store/runtime; only inference is
// stubbed by the existing worker. All mutations below are test fixture setup,
// never browser/UI requests, and operate on a private temporary store.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const { spawn } = require('node:child_process');
const assert = require('node:assert/strict');
const [app, worker, root, chromium, artifactDir] = process.argv.slice(2);
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'firefly-v2-'));
fs.mkdirSync(path.join(temp, 'store'), { mode: 0o700 });
fs.mkdirSync(artifactDir, { recursive: true });
const log = fs.openSync(path.join(artifactDir, 'server.log'), 'w');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
let server, chrome, socket, output = '', nextId = 0;
const pending = new Map(), browserRequests = [], blocked = [], exceptions = [], badResponses = [];
let hardwareRequests = 0;
const mock = http.createServer((req, res) => {
    hardwareRequests++;
    req.resume();
    req.on('end', () => { const body = '{"on":true}'; res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': body.length }); res.end(body); });
});
async function until(check, label, timeout = 20000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) { if (await check()) return; await delay(100); }
    throw Error('Timeout: ' + label);
}
function send(method, params = {}) {
    return new Promise((resolve, reject) => {
        const id = ++nextId;
        pending.set(id, { resolve, reject });
        socket.send(JSON.stringify({ id, method, params }));
    });
}
async function js(expression) {
    const reply = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (reply.exceptionDetails) throw Error(JSON.stringify(reply.exceptionDetails));
    return reply.result.value;
}
async function text() { return js('document.querySelector("main")?.innerText || ""'); }
async function route(name) {
    await js(`document.querySelector('nav a[href$="#/${name}"]').click()`);
    await until(() => js(`location.hash === '#/${name}'`), 'route ' + name);
    await delay(50);
}

(async () => {
    await new Promise(resolve => mock.listen(0, '127.0.0.1', resolve));
    const config = JSON.parse(fs.readFileSync(path.join(root, 'tests/admin/v2/system.json')));
    config.connections = [{ id: 'connection_mock', name: 'Контроллер участка', type: 'http', enabled: true, timeout_ms: 2000, http: { base_url: `http://127.0.0.1:${mock.address().port}`, base_path: '' } }];
    config.bindings = config.devices.slice(0, 3).flatMap(device => ['on', 'off'].map(action => ({
        target_id: device.id, action_id: 'action_' + action, connection_id: 'connection_mock', executor: 'http',
        http: { method: 'POST', path: '/relay/1', body: { on: action === 'on' }, expected_status: 200 }
    })));
    config.observations = [{ target_id: 'device_fan', connection_id: 'connection_mock', executor: 'http', http: { method: 'GET', path: '/relay/1', expected_status: 200 }, fields: { on: 'power' } }];
    const fixture = path.join(temp, 'fixture.json');
    fs.writeFileSync(fixture, JSON.stringify(config));
    server = spawn(app, ['--store', path.join(temp, 'store'), '--init-config', fixture, '--runtime-worker', worker, '--admin-port', '0'], { stdio: ['ignore', 'pipe', log], env: { ...process.env, PATH: '/nonexistent' } });
    server.stdout.on('data', bytes => { output += bytes; });
    await until(() => output.includes('ADMIN_LISTEN'), 'C++ listener');
    const url = output.match(/ADMIN_LISTEN (http:\/\/[^\s]+)/)[1];
    const api = async (endpoint, body, method = body ? 'POST' : 'GET') => {
        const response = await fetch(url + endpoint, { method, headers: { 'Content-Type': 'application/json', 'X-Firefly-Admin': '1' }, body: body ? JSON.stringify(body) : undefined });
        const result = await response.json();
        assert.equal(response.status, 200, JSON.stringify(result));
        assert.notEqual(result.ok, false, JSON.stringify(result));
        return result;
    };
    await until(async () => (await api('/api/status')).runtime_ready, 'runtime initialized');
    const active = (await api('/api/config/active')).active;
    await api('/api/devices/observe', { target_id: 'device_pump', properties: { power: false } });
    const evidenceStarted = Date.now();
    await api('/api/devices/poll', { target_id: 'device_fan', expected_hash: active.hash });
    const predicted = await api('/api/test-command', { text: 'Включи сигнальную лампу на складе', execute: true, production: true });
    assert.equal(predicted.execution?.success, true, JSON.stringify(predicted));
    assert.equal(predicted.safety, 'PASS');
    const stored = await api('/api/config');
    const candidate = structuredClone(stored.configuration);
    candidate.system.name = 'Сохранённая, ещё не активная система';
    candidate.devices.at(-1).name = 'Неактивное переименование';
    const saved = await api('/api/config', { configuration: candidate, expected: { revision: stored.revision, hash: stored.hash } }, 'PUT');
    assert.equal((await api('/api/config/active')).active.hash, active.hash);
    const beforeHardware = hardwareRequests;
    assert.equal(beforeHardware, 2);
    const beforeState = await api('/api/devices/state');
    const beforeHistory = await api('/api/executions');

    // Old UI and transport policies still apply. No API/static SPA fallback.
    const vanilla = await fetch(url + '/');
    assert.equal(vanilla.status, 200);
    assert.match(await vanilla.text(), /admin\.js/);
    const entry = await fetch(url + '/v2/');
    assert.equal(entry.status, 200);
    assert.equal(entry.headers.get('cache-control'), 'no-store');
    assert.equal(entry.headers.get('x-content-type-options'), 'nosniff');
    assert.match(entry.headers.get('content-security-policy'), /script-src 'self'/);
    const html = await entry.text();
    assert.match(html, /type="module"/);
    for (const [, asset] of html.matchAll(/(?:src|href)="(\/v2\/assets\/[^\"]+)"/g)) {
        const response = await fetch(url + asset);
        assert.equal(response.status, 200);
        assert.match(response.headers.get('content-type'), asset.endsWith('.css') ? /text\/css/ : asset.endsWith('.svg') ? /image\/svg\+xml/ : /text\/javascript/);
        assert.equal(response.headers.get('cache-control'), 'no-store');
    }
    for (const endpoint of ['/v2/no-such-route', '/v2/assets/', '/v2/firefly-assets.json', '/api/no-such-route', '/v2/assets/not-declared.js']) assert.equal((await fetch(url + endpoint)).status, 404, endpoint);
    assert.equal((await fetch(url + '/v2/?cache=1')).status, 400);
    assert.equal((await fetch(url + '/api/config', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: '{}' })).status, 403);
    assert.equal((await fetch(url + '/api/config', { method: 'PUT', headers: { 'Content-Type': 'application/json', 'X-Firefly-Admin': '1', Origin: 'http://other.invalid' }, body: '{}' })).status, 403);

    chrome = spawn(chromium, ['--ozone-platform=x11', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--disable-background-networking', '--no-first-run', '--remote-debugging-port=0', '--user-data-dir=' + path.join(temp, 'chrome'), 'about:blank'], { stdio: ['ignore', log, log] });
    const portFile = path.join(temp, 'chrome', 'DevToolsActivePort');
    await until(() => fs.existsSync(portFile), 'Chromium');
    const port = fs.readFileSync(portFile, 'utf8').split('\n')[0];
    const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    socket = new WebSocket(pages.find(page => page.type === 'page').webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
    socket.onmessage = event => {
        const message = JSON.parse(event.data);
        if (message.id) {
            const promise = pending.get(message.id); pending.delete(message.id);
            if (promise) message.error ? promise.reject(message.error) : promise.resolve(message.result);
        } else if (message.method === 'Runtime.exceptionThrown') exceptions.push(message.params);
        else if (message.method === 'Network.responseReceived' && message.params.response.status >= 400) badResponses.push(message.params.response);
        else if (message.method === 'Fetch.requestPaused') {
            const { requestId, request } = message.params;
            browserRequests.push({ url: request.url, method: request.method });
            const safe = request.url.startsWith(url + '/') && request.method === 'GET';
            if (!safe) blocked.push(request);
            send(safe ? 'Fetch.continueRequest' : 'Fetch.failRequest', safe ? { requestId } : { requestId, errorReason: 'BlockedByClient' }).catch(error => exceptions.push(String(error)));
        }
    };
    await send('Runtime.enable'); await send('Page.enable'); await send('Network.enable');
    await send('Fetch.enable', { patterns: [{ urlPattern: '*' }] });
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: url + '/v2/' });
    await until(async () => (await text()).includes('Вентилятор'), 'V2 overview real catalog');
    assert.equal(await js('location.hash'), '#/overview');
    assert.match(await text(), /сохранённые, но не применённые/);
    assert.match(await js('document.querySelector("header").innerText'), /Производственный участок/);
    assert.doesNotMatch(await text(), /Неактивное переименование/);
    assert.match(await text(), /Команда принята/);
    await route('zones');
    for (const name of ['Цех', 'Склад', 'Лабораторный стенд', 'Без зоны']) assert.ok((await text()).includes(name), name);
    await route('devices');
    await until(async () => (await text()).includes('Измерительный блок'), 'Devices data');
    assert.match(await text(), /Без зоны/);
    assert.equal(await js('document.querySelectorAll(".device-card").length'), 4);
    await js('document.querySelector(".filters select").value = "none"; document.querySelector(".filters select").dispatchEvent(new Event("change", {bubbles:true}))');
    await until(() => js('document.querySelectorAll(".device-card").length === 1'), 'No-zone filter');
    await js('document.querySelector(".device-card h2 a").click()');
    await until(() => js('location.hash === "#/devices/device_bench"'), 'Device detail');
    assert.match(await text(), /Измерительный блок/);
    assert.match(await text(), /Состояние неизвестно/);
    assert.doesNotMatch(await text(), /Подтверждено устройством/);
    const beforeReload = await js('performance.timeOrigin');
    await send('Page.reload', { ignoreCache: true });
    await until(async () => await js('performance.timeOrigin') !== beforeReload
        && (await text()).includes('Измерительный блок'), 'Deep-link reload in new document');
    assert.equal(await js('location.hash'), '#/devices/device_bench');
    await route('states');
    const knowledge = await js(`Object.fromEntries([...document.querySelectorAll('.state-row')].map(row => [row.dataset.deviceId, row.querySelector('.property')?.dataset.knowledge]))`);
    assert.deepEqual(knowledge, { device_fan: 'confirmed', device_lamp: 'predicted', device_pump: 'confirmed', device_bench: 'unknown' });
    await route('setup'); assert.match(await text(), /Будет в следующем этапе/);
    for (const name of ['connections', 'advanced', 'diagnostics']) { await route(name); assert.doesNotMatch(await text(), /Будет в следующем этапе/); }
    await route('overview');
    for (const [width, height] of [[1280, 800], [800, 600], [390, 844]]) {
        await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
        await delay(100);
        assert.equal(await js('document.documentElement.scrollWidth <= window.innerWidth'), true, 'No horizontal scroll at ' + width);
        const screenshot = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true });
        fs.writeFileSync(path.join(artifactDir, `overview-${width}.png`), Buffer.from(screenshot.data, 'base64'));
    }
    await js('document.querySelector("button.refresh").click()');
    await until(() => js('!document.querySelector("button.refresh").disabled'), 'Manual read refresh');
    for (const endpoint of ['/api/status', '/api/config', '/api/config/active', '/api/devices/state', '/api/executions']) assert.ok(browserRequests.some(request => request.url === url + endpoint), 'Real API read ' + endpoint);
    assert.deepEqual(blocked, [], 'No mutations or external browser requests');
    assert.deepEqual(exceptions, [], 'No JS exceptions');
    assert.deepEqual(badResponses, [], 'No missing assets/API errors');
    // Public API does not admit caller timestamps. Wait only after all basic
    // acceptance checks, then test the real 60s TTL without changing clocks.
    console.log('Checking real backend evidence TTL; no NPU or user store involved.');
    await delay(Math.max(0, evidenceStarted + 61000 - Date.now()));
    await route('states');
    await js('document.querySelector("button.refresh").click()');
    await until(() => js('document.querySelector("[data-device-id=device_pump] .property")?.dataset.knowledge === "stale"'), 'Backend Stale evidence');
    assert.match(await text(), /Данные устарели/);
    assert.deepEqual(blocked, [], 'Refresh also remains read-only/offline');
    assert.deepEqual(exceptions, [], 'No late exceptions');
    assert.equal(hardwareRequests, beforeHardware, 'Read-only V2 causes no hardware I/O');
    assert.equal((await api('/api/config')).hash, saved.hash, 'Stored unchanged');
    assert.equal((await api('/api/config/active')).active.hash, active.hash, 'Active unchanged');
    const withoutAge = value => JSON.parse(JSON.stringify(value, (key, item) => key === 'stale' ? undefined : item));
    assert.deepEqual(withoutAge((await api('/api/devices/state')).devices), withoutAge(beforeState.devices), 'State evidence unchanged; only computed age advances');
    assert.deepEqual((await api('/api/executions')).executions, beforeHistory.executions, 'History unchanged');
    fs.writeFileSync(path.join(artifactDir, 'acceptance.json'), JSON.stringify({ passed: true, url: url + '/v2/#/overview', browserRequests, hardwareRequests, knowledge, stored: saved.hash, active: active.hash }, null, 2));
    console.log('PASS Admin V2 Chromium: C++ production dist, vanilla /, all hash routes/reload, real API, stored/active isolation, non-home/no-zone, all evidence states, 3 viewports, no mutations/external requests/hardware I/O');
})().catch(error => {
    console.error(error);
    console.error(fs.readFileSync(path.join(artifactDir, 'server.log'), 'utf8').slice(-4000));
    process.exitCode = 1;
}).finally(async () => {
    if (socket) socket.close();
    mock.closeAllConnections(); await new Promise(resolve => mock.close(resolve));
    for (const child of [chrome, server]) {
        if (!child || child.exitCode !== null || child.signalCode !== null) continue;
        const exited = new Promise(resolve => child.once('exit', resolve));
        const timeout = setTimeout(() => { child.kill('SIGKILL'); process.exitCode = 1; }, 10000);
        child.kill('SIGTERM');
        await exited;
        clearTimeout(timeout);
    }
    fs.closeSync(log);
    if (!process.exitCode) fs.rmSync(temp, { recursive: true, force: true });
});
