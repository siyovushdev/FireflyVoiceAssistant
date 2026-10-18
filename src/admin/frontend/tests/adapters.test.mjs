import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { decodeResponse, readApi } from '../.test-build/api/client.js';
import { devices, zones, properties, sameGeneration } from '../.test-build/adapters/catalog.js';
import { createSystemStore } from '../.test-build/stores/system.js';
import { newMqttConnection, defaultMqttMapping, projectConnectivity, projectMqttWizard, readableDiff,
    consumers, connectionActionSummary, connectionObservationSummary } from '../.test-build/adapters/editing.js';
import { MqttDeviceDrafts, observationFromMqttDraft, pinMqttObservationConnection } from '../.test-build/adapters/mqttDeviceDrafts.js';

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

test('M20 MQTT Review shows preserved HTTP actions, GET and distinct brokers', () => {
    const candidate = structuredClone(fixture);
    candidate.connections = [
        { id: 'broker_a', name: 'MQTT-A', type: 'mqtt', enabled: true,
          network: { host: '127.0.0.1', port: 1883 }, mqtt: { tls: false }, timeout_ms: 2000 },
        { id: 'broker_b', name: 'MQTT-B', type: 'mqtt', enabled: true,
          network: { host: '127.0.0.1', port: 1884 }, mqtt: { tls: false }, timeout_ms: 2000 },
        { id: 'http_device', name: 'HTTP Controller', type: 'http', enabled: true,
          http: { base_url: 'http://127.0.0.1:8765' }, timeout_ms: 2000 }
    ];
    candidate.bindings = [
        { target_id: 'device_bench', action_id: 'turn_on', executor: 'mqtt',
          connection_id: 'broker_a', mqtt: { topic: 'relay/set', payload: 'ON', qos: 0, retain: false } },
        { target_id: 'device_bench', action_id: 'turn_off', executor: 'mqtt',
          connection_id: 'broker_b', mqtt: { topic: 'relay/set', payload: 'OFF', qos: 0, retain: false } },
        { target_id: 'device_bench', action_id: 'refresh', executor: 'http',
          connection_id: 'http_device', http: { method: 'POST', path: '/refresh' } }
    ];
    candidate.observations = [
        { target_id: 'device_bench', connection_id: 'http_device',
          executor: 'http', http: { method: 'GET', path: '/state' }, fields: { power: 'power' } }
    ];
    assert.match(connectionActionSummary(candidate, 'device_bench', 'turn_on'), /MQTT-A.*ON/);
    assert.match(connectionActionSummary(candidate, 'device_bench', 'turn_off'), /MQTT-B.*OFF/);
    assert.match(connectionActionSummary(candidate, 'device_bench', 'refresh'), /HTTP Controller.*POST \/refresh/);
    assert.match(connectionObservationSummary(candidate, 'device_bench'), /HTTP Controller.*GET \/state/);
    assert.equal(connectionActionSummary(candidate, 'device_bench', 'missing'), 'не подключено');
    candidate.observations = [{ target_id: 'device_bench', executor: 'mqtt',
        connection_id: 'broker_b', mqtt: { topic: 'relay/state', format: 'text', qos: 0 }, fields: {} }];
    assert.match(connectionObservationSummary(candidate, 'device_bench'), /MQTT-B.*relay\/state/);
});

