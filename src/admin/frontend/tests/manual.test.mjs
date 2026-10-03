import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createManualStore } from '../.test-build/stores/manual.js';
import { AdminError } from '../.test-build/api/admin.js';
import { clone, readableDiff, powerOnly } from '../.test-build/adapters/editing.js';
import { commandIntent } from '../.test-build/adapters/commands.js';
const fixture = JSON.parse(readFileSync(new URL('../../../../tests/admin/v2/system.json', import.meta.url)));
function harness() {
    const state = { stored: { ok: true, configuration: clone(fixture), revision: 1, hash: 'a'.repeat(64) }, active: null, calls: [], failApply: false, lostSave: false, setupActive: false };
    state.active = clone(state.stored);
    const api = async (path, body, method) => {
        state.calls.push({ path, body, method });
        if (path === '/api/setup/status') return { ok: true, active: state.setupActive, status: 'idle' };
        if (path === '/api/config/validate') return { ok: true, apply_ready: true };
        if (path === '/api/config/diff') return { ok: true, changes: [] };
        if (path === '/api/config' && method === 'PUT') {
            assert.equal(body.expected.hash, state.stored.hash);
            state.stored = { ok: true, configuration: clone(body.configuration), revision: 2, hash: 'b'.repeat(64) };
            if (state.lostSave) throw new AdminError([{ code: 'network_unknown', path: '', message: 'lost response' }], true);
            return state.stored;
        }
        if (path === '/api/config/apply') {
            assert.equal(body.expected.hash, state.stored.hash);
            if (state.failApply) throw new AdminError([{ code: 'apply_failed', path: '', message: 'fixture failure' }]);
            state.active = clone(state.stored); return { ok: true };
        }
        throw Error('Unexpected endpoint ' + path);
    };
    const read = async path => path === '/api/config' ? state.stored : { ok: true, active: state.active };
    state.store = createManualStore(api, read, async () => {});
    return state;
}
const writes = state => state.calls.filter(c => c.method === 'PUT' || c.path === '/api/config/apply');

test('incomplete TaskBuffer stays isolated; Cancel retains previously staged task and unknown fields', async () => {
    const h = harness(); h.stored.configuration.extension = { rare: 'retained' };
    await h.store.begin('system'); h.store.task.value.buffer.system.name = 'Цех';
    assert.equal(h.store.candidate.value.system.name, fixture.system.name);
    assert.equal(await h.store.stage(), true);
    const previous = clone(h.store.candidate.value);
    await h.store.begin('zone'); h.store.task.value.buffer.rooms.at(-1).name = '';
    h.store.cancel(); assert.deepEqual(h.store.candidate.value, previous);
    assert.equal(h.store.candidate.value.extension.rare, 'retained'); assert.equal(writes(h).length, 0);
});

test('whole review then explicit CAS Save/Apply uses saved identity; edit invalidates review', async () => {
    const h = harness(); await h.store.begin('system'); h.store.task.value.buffer.system.name = 'Склад'; await h.store.stage();
    await h.store.prepare(); assert.equal(writes(h).length, 0); assert.equal(h.store.canConfirm.value, true);
    await h.store.begin('system'); h.store.task.value.buffer.system.name = 'Лаборатория';
    assert.equal(h.store.canConfirm.value, false); await h.store.confirm(); assert.equal(writes(h).length, 0);
    await h.store.stage(); await h.store.prepare(); await h.store.confirm();
    assert.equal(writes(h).length, 2); assert.equal(h.active.hash, h.stored.hash); assert.match(h.store.outcome.value, /применены/);
});

test('stale stored or active after review causes zero mutations', async () => {
    for (const key of ['stored', 'active']) {
        const h = harness(); await h.store.prepare(); h[key] = { ...h[key], hash: 'c'.repeat(64) };
        await h.store.confirm(); assert.equal(writes(h).length, 0); assert.ok(h.store.issues.value.length);
    }
});

test('saved-not-active and lost Save response reconcile with reads; never blind retry', async () => {
    for (const flag of ['failApply', 'lostSave']) {
        const h = harness(); h[flag] = true; await h.store.begin('system'); h.store.task.value.buffer.system.name = 'Изменение'; await h.store.stage(); await h.store.prepare(); await h.store.confirm();
        assert.match(h.store.outcome.value, /Версия сохранена, но не активирована/);
        assert.equal(h.active.revision, 1); assert.equal(h.stored.revision, 2);
        assert.equal(writes(h).filter(x => x.method === 'PUT').length, 1);
        assert.equal(writes(h).filter(x => x.path === '/api/config/apply').length, flag === 'lostSave' ? 0 : 1);
        assert.equal(h.store.canConfirm.value, false);
    }
});

test('setup ownership blocks manual mutation without discarding local task', async () => {
    const h = harness(); await h.store.begin('system'); h.store.task.value.buffer.system.name = 'Новый план'; h.setupActive = true;
    assert.equal(await h.store.stage(), false); assert.equal(h.store.task.value.buffer.system.name, 'Новый план'); assert.equal(writes(h).length, 0);
});

