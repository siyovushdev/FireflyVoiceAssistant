import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { decodeResponse, readApi } from '../.test-build/api/client.js';
import { devices, zones, properties, sameGeneration } from '../.test-build/adapters/catalog.js';
import { createSystemStore } from '../.test-build/stores/system.js';
import { newMqttConnection, defaultMqttMapping, projectConnectivity, readableDiff } from '../.test-build/adapters/editing.js';

const fixture = JSON.parse(readFileSync(new URL('../../../../tests/admin/v2/system.json', import.meta.url)));
const catalog = () => ({ ok: true, configuration: structuredClone(fixture), hash: 'a'.repeat(64), revision: 1, generation_id: 3 });
const property = (knowledge, value, stale = false) => ({ knowledge, value, stale, updated_at_ms: 400, observed_at_ms: knowledge === 'confirmed' ? 300 : null });
const state = props => ({ targetId: 'device_bench', properties: { power: props }, connectivity: 'reachable', last_success_at_ms: 300, last_failure_at_ms: null });

test('wire DTO retains rare fields and immutable data; zones are a projection', () => {
    const wire = catalog();
    wire.configuration.future_extension = { nested: ['preserved'] };
    const decoded = decodeResponse('/api/config', wire);
    assert.deepEqual(decoded.configuration.future_extension, { nested: ['preserved'] });
    assert.throws(() => { decoded.configuration.future_extension.nested.push('mutated'); }, TypeError);
    assert.deepEqual(zones(decoded.configuration).map(zone => zone.name), ['Цех', 'Склад', 'Лабораторный стенд']);
    assert.equal(devices(decoded.configuration, null, null).find(d => d.id === 'device_bench').location, 'Без зоны');
    assert.equal(decoded.configuration.devices.at(-1).room_id, null);
});

test('initial and legacy values never become physical facts', () => {
    const device = fixture.devices.at(-1);
    assert.equal(device.state[0].initial, true);
    assert.equal(properties(device)[0].value, '—');
    assert.equal(properties(device, { state: { power: true }, properties: {} })[0].tone, 'unknown');
    assert.equal(properties(device, state(property('future-knowledge', true)))[0].value, '—');
    assert.equal(properties(device, state(property('unknown', true)))[0].value, '—');
});

test('knowledge, stale and reachability stay separate; false/zero are retained', () => {
    const device = fixture.devices.at(-1);
    assert.match(properties(device, state(property('predicted', false)))[0].label, /не подтверждено/);
    assert.equal(properties(device, state(property('confirmed', false)))[0].value, 'Нет / выключено');
    assert.equal(properties(device, state(property('confirmed', 0)))[0].value, '0');
    const faultDevice = { ...device, state: [{ id: 'fault', name: 'Неисправность', type: 'boolean' }] };
    assert.equal(properties(faultDevice, { properties: { fault: property('confirmed', true) } })[0].value, 'Да');
    assert.equal(properties(faultDevice, { properties: { fault: property('confirmed', false) } })[0].value, 'Нет');
    const stale = properties(device, state(property('confirmed', true, true)))[0];
    assert.equal(stale.tone, 'stale');
    assert.equal(stale.stale, true);
    assert.equal(stale.timestamp, 300);
    const active = catalog();
    const projected = devices(active.configuration, active, { active, devices: [state(property('unknown', null))] }).at(-1);
    assert.equal(projected.reachability, 'Последний обмен успешен');
    assert.equal(projected.properties[0].tone, 'unknown');
    assert.equal(projected.connection, 'Подключение не настроено');
});

test('evidence cannot cross generation, catalog identity or stored/active boundary', () => {
    const active = catalog();
    const runtime = { active, devices: [state(property('confirmed', true))] };
    assert.equal(devices(active.configuration, active, runtime).at(-1).properties[0].tone, 'confirmed');
    for (const other of [{ ...active, hash: 'b'.repeat(64) }, { ...active, generation_id: 4 }, { ...active, revision: 2 }, null]) {
        assert.equal(sameGeneration(active, other), false);
        assert.equal(devices(active.configuration, other, runtime).at(-1).properties[0].tone, 'unknown');
    }
    assert.equal(devices(structuredClone(active.configuration), active, runtime).at(-1).properties[0].tone, 'unknown');
});

test('malformed responses fail closed and error envelopes never look successful', () => {
    assert.throws(() => decodeResponse('/api/config', { ok: false }), /не смог/);
    const bad = catalog(); bad.configuration.devices[0].room_id = 123;
    assert.throws(() => decodeResponse('/api/config', bad), /формат/);
    assert.throws(() => decodeResponse('/api/devices/state', { ok: true, active: null, devices: [state({ ...property('confirmed', true), stale: 'no' })] }), /формат/);
    assert.throws(() => decodeResponse('/api/config/active', { ok: true }), /формат/);
});

test('read-only client cannot call a mutation or external endpoint', async () => {
    await assert.rejects(readApi('/api/devices/poll'), /режиме просмотра/);
    await assert.rejects(readApi('https://example.invalid'), /режиме просмотра/);
});