test('M20 MQTT wizard switching to broker B preserves independent A and HTTP actions without side effects', () => {
    const base = structuredClone(fixture);
    const target = base.devices.at(-1).id, other = base.devices[0].id;
    base.connections = [
        { id: 'broker_a', name: 'MQTT A', enabled: true, type: 'mqtt',
          timeout_ms: 2000, network: { host: 'broker-a.local', port: 1883 }, mqtt: { tls: false } },
        { id: 'broker_b', name: 'MQTT B', enabled: true, type: 'mqtt',
          timeout_ms: 2000, network: { host: 'broker-b.local', port: 1884 },
          mqtt: { tls: false, credentials_id: 'stale_b', future_option: 'retain in candidate' } },
        { id: 'http_control', name: 'HTTP shared', enabled: true, type: 'http',
          http: { base_url: 'http://127.0.0.1:8765' }, timeout_ms: 2000 },
    ];
    base.bindings = [
        { target_id: target, action_id: 'on_mqtt', executor: 'mqtt',
          connection_id: 'broker_a', mqtt: { topic: 'a/set', payload: 'ON', qos: 1, retain: false } },
        { target_id: target, action_id: 'off_mqtt', executor: 'mqtt',
          connection_id: 'broker_b', mqtt: { topic: 'b/set', payload: 'OFF', qos: 0, retain: false } },
        { target_id: target, action_id: 'http_reset', executor: 'http',
          connection_id: 'http_control', http: { method: 'POST', path: '/reset', body: { reset: true } } },
        { target_id: other, action_id: 'another_device', executor: 'mqtt',
          connection_id: 'broker_a', mqtt: { topic: 'other/set', payload: 'ON', qos: 0, retain: false } },
    ];
    base.observations = [{ target_id: target, connection_id: 'http_control', executor: 'http',
                           http: { method: 'GET', path: '/state' }, fields: { on: 'power' } }];
    const frozen = structuredClone(base);
    const draft = projectMqttWizard({
        config: base, connection: base.connections[1], local: false,
        host: 'new-broker-b.example', port: 8883, tls: true, credentialsId: ' ',
        targetId: target, mappings: base.bindings.filter(b => b.executor === 'mqtt' && b.target_id === target),
        replaceOtherObservation: false
    });
    assert.deepEqual(base, frozen, 'Preview may not mutate any existing catalog');
    assert.deepEqual(draft.bindings.filter(b => b.executor === 'http'), base.bindings.filter(b => b.executor === 'http'));
    assert.deepEqual(draft.bindings.find(b => b.action_id === 'on_mqtt'), base.bindings[0],
                     'A stays on broker A without explicit transfer');
    assert.deepEqual(draft.bindings.find(b => b.action_id === 'off_mqtt'), base.bindings[1],
                     'B remains on broker B');
    assert.deepEqual(draft.bindings.find(b => b.action_id === 'another_device'), base.bindings[3],
                     'another device consumer of broker A is untouched');
    assert.deepEqual(draft.observations, base.observations, 'HTTP GET stays configured');
    assert.deepEqual(draft.connections[0], base.connections[0]);
    assert.equal(draft.connections[1].network.host, 'new-broker-b.example');
    assert.equal(draft.connections[1].network.port, 8883);
    assert.equal(draft.connections[1].mqtt.tls, true);
    assert.equal('credentials_id' in draft.connections[1].mqtt, false, 'explicitly empty credentials reference clears stale secret');
    assert.equal(draft.connections[1].mqtt.future_option, 'retain in candidate');
    assert.deepEqual(consumers(draft, 'broker_a').sort(),
                     [base.devices[0].name, base.devices.at(-1).name].sort());
    assert.match(connectionActionSummary(draft, target, 'on_mqtt'), /MQTT A.*a\/set/);
    assert.match(connectionActionSummary(draft, target, 'off_mqtt'), /MQTT B.*b\/set/);
    assert.match(connectionObservationSummary(draft, target), /HTTP shared/);
});

