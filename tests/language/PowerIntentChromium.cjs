'use strict';
// Visible Preview controls against the real isolated C++ HTTP fixture. No API
// interception, fake inference, execution controls, services or host devices.
const fs = require('node:fs'), path = require('node:path');
const { spawn } = require('node:child_process');
const assert = require('node:assert/strict');
const [url, chromium, output] = process.argv.slice(2);
fs.mkdirSync(output, { mode: 0o700 });
const log = fs.openSync(path.join(output, 'chrome.log'), 'w');
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let chrome, socket, id = 0;
const pending = new Map(), errors = [], external = [], requests = [], checks = [];
async function until(test, label) {
    const end = Date.now() + 45000;
    while (Date.now() < end) { if (await test()) return; await pause(75); }
    throw Error('Timeout: ' + label);
}
function send(method, params = {}) {
    return new Promise((resolve, reject) => {
        const n = ++id; pending.set(n, { resolve, reject });
        socket.send(JSON.stringify({ id: n, method, params }));
    });
}
async function js(expression) {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw Error(JSON.stringify(r.exceptionDetails));
    return r.result.value;
}
const body = () => js('document.body?.innerText ?? ""');
(async () => {
    try {
        chrome = spawn(chromium, ['--headless=new', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage',
            '--disable-background-networking', '--no-first-run', '--no-default-browser-check',
            '--remote-debugging-port=0', '--user-data-dir=' + path.join(output, 'profile'), 'about:blank'],
            { stdio: ['ignore', log, log] });
        const portFile = path.join(output, 'profile/DevToolsActivePort');
        await until(() => fs.existsSync(portFile), 'Chromium');
        const port = fs.readFileSync(portFile, 'utf8').split('\n')[0];
        const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
        socket = new WebSocket(pages.find(p => p.type === 'page').webSocketDebuggerUrl);
        await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
        socket.onmessage = event => {
            const m = JSON.parse(event.data);
            if (m.id) {
                const p = pending.get(m.id); pending.delete(m.id);
                if (p) m.error ? p.reject(m.error) : p.resolve(m.result);
            } else if (m.method === 'Runtime.exceptionThrown') errors.push(m.params);
            else if (m.method === 'Fetch.requestPaused') {
                const { request, requestId } = m.params;
                requests.push({ url: request.url, method: request.method, body: request.postData });
                const safe = request.url.startsWith(url + '/');
                if (!safe) external.push(request.url);
                send(safe ? 'Fetch.continueRequest' : 'Fetch.failRequest', safe ? { requestId } :
                    { requestId, errorReason: 'BlockedByClient' });
            }
        };
        await send('Runtime.enable'); await send('Page.enable');
        await send('Fetch.enable', { patterns: [{ urlPattern: '*' }] });
        await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 850, deviceScaleFactor: 1, mobile: false });
        await send('Page.navigate', { url: url + '/#/playground' });
        await until(async () => (await body()).includes('Command Playground'), 'Playground');
        for (const [text, route, action] of [
            ['вырубай свет на кухне', 'Qwen · определение намерения', 'Выключить'],
            ['включи свет на кухне', 'Правила · знакомая команда', 'Включить'],
            ['не выключай свет на кухне', 'Отклонено до вызова Qwen', null]]) {
            await js(`(() => { const t = document.querySelector('main textarea'); t.value = ${JSON.stringify(text)};
                t.dispatchEvent(new Event('input', {bubbles:true})); })()`);
            await until(() => js(`[...document.querySelectorAll('main button')].some(b => b.textContent.trim() === 'Preview' && !b.disabled)`), 'Preview enabled');
            await js(`[...document.querySelectorAll('main button')].find(b => b.textContent.trim() === 'Preview').click()`);
            await until(() => js(`document.querySelector('[aria-label="Разбор команды"] strong')?.textContent === ${JSON.stringify(text)}`), 'original phrase visible');
            const trace = await js('document.querySelector(\'[aria-label="Разбор команды"]\').innerText');
            assert.ok(trace.includes(route), trace);
            if (action) assert.ok(trace.includes(action), trace);
            else assert.ok(trace.includes('Причина:'), trace);
            assert.ok(!trace.includes('Исполнитель принял команду'), 'Preview must not claim execution');
            checks.push({ text, trace });
            fs.writeFileSync(path.join(output, `preview-${checks.length}.png`), Buffer.from((await send('Page.captureScreenshot')).data, 'base64'));
        }
        await js('location.hash = "#/diagnostics"');
        await until(() => js('!!document.querySelector(\'[aria-label="Разбор команды"]\')'), 'diagnostics trace');
        assert.ok((await body()).includes('не выключай свет на кухне'));
        assert.equal(requests.filter(r => r.method !== 'GET' && !r.url.endsWith('/api/test-command')).length, 0);
        assert.ok(requests.filter(r => r.url.endsWith('/api/test-command')).every(r => JSON.parse(r.body).execute === false));
        assert.deepEqual(external, []); assert.deepEqual(errors, []);
        fs.writeFileSync(path.join(output, 'result.json'), JSON.stringify({ status: 'PASS', checks, requests,
            exceptions: errors, external_requests: external, real_api: true, execution_requested: false }, null, 2));
        console.log('PASS Chromium: visible Preview rules/Qwen/refusal and Diagnostics, no execution or config writes');
    } finally {
        if (socket) socket.close();
        if (chrome && chrome.exitCode === null) {
            const ended = new Promise(resolve => chrome.once('exit', resolve));
            chrome.kill('SIGTERM');
            const timer = setTimeout(() => chrome.kill('SIGKILL'), 5000);
            await ended; clearTimeout(timer);
        }
        fs.closeSync(log);
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