test('partial API failures keep catalog available, clear current evidence and expose errors', async () => {
    let fail = false;
    const active = catalog();
    const stored = { ...catalog(), hash: 'b'.repeat(64), revision: 2 };
    stored.configuration.system.name = 'Saved but not active';
    const replies = {
        '/api/config': stored,
        '/api/config/active': { ok: true, active },
        '/api/devices/state': { ok: true, active, devices: [state(property('confirmed', true))] },
        '/api/executions': { ok: true, active, executions: [] },
        '/api/status': { ok: true, runtime_ready: true, runtime_busy: false, needs_apply: true, active, stored },
    };
    let calls = 0;
    let inFlight = 0, maximumInFlight = 0;
    const store = createSystemStore(async path => {
        calls++; inFlight++; maximumInFlight = Math.max(maximumInFlight, inFlight);
        await new Promise(resolve => setTimeout(resolve, 1));
        inFlight--;
        if (fail && path === '/api/devices/state') throw Error('offline');
        return replies[path];
    });
    await Promise.all([store.refresh(), store.refresh()]);
    assert.equal(calls, 5, 'refresh is serialized');
    assert.equal(maximumInFlight, 1, 'state/history reads respect legacy IPC runtime serialization');
    assert.equal(store.pending.value, true);
    assert.notEqual(store.configuration.value.system.name, 'Saved but not active');
    assert.equal(store.devices.value.at(-1).properties[0].tone, 'confirmed');
    fail = true;
    await store.refresh();
    assert.equal(store.errors.value['/api/devices/state'], 'offline');
    assert.equal(store.devices.value.at(-1).properties[0].tone, 'unknown');
    assert.equal(store.configuration.value, active.configuration);
});

test('no active runtime shows stored catalog with Unknown evidence', async () => {
    const stored = catalog();
    const store = createSystemStore(async path => {
        if (path === '/api/config') return stored;
        if (path === '/api/config/active') return { ok: true, active: null };
        throw Error('runtime unavailable');
    });
    await store.refresh();
    assert.equal(store.configuration.value, stored.configuration);
    assert.equal(store.active.value, null);
    assert.equal(store.health.value, 'Статус недоступен');
    assert.equal(store.devices.value.at(-1).properties[0].value, '—');
});

test('M20 MQTT manual draft is separate from HTTP and visible in readable Review', () => {
    const base = structuredClone(fixture);
    const connection = newMqttConnection(base);
    assert.equal(connection.type, 'mqtt');
    assert.equal(connection.network.host, '127.0.0.1');
    assert.equal(connection.mqtt.tls, false);
    const command = { target_id: 'device_bench', action_id: 'action_relay_on',
        connection_id: connection.id, executor: 'mqtt',
        mqtt: { ...defaultMqttMapping(), topic: 'home/relay/set', payload: 'ON' } };
    const observation = { target_id: 'device_bench', connection_id: connection.id, executor: 'mqtt',
        mqtt: { topic: 'home/relay/state', format: 'text', field: 'power',
            true_payload: 'ON', false_payload: 'OFF', qos: 0 }, fields: {} };
    const next = projectConnectivity(base, connection, 'device_bench', [command], observation);
    assert.equal(next.connections.some(c => c.id === connection.id), true);
    assert.equal(next.bindings.some(b => b.executor === 'mqtt' && b.mqtt.topic === 'home/relay/set'), true);
    assert.equal(next.observations.some(o => o.executor === 'mqtt'), true);
    assert.match(readableDiff(base, next).map(c => c.afterText).join(' / '), /MQTT|relay\/set/);
    assert.equal(base.connections?.some(c => c.type === 'mqtt') ?? false, false,
        'editing must not mutate the active snapshot');
});

test('M20 mixed HTTP/MQTT draft retains other transport on edit', () => {
    const base = structuredClone(fixture);
    const mqtt = newMqttConnection(base);
    base.connections ??= [];
    base.connections.push(mqtt);
    base.bindings = [
        { target_id: 'device_bench', action_id: 'action_http', executor: 'http',
          connection_id: 'http_controller', http: { method: 'POST', path: '/relay/2', body: {} } },
        { target_id: 'device_bench', action_id: 'action_mqtt_a', executor: 'mqtt',
          connection_id: 'broker_a', mqtt: { topic: 'a/set', payload: 'ON', qos: 0, retain: false } },
        { target_id: 'device_bench', action_id: 'action_mqtt_b', executor: 'mqtt',
          connection_id: 'broker_b', mqtt: { topic: 'b/set', payload: 'OFF', qos: 0, retain: false } }
    ];
    base.observations = [{ target_id: 'device_bench', connection_id: 'http_controller', executor: 'http',
        http: { method: 'GET', path: '/relay/2', body: {} }, fields: { on: 'power' } }];
    const updateA = structuredClone(base.bindings.filter(b => b.executor === 'mqtt'));
    updateA[0].mqtt.payload = 'OFF';
    const next = projectConnectivity(base, mqtt, 'device_bench', updateA, undefined, 'mqtt');
    assert.deepEqual(next.bindings.find(b => b.executor === 'http'), base.bindings[0],
        'saving MQTT must preserve HTTP action');
    assert.deepEqual(next.observations[0], base.observations[0],
        'saving MQTT without a new observation preserves HTTP GET');
    assert.equal(next.bindings.find(b => b.action_id === 'action_mqtt_a').connection_id, 'broker_a');
    assert.equal(next.bindings.find(b => b.action_id === 'action_mqtt_b').connection_id, 'broker_b');
    assert.equal(next.bindings.find(b => b.action_id === 'action_mqtt_a').mqtt.payload, 'OFF');
    const newMqttObs = { target_id: 'device_bench', connection_id: 'broker_b',
        executor: 'mqtt', mqtt: { topic: 'b/state', format: 'text', field: 'power', qos: 0 },
        fields: {} };
    const replace = projectConnectivity(base, mqtt, 'device_bench', updateA, newMqttObs, 'mqtt', true);
    assert.equal(replace.observations.length, 1, 'explicit replacement is required');
    assert.equal(replace.observations[0].executor, 'mqtt');
    const httpEdit = projectConnectivity(next, null, 'device_bench', [base.bindings[0]],
        base.observations[0], 'http');
    assert.deepEqual(httpEdit.bindings.filter(b => b.executor === 'mqtt'), updateA);
});