test('M20 MQTT wizard attaches only new bindings to chosen broker and transfers existing bindings explicitly', () => {
    const base = structuredClone(fixture), target = base.devices.at(-1).id;
    const a = { id: 'broker_a', name: 'Broker A', type: 'mqtt', enabled: true,
                timeout_ms: 2000, network: { host: '127.0.0.1', port: 1883 }, mqtt: { tls: false } };
    const b = { id: 'broker_b', name: 'Broker B', type: 'mqtt', enabled: true,
                timeout_ms: 2000, network: { host: '127.0.0.1', port: 1884 }, mqtt: { tls: false } };
    base.connections = [a, b];
    base.bindings = [{ target_id: target, action_id: 'previous', executor: 'mqtt',
                       connection_id: a.id, mqtt: { topic: 'a/set', payload: 'ON', qos: 0, retain: false } }];
    const added = { target_id: target, action_id: 'new', executor: 'mqtt',
                    mqtt: { topic: 'b/set', payload: 'OFF', qos: 1, retain: false } };
    const input = { config: base, connection: b, local: true, host: 'ignored-host',
        port: 1884, tls: false, credentialsId: ' relay_user ', targetId: target,
        mappings: [...base.bindings, added], replaceOtherObservation: false };
    const candidate = projectMqttWizard(input);
    assert.equal(candidate.bindings.find(x => x.action_id === 'previous').connection_id, 'broker_a');
    assert.equal(candidate.bindings.find(x => x.action_id === 'new').connection_id, 'broker_b');
    assert.equal(candidate.connections[1].network.host, '127.0.0.1');
    assert.equal(candidate.connections[1].mqtt.credentials_id, 'relay_user');
    assert.equal(base.bindings.length, 1, 'only the projected candidate contains new command');
    const transferred = projectMqttWizard({
        ...input, mappings: [{ ...base.bindings[0], connection_id: b.id }, added]
    });
    assert.equal(transferred.bindings.find(x => x.action_id === 'previous').connection_id, 'broker_b');
    assert.equal(base.bindings[0].connection_id, 'broker_a', 'transfer never mutates source or the old active broker');
});

test('M20 MQTT wizard observation replaces HTTP GET only on explicitly approved choice', () => {
    const base = structuredClone(fixture), target = base.devices.at(-1).id;
    const mqtt = newMqttConnection(base);
    mqtt.name = 'Local'; base.connections = [mqtt];
    const http = { target_id: target, connection_id: 'http_read', executor: 'http',
                   http: { method: 'GET', path: '/state' }, fields: { on: 'power' } };
    base.observations = [http];
    const projection = { config: base, connection: mqtt, local: true, host: '127.0.0.1',
        port: 1883, tls: false, credentialsId: '', targetId: target, mappings: [] };
    const noChange = projectMqttWizard({ ...projection, replaceOtherObservation: false });
    assert.deepEqual(noChange.observations, [http], 'selecting MQTT connection alone must not remove HTTP observe');
    const next = { target_id: target, connection_id: mqtt.id, executor: 'mqtt',
        mqtt: { topic: 'relay/state', format: 'json', qos: 1 }, fields: { on: 'power' } };
    const replaced = projectMqttWizard({
        ...projection, observation: next, replaceOtherObservation: true
    });
    assert.deepEqual(replaced.observations, [next]);
    assert.deepEqual(base.observations, [http], 'HTTP observation in stored candidate remains intact');
});

test('M20 MQTT wizard can draft broker-only connection in empty catalog without hardware effects', () => {
    const base = structuredClone(fixture);
    base.connections = []; base.bindings = []; base.observations = [];
    const mqtt = newMqttConnection(base);
    mqtt.name = 'Broker only';
    const draft = projectMqttWizard({
        config: base, connection: mqtt, local: true, host: '', port: 1883,
        tls: false, credentialsId: '', targetId: '', mappings: [],
        replaceOtherObservation: false
    });
    assert.equal(draft.connections.length, 1);
    assert.deepEqual(draft.bindings, []);
    assert.deepEqual(draft.observations, []);
    assert.equal(base.connections.length, 0, 'wizard preview never writes the source graph');
});

