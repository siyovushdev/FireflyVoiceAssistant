import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { devices, properties, formatAge } from '../.test-build/adapters/catalog.js';
import { decodeResponse } from '../.test-build/api/client.js';

const config = JSON.parse(readFileSync(new URL('../../../../tests/admin/v2/system.json', import.meta.url)));
const active = { ok:true, hash:'a'.repeat(64), revision:1, generation_id:1, configuration:config };
const evidence = { targetId:'device_bench', properties:{power:{value:false,knowledge:'confirmed',stale:false,updated_at_ms:100,observed_at_ms:100,last_observed_value:false,observation_age_ms:0}}, connectivity:'reachable',last_success_at_ms:100,last_failure_at_ms:null,availability:'online',availability_reason:'recent_observation',last_observation_at_ms:100,observation_age_ms:0 };

test('device availability is independent of power and broker/command success', () => {
    const project = e => devices(config,active,{ok:true,active,devices:[e]}).find(d=>d.id==='device_bench');
    assert.equal(project(evidence).availability,'online');
    assert.equal(project(evidence).properties[0].value,'Нет / выключено');
    const oldBackend = {...evidence};delete oldBackend.availability;
    assert.equal(project(oldBackend).availability,'unknown','reachable broker must not imply online device');
    assert.equal(project({...evidence,availability:'offline',observation_age_ms:61000}).properties[0].value,'Нет / выключено');
    const mismatch = devices(config,{...active,generation_id:2},{ok:true,active,devices:[evidence]}).at(-1);
    assert.equal(mismatch.availability,'unknown');assert.equal(mismatch.lastObservation,null);
});
test('disconnect retains historical OFF; a later ON prediction is a separate value', () => {
    const target=config.devices.at(-1);
    const p={...evidence.properties.power,value:null,knowledge:'unknown'};
    let shown=properties(target,{properties:{power:p}})[0];
    assert.equal(shown.value,'Нет / выключено');assert.equal(shown.tone,'stale');
    assert.match(shown.label,/Последнее известное/);
    shown=properties(target,{properties:{power:{...p,value:true,knowledge:'predicted'}}})[0];
    assert.equal(shown.value,'Да / включено');assert.equal(shown.lastObserved,'Нет / выключено');
    assert.equal(shown.tone,'predicted');
});
test('telemetry DTO validates ages and values and preserves extra metadata', () => {
    const response={ok:true,active,devices:[evidence]};
    assert.equal(decodeResponse('/api/devices/state',response).devices[0].observation_age_ms,0);
    for(const patch of [{observation_age_ms:-1},{availability:'broker_connected'},{observation_age_ms:Infinity}])
        assert.throws(()=>decodeResponse('/api/devices/state',{...response,devices:[{...evidence,...patch}]}));
    assert.equal(formatAge(0),'0 с назад');assert.equal(formatAge(61000),'1 мин назад');
    assert.equal(formatAge(null),'Данных ещё нет');
});

test('voice input DTO keeps CLI/config mode and filter capability explicit', () => {
    for (const enabled of [false,true]) {
        const input={inline_commands:enabled,mode:enabled?'auto':'after-beep',confirmation_enabled:true};
        assert.deepEqual(decodeResponse('/api/status',{ok:true,runtime_ready:true,runtime_busy:false,needs_apply:false,active:null,voice_input:input}).voice_input,input);
    }
    for(const input of [{inline_commands:'true',mode:'auto'},{inline_commands:true,mode:'anything'},{inline_commands:true,mode:'auto',confirmation_enabled:1}])
        assert.throws(()=>decodeResponse('/api/status',{ok:true,runtime_ready:true,runtime_busy:false,needs_apply:false,active:null,voice_input:input}));
});