test('shared consumers, typed values and rare mapping fields survive review projection', () => {
    const config = clone(fixture); config.connections = [{ id: 'connection_shared', name: 'Контроллер', enabled: true, type: 'http', http: { base_url: 'http://127.0.0.1' } }];
    config.bindings = config.devices.slice(0, 2).map(d => ({ target_id: d.id, action_id: 'action_on', connection_id: 'connection_shared', http: { method: 'POST', path: '/x', body: { on: true }, expected_status: 202, headers: { 'X-Mode': 'manual' } } }));
    const next = clone(config); next.connections[0].http.base_url = 'http://127.0.0.2';
    const change = readableDiff(config, next).find(c => c.title.startsWith('Подключение'));
    assert.deepEqual(change.consumers, ['Вентилятор', 'Сигнальная лампа']);
    assert.equal(next.bindings[0].http.expected_status, 202); assert.equal(next.bindings[0].http.body.on, true);
});

test('power-only template has exactly two actions; command intent pins target; door never synthesized', () => {
    const config = clone(fixture); const device = { id: 'device_new', name: 'Насос', kind: 'generic', enabled: true, room_id: null, spoken_aliases: ['насос'], action_ids: [], state: [] };
    config.devices.push(device); powerOnly(config, device); assert.equal(device.action_ids.length, 2);
    const action = config.actions.find(a => a.id === device.action_ids[0]);
    const intent = commandIntent(config, device, action, {}); assert.equal(intent.text, 'включи насос'); assert.equal(intent.expected_action.target_id, device.id);
    assert.equal(commandIntent(config, { ...device, kind: 'entrance_door' }, action, {}), null);
    assert.equal(commandIntent(config, { ...device, spoken_aliases: [] }, action, {}), null);
});

test('unaccepted import invalidates review and blocks publication without replacing accepted candidate', async () => {
    const h = harness(); await h.store.prepare(); const candidate = h.store.candidate.value;
    h.store.importBuffer.value = '{"unfinished":'; assert.equal(h.store.canConfirm.value, false);
    await h.store.prepare(); assert.equal(h.store.issues.value[0].code, 'import_pending'); assert.equal(h.store.candidate.value, candidate);
    await h.store.confirm(); assert.equal(writes(h).length, 0);
    h.store.importBuffer.value = ''; await h.store.prepare(); assert.equal(h.store.canConfirm.value, true);
    h.store.voiceBusy.value = true; assert.equal(h.store.canConfirm.value, false);
});

test('same hash but changed active generation requires another review', async () => {
    const h = harness(); h.active.generation_id = 1; await h.store.prepare(); h.active = { ...h.active, generation_id: 2 };
    await h.store.confirm(); assert.equal(writes(h).length, 0); assert.ok(h.store.issues.value.length);
});

test('zone examples never populate TaskBuffer; missing speech blocks validation without losing edits', async () => {
    for (const name of ['Цех', 'Склад', 'Лабораторный стенд']) {
        const h = harness(); await h.store.begin('zone');
        const zone = h.store.task.value.buffer.rooms.at(-1); zone.name = name;
        assert.deepEqual(zone.speech, { mentions: [], locations: [] });
        const before = clone(h.store.candidate.value);
        const count = () => h.calls.filter(c => c.path === '/api/config/validate').length;
        assert.equal(await h.store.stage(), false);
        assert.equal(count(), 0); assert.equal(h.store.task.value.buffer.rooms.at(-1).name, name);
        assert.deepEqual(h.store.issues.value.map(e => e.path.split('/').at(-1)), ['mentions', 'locations']);
        assert.deepEqual(h.store.candidate.value, before);
        zone.speech.mentions = [name.toLowerCase()];
        assert.equal(await h.store.stage(), false); assert.equal(count(), 0);
        assert.match(h.store.issues.value[0].message, /местоположение/);
        zone.speech.locations = ['на участке'];
        assert.equal(await h.store.stage(), true); assert.equal(count(), 1);
        assert.deepEqual(h.store.candidate.value.rooms.at(-1).speech, { mentions: [name.toLowerCase()], locations: ['на участке'] });
    }
});

test('zone group keeps existing backend rule; whitespace is not an explicit speech value', async () => {
    const h = harness(); await h.store.begin('zone');
    const zone = h.store.task.value.buffer.rooms.at(-1); zone.name = 'Участки';
    zone.speech = { mentions: ['   '], locations: ['\t'] };
    assert.equal(await h.store.stage(), false);
    zone.metadata.container = true;
    assert.equal(await h.store.stage(), false); assert.equal(h.store.issues.value[0].code, 'group_speech');
    zone.speech = { mentions: [], locations: [] };
    assert.equal(await h.store.stage(), true);
    assert.equal(h.calls.filter(c => c.path === '/api/config/validate').length, 1);
});

test('backend zone errors have human presentation without changing technical details or other errors', async () => {
    const { issueMessage } = await import('../.test-build/adapters/manualValidation.js');
    const issue = { code: 'invalid_alias', path: '/rooms/3', message: 'Command location requires explicit mentions and phrases; use a container for grouping only.' };
    const original = clone(issue);
    assert.match(issueMessage(issue), /Укажите, как обращаться/);
    assert.deepEqual(issue, original);
    const unrelated = { ...issue, message: 'Duplicate alias' };
    assert.equal(issueMessage(unrelated), 'Duplicate alias');
});