test('M20 MQTT R2 retains per-device unsaved topic and observation across A→B→A', () => {
    const base = structuredClone(fixture);
    const idA = base.devices.at(-1).id, idB = base.devices[0].id;
    const selected = { id: 'broker_b', name: 'Broker B', enabled: true, type: 'mqtt',
        timeout_ms: 2000, network: { host: '127.0.0.1', port: 1884 }, mqtt: { tls: false } };
    base.connections = [
        { id: 'broker_a', name: 'Broker A', enabled: true, type: 'mqtt',
          timeout_ms: 2000, network: { host: '127.0.0.1', port: 1883 }, mqtt: { tls: false } },
        selected
    ];
    base.bindings = [
        { target_id: idA, action_id: 'action_on_a', executor: 'mqtt',
          connection_id: 'broker_a', mqtt: { topic: 'cmd/a', payload: 'ON', qos: 0, retain: false } },
        { target_id: idB, action_id: 'action_on_b', executor: 'mqtt',
          connection_id: 'broker_b', mqtt: { topic: 'cmd/b', payload: 'ON', qos: 1, retain: false } }
    ];
    base.observations = [
        { target_id: idA, connection_id: 'broker_a', executor: 'mqtt',
          mqtt: { topic: 'state/a', format: 'text', field: 'power', qos: 0,
                  true_payload: 'ON', false_payload: 'OFF' }, fields: {} },
        { target_id: idB, connection_id: 'http_reader', executor: 'http',
          http: { method: 'GET', path: '/state' }, fields: { on: 'power' } }
    ];
    const before = structuredClone(base);
    const stash = new MqttDeviceDrafts(base);
    const editedA = stash.restore(idA);
    editedA.mappings[0].mqtt.topic = 'cmd/edited';
    editedA.observation.topic = 'state/edited';
    stash.remember(idA, editedA);
    const editedB = stash.restore(idB);
    editedB.mappings[0].mqtt.topic = 'cmd/b-edited';
    const restoredA = stash.restore(idA);
    assert.equal(restoredA.mappings[0].mqtt.topic, 'cmd/edited');
    assert.equal(restoredA.observation.topic, 'state/edited');
    // A user may edit the current form after the last stash; its live value
    // must win over the stale per-device snapshot.
    editedB.mappings[0].mqtt.payload = 'OFF';
    const modified = stash.changed(idB, editedB);
    assert.deepEqual(modified.map(([id]) => id), [idA, idB]);
    let candidate = projectMqttWizard({
        config: base, connection: selected, local: false,
        host: '127.0.0.1', port: 1884, tls: false, credentialsId: '',
        targetId: '', mappings: [], replaceOtherObservation: false
    });
    for (const [id, draft] of modified) {
        candidate = projectMqttWizard({
            config: candidate, connection: selected, local: false,
            host: '127.0.0.1', port: 1884, tls: false, credentialsId: '',
            targetId: id, mappings: draft.mappings,
            observation: observationFromMqttDraft(id, draft),
            replaceOtherObservation: draft.observing && draft.replaceOtherObservation
        });
    }
    assert.deepEqual(base, before, 'draft navigation and projection cannot touch stored source');
    assert.equal(candidate.bindings.find(x => x.target_id === idA).mqtt.topic, 'cmd/edited');
    assert.equal(candidate.bindings.find(x => x.target_id === idA).connection_id, 'broker_a');
    assert.equal(candidate.bindings.find(x => x.target_id === idB).mqtt.topic, 'cmd/b-edited');
    assert.equal(candidate.bindings.find(x => x.target_id === idB).mqtt.payload, 'OFF');
    assert.equal(candidate.observations.find(x => x.target_id === idA).mqtt.topic, 'state/edited');
    assert.deepEqual(candidate.observations.find(x => x.target_id === idB),
                     before.observations[1], 'other HTTP GET remains intact');
});

