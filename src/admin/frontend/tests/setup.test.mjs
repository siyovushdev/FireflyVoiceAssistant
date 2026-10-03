import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { collectFacts, emptyFacts, missingFact, physicalInput, verifyConnectivity, verifySemantic, applyLanguage } from '../.test-build/adapters/setup.js';
import { clone, powerOnly } from '../.test-build/adapters/editing.js';
const base = JSON.parse(readFileSync(new URL('../../../../config/empty-home.json', import.meta.url)));
function fixture() {
    const c = clone(base); const d = { id: 'fan', name: 'Вентилятор', kind: 'generic', room_id: null, enabled: true, spoken_aliases: ['вентилятор'], state: [], action_ids: [] }; c.devices.push(d); powerOnly(c, d); return c;
}
test('explicit facts accumulate without guesses; dotted IPv4 and JSON use physical lane intact', () => {
    let f = collectFacts(emptyFacts(), 'Подключи насос к контроллеру.');
    assert.deepEqual(f, emptyFacts()); assert.equal(missingFact(f).key, 'endpoint');
    f = collectFacts(f, 'как обычно'); assert.deepEqual(f, emptyFacts());
    f = collectFacts(f, '127.0.0.1:8765'); assert.equal(f.endpoint, '', 'protocol must be explicit');
    f = collectFacts(f, 'HTTP http://127.0.0.1:8765. Включить POST /relay/1 {"on":true}. Выключить POST /relay/1 {"on":false}. Состояние GET /relay/1, поле on.');
    assert.equal(f.endpoint, 'http://127.0.0.1:8765'); assert.equal(f.on.body.on, true); assert.equal(f.off.body.on, false); assert.equal(f.field, 'on'); assert.equal(missingFact(f), null);
    assert.ok(physicalInput('127.0.0.1:8765')); assert.ok(physicalInput('POST /relay/1 {"on":true}'));
    assert.equal(missingFact(collectFacts({ ...f, observe: '', field: '' }, 'без чтения')), null);
});
function physical() {
    const before = fixture(), after = clone(before);
    const facts = collectFacts(emptyFacts(), 'http://127.0.0.1:8765 Включить POST /relay/1 {"on":true}. Выключить POST /relay/1 {"on":false}. Состояние GET /relay/1, поле on.');
    after.connections = [{ id: 'controller', name: 'Контроллер', type: 'http', enabled: true, timeout_ms: 2000, http: { base_url: facts.endpoint, base_path: '' } }];
    after.bindings = before.actions.map(a => ({ target_id: 'fan', action_id: a.id, connection_id: 'controller', executor: 'http', http: { method: 'POST', path: '/relay/1', body: { on: a.assignments[0].value }, expected_status: 200 } }));
    after.observations = [{ target_id: 'fan', connection_id: 'controller', executor: 'http', http: { method: 'GET', path: '/relay/1', expected_status: 200 }, fields: { on: 'power' } }];
    return { before, after, facts, proposal: { configuration: after, operations: [{ op: 'set_http_connection' }, { op: 'set_connection_binding' }, { op: 'set_http_observe' }] } };
}
test('connectivity proof rejects invented facts, mixed and unknown operations, wrong target and coercion', () => {
    const x = physical(); assert.deepEqual(verifyConnectivity(x.before, x.proposal, 'fan', x.facts, 'separate'), x.after);
    for (const mutate of [p => p.configuration.connections[0].http.base_url = 'http://192.168.1.1', p => p.configuration.bindings[0].http.path = '/on', p => p.configuration.bindings[0].http.body.on = 'true', p => p.configuration.bindings[0].target_id = 'other', p => p.configuration.observations[0].fields = { nested: 'power' }, p => p.operations.push({ op: 'create_device' }), p => p.operations.push({ op: 'unknown' }), p => p.configuration.devices[0].name = 'Changed']) {
        const p = clone(x.proposal); mutate(p); assert.throws(() => verifyConnectivity(x.before, p, 'fan', x.facts, 'separate'));
    }
    assert.throws(() => verifySemantic(x.before, x.after));
});
test('contextual AI gives editable values, preserves capabilities; checkpoint is immutable', () => {
    const original = { id: 'zone', name: 'Цех', speech: { mentions: [], locations: [] }, metadata: { description: '' } };
    const next = applyLanguage(original, { description: 'Производственная зона', aliases: ['цех'], mentions: ['цех', 'цехе'], locations: ['в цехе'] }, 'location');
    assert.deepEqual(original.speech.mentions, []); assert.deepEqual(next.speech.mentions, ['цех', 'цехе']); next.speech.mentions.push('участок');
    const device = fixture().devices[0]; const result = applyLanguage(device, { aliases: ['вентилятор'], description: 'Вентиляция', capabilities: ['invented'] }, 'device');
    assert.deepEqual(result.action_ids, device.action_ids); assert.equal(result.capabilities, undefined);
    assert.deepEqual(applyLanguage({ name: 'Включить' }, { aliases: ['включи'] }, 'action').speech_aliases, ['включи']);
});
test('coordinator opening is read-only; semantic proposal reject/accept never writes stored or executes', async () => {
    const { manual } = await import('../.test-build/stores/manual.js');
    const s = await import('../.test-build/stores/setup.js');
    const memory = new Map(); globalThis.sessionStorage = { getItem: k => memory.get(k) ?? null, setItem: (k,v) => memory.set(k,v), removeItem: k => memory.delete(k) };
    let state = { ok: true, active: false, status: 'idle' }; const calls = [];
    const catalog = { ok: true, hash: 'base', revision: 1, configuration: base };
    const originalFetch = globalThis.fetch;
    globalThis.fetch = async (path, options = {}) => {
        calls.push({path, body: options.body && JSON.parse(options.body)}); const b = options.body ? JSON.parse(options.body) : {};
        let reply;
        if (path === '/api/setup/status') reply = state;
        else if (path === '/api/config') reply = catalog;
        else if (path === '/api/setup/start') reply = state = { ok: true, active: true, status: 'editing', version: 0, configuration: base, session_id: 'owner' };
        else if (path === '/api/setup/heartbeat') reply = state;
        else if (path === '/api/setup/message') reply = state = { ...state, version: state.version + 1, configuration: fixture() };
        else if (path === '/api/setup/draft') reply = state = { ...state, version: state.version + 1, configuration: b.configuration };
        else if (path === '/api/config/validate') reply = { ok: true, apply_ready: true };
        else throw Error('Unexpected request ' + path);
        return new Response(JSON.stringify(reply), { status: 200 });
    };
    try {
        await s.restoreSession(); assert.deepEqual(calls.map(c => c.path), ['/api/setup/status']);
        await s.startSetup(); s.composer.value = 'Добавь устройство без зоны'; await s.sendMessage();
        assert.equal(s.proposal.value.after.devices.length, 1); assert.equal(manual.candidate.value.devices.length, 0); assert.equal(s.ready.value, false);
        await s.rejectProposal(); assert.equal(state.configuration.devices.length, 0);
        s.composer.value = 'Добавь устройство без зоны'; await s.sendMessage(); await s.acceptProposal();
        assert.equal(manual.candidate.value.devices.length, 1); assert.equal(s.ready.value, true);
        assert.equal(calls.filter(c => /apply|command|poll|execute/.test(c.path)).length, 0);
        assert.equal(calls.filter(c => c.path === '/api/config' && c.body).length, 0);
        manual.review.value = {version:manual.version.value};
        state = {...state,version:state.version+1};
        await s.refreshSession();
        assert.equal(manual.review.value,null,'server version change invalidates review even if configuration was restored');
    } finally { globalThis.fetch = originalFetch; }
});
test('manual final review in owned setup uses setup Apply only; no direct CAS bypass', async () => {
    const { createManualStore } = await import('../.test-build/stores/manual.js');
    let stored = { ok:true, revision:1, hash:'old', configuration:fixture() }, active = clone(stored), version = 0;
    const calls = []; let inSetup = true;
    const api = async (path, body) => { calls.push({path,body}); if (path === '/api/setup/status') return {ok:true,active:inSetup,status:'editing'}; if (path === '/api/config/validate') return {ok:true,valid:true,apply_ready:false,runtime_preflight:'unavailable'}; if (path === '/api/config/diff') return {ok:true,changes:[]}; throw Error('Forbidden direct mutation '+path); };
    const read = async path => path === '/api/config' ? stored : {ok:true,active};
    const m = createManualStore(api, read, async () => {});
    m.connectSetup({ owns: () => true, ready: () => true, sync: async c => c, apply: async () => {version++; stored={...stored,revision:2,hash:'new'};active=clone(stored);inSetup=false;} });
    await m.prepare(); assert.equal(m.canConfirm.value,true,'setup owns deferred preflight'); await m.confirm();
    assert.equal(version,1); assert.equal(calls.filter(c => ['/api/config','/api/config/apply'].includes(c.path)).length,0);
});