test('M20 MQTT R2 preserves partially edited JSON observation and never rewrites unvisited devices', () => {
    const base = structuredClone(fixture), idA = base.devices.at(-1).id;
    const idB = base.devices[0].id;
    const unknownFuture = { vendor_mapping: { future: 'must not change' } };
    base.observations = [{
        target_id: idB, connection_id: 'broker_b', executor: 'mqtt',
        mqtt: { topic: 'state/b', format: 'text', field: 'power', qos: 0,
                true_payload: 'ON', false_payload: 'OFF', ...unknownFuture },
        fields: {}
    }];
    const stash = new MqttDeviceDrafts(base);
    const untouchedB = stash.restore(idB);
    stash.remember(idB, untouchedB); // merely visiting must not modify it
    const editedA = stash.restore(idA);
    editedA.observeConnectionId = pinMqttObservationConnection(
        editedA.observeConnectionId, 'broker_a');
    editedA.observing = true;
    editedA.observation.format = 'json';
    editedA.observation.topic = 'state/new';
    editedA.fields = [['on', 'power']];
    stash.remember(idA, editedA);
    assert.deepEqual(stash.restore(idA).fields, [['on', 'power']],
                     'JSON state fields must survive a device navigation');
    assert.deepEqual(stash.changed(idB, untouchedB).map(([id]) => id), [idA],
                     'untouched B excluded from Review projection');
    assert.deepEqual(base.observations[0].mqtt.vendor_mapping, unknownFuture.vendor_mapping);
    assert.deepEqual(observationFromMqttDraft(idA, editedA).fields,
                     { on: 'power' });
});

test('M20 MQTT R3 caches wizard component across protocol navigation without early Apply', () => {
    const shell = readFileSync(new URL('../src/components/ConnectionWizard.vue', import.meta.url), 'utf8');
    assert.match(shell, /<KeepAlive>\s*<MqttWizard v-if="transport === 'mqtt'"/,
        'MQTT instance must not be torn down by protocol chooser');
    assert.match(shell, /<div v-if="transport !== 'mqtt'"/,
        'HTTP wizard is independent and still visible after leaving MQTT');
    const wizard = readFileSync(new URL('../src/components/MqttWizard.vue', import.meta.url), 'utf8');
    assert.match(wizard, /watch\(targetId, \(next, previous\) => \{/,
        'outgoing per-device state is captured before loading another device');
    assert.match(wizard, /deviceDrafts\.changed\(targetId\.value, currentDeviceDraft\(\)\)/,
        'Review projects all changed devices, not merely the last visible one');
    assert.match(wizard, /В общий Review \(без Apply\)/,
        'navigation back and forward must never perform implicit Apply');
});


test('M20 #33 R4 new state subscription A stays on broker A after selecting B and adding B state', () => {
    const base = structuredClone(fixture);
    const idA = base.devices.at(-1).id;
    const idB = base.devices[0].id;
    const brokerA = { id: 'broker_a', name: 'Broker A', type: 'mqtt',
        enabled: true, timeout_ms: 2000, network: { host: '127.0.0.1', port: 1883 },
        mqtt: { tls: false } };
    const brokerB = { id: 'broker_b', name: 'Broker B', type: 'mqtt',
        enabled: true, timeout_ms: 2000, network: { host: '127.0.0.1', port: 1884 },
        mqtt: { tls: false } };
    base.connections = [brokerA, brokerB];
    base.bindings = [
        { target_id: idA, action_id: 'action_A', executor: 'mqtt',
          connection_id: brokerA.id,
          mqtt: { topic: 'cmd/a', payload: 'ON', qos: 0, retain: false } },
        { target_id: idB, action_id: 'action_B', executor: 'mqtt',
          connection_id: brokerB.id,
          mqtt: { topic: 'cmd/b', payload: 'OFF', qos: 1, retain: false } }
    ];
    base.observations = [{
        target_id: idA, executor: 'http', connection_id: 'http_reader',
        http: { method: 'GET', path: '/state' }, fields: { on: 'power' }
    }];
    const original = structuredClone(base);
    const draftStore = new MqttDeviceDrafts(base);

    // Browser action: user selects broker A and ENABLES an entirely new
    // observation; that is the moment A's broker identity becomes durable
    // inside the per-device TaskBuffer draft.
    const draftA = draftStore.restore(idA);
    assert.equal(draftA.observeConnectionId, '');
    draftA.observeConnectionId = pinMqttObservationConnection(
        draftA.observeConnectionId, brokerA.id);
    draftA.observing = true;
    draftA.replaceOtherObservation = true; // explicit HTTP GET -> MQTT choice
    draftA.observation = {
        topic: 'state/new-A', format: 'text', qos: 0, field: 'power',
        true_payload: 'ON', false_payload: 'OFF'
    };
    draftStore.remember(idA, draftA);

    // Browser action: user navigates back to Connection step, selects B,
    // selects device B, creates a new observation. No button ever transfers A.
    const draftB = draftStore.restore(idB);
    draftB.observeConnectionId = pinMqttObservationConnection(
        draftB.observeConnectionId, brokerB.id);
    draftB.observing = true;
    draftB.observation = {
        topic: 'state/new-B', format: 'text', qos: 1, field: 'power',
        true_payload: 'ON', false_payload: 'OFF'
    };
    assert.equal(pinMqttObservationConnection(draftA.observeConnectionId, brokerB.id),
                 brokerA.id, 'reselecting a different broker must not repin A');

    // This is the real final Review's projection loop: every changed device
    // uses its individually pinned observation ID, not the latest selection B.
    let review = projectMqttWizard({
        config: base, connection: brokerB, local: false,
        host: '127.0.0.1', port: 1884, tls: false, credentialsId: '',
        targetId: '', mappings: [], replaceOtherObservation: false
    });
    for (const [id, draft] of draftStore.changed(idB, draftB)) {
        review = projectMqttWizard({
            config: review, connection: brokerB, local: false,
            host: '127.0.0.1', port: 1884, tls: false, credentialsId: '',
            targetId: id, mappings: draft.mappings,
            observation: observationFromMqttDraft(id, draft),
            replaceOtherObservation: draft.observing && draft.replaceOtherObservation
        });
    }
    assert.deepEqual(base, original, 'no mutation to source/current ConfigStore before Apply');
    assert.equal(review.bindings.find(x => x.target_id === idA).connection_id, brokerA.id);
    assert.equal(review.bindings.find(x => x.target_id === idB).connection_id, brokerB.id);
    assert.equal(review.observations.find(x => x.target_id === idA).connection_id,
                 brokerA.id, 'new state topic of A must still subscribe at broker A');
    assert.equal(review.observations.find(x => x.target_id === idA).mqtt.topic, 'state/new-A');
    assert.equal(review.observations.find(x => x.target_id === idB).connection_id,
                 brokerB.id, 'new state topic of B belongs at broker B');
    assert.equal(review.observations.find(x => x.target_id === idB).mqtt.topic, 'state/new-B');
    assert.equal(review.observations.length, 2,
                 'explicit HTTP observation replacement must produce one observation per device');
    assert.match(connectionObservationSummary(review, idA), /Broker A.*state\/new-A/);
    assert.match(connectionObservationSummary(review, idB), /Broker B.*state\/new-B/);
});

test('M20 #33 R4 missing or changed state broker identity fails closed until explicit transfer', () => {
    const base = structuredClone(fixture);
    const id = base.devices.at(-1).id;
    const draft = new MqttDeviceDrafts(base).restore(id);
    draft.observing = true;
    draft.observation.topic = 'state/new-A';

    assert.throws(() => observationFromMqttDraft(id, draft),
        /mqtt_observation_broker_not_selected/,
        'missing broker must not use the last selected broker during Review');
    assert.throws(() => pinMqttObservationConnection('', ''),
        /mqtt_observation_broker_not_selected/,
        'an observation cannot be enabled without an actual MQTT connection');

    draft.observeConnectionId = pinMqttObservationConnection('', 'broker_a');
    assert.equal(observationFromMqttDraft(id, draft).connection_id, 'broker_a');
    assert.equal(pinMqttObservationConnection(draft.observeConnectionId, 'broker_b'),
                 'broker_a', 'selecting B is not explicit transfer');

    // Existing observation migrates ONLY upon an explicit UI transfer action.
    draft.observeConnectionId = 'broker_b';
    assert.equal(observationFromMqttDraft(id, draft).connection_id, 'broker_b');
    draft.observing = false;
    assert.equal(observationFromMqttDraft(id, draft), undefined,
                 'disabling observation removes subscription without inventing a broker');
});
