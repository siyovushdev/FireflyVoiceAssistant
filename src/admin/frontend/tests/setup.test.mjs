import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { collectFacts, emptyFacts, missingFact, physicalInput, buildConnectivity, supportsPowerContract, verifyConnectivity, verifySemantic, applyLanguage } from '../.test-build/adapters/setup.js';
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
const fullFacts = () => collectFacts(emptyFacts(), 'http://127.0.0.1:8765 Включить POST /relay/1 {"on":true}. Выключить POST /relay/1 {"on":false}. Состояние GET /relay/1, поле on.');
test('explicit connectivity uses B2 defaults/projection, exact scalar contracts, and immutable semantic plan', async () => {
    const { newHttpConnection, defaultHttpMapping, projectConnectivity } = await import('../.test-build/adapters/editing.js');
    const before = fixture(), snapshot = clone(before), facts = fullFacts();
    const after = buildConnectivity(before, 'fan', facts, 'separate');
    const connection = { ...newHttpConnection(before), name: 'Вентилятор HTTP', http: { base_url: facts.endpoint, base_path: '' } };
    const mappings = before.actions.map(a => ({ target_id:'fan', action_id:a.id, connection_id:connection.id, executor:'http', http:{...defaultHttpMapping(),method:'POST',path:'/relay/1',body:{on:a.assignments[0].value}} }));
    const observation = {target_id:'fan',connection_id:connection.id,executor:'http',http:{...defaultHttpMapping(),method:'GET',path:'/relay/1'},fields:{on:'power'}};
    assert.deepEqual(after, projectConnectivity(before, connection, 'fan', mappings, observation));
    assert.deepEqual(before,snapshot); assert.deepEqual(after.actions,before.actions); assert.deepEqual(after.devices,before.devices);
    verifyConnectivity(before,after,'fan',facts,'separate');
    for (const mutate of [c=>c.connections[0].http.base_url='http://192.168.1.1', c=>c.bindings[0].http.body.on='true', c=>c.bindings[0].target_id='other', c=>c.bindings[0].http.headers={secret:'invented'}, c=>c.observations[0].fields={nested:'power'}, c=>c.devices[0].name='Changed',c=>c.operations=[{op:'create_device'}]]) {
        const changed=clone(after);mutate(changed);assert.throws(()=>verifyConnectivity(before,changed,'fan',facts,'separate'));
    }
    assert.throws(()=>verifyConnectivity(before,after,'fan',{...facts,endpoint:'http://127.0.0.2'},'separate'));
    assert.throws(()=>verifySemantic(before,after));
});
test('explicit shared/separate choice preserves other consumers including Observe-only; no-observe removes only selected reading', () => {
    let before=buildConnectivity(fixture(),'fan',fullFacts(),'separate');
    const other={...clone(before.devices[0]),id:'other',name:'Другое устройство'};before.devices.push(other);
    before.observations.push({...clone(before.observations[0]),target_id:'other'});
    const id=before.connections[0].id, facts={...fullFacts(),endpoint:'http://127.0.0.2',noObserve:true,observe:'',field:''};
    for(const choice of ['all','separate']) {
        const after=buildConnectivity(before,'fan',facts,choice);
        assert.deepEqual(after.observations,before.observations.filter(o=>o.target_id==='other'));
        assert.equal(after.connections.find(c=>c.id===id).http.base_url,choice==='all'?facts.endpoint:fullFacts().endpoint);
        assert.equal(after.connections.length,choice==='all'?1:2);
        assert.ok(after.bindings.every(b=>choice==='all'?b.connection_id===id:b.connection_id!==id));
        assert.deepEqual(after.actions,before.actions);
    }
    assert.throws(()=>buildConnectivity(before,'fan',facts,''));
    before.observations[0].connection_id='second';assert.throws(()=>buildConnectivity(before,'fan',facts,'all'));
});
test('no guesses: incomplete, non-power, ambiguous actions and unsupported HTTP use manual fallback', () => {
    const before=fixture();
    for(const facts of [emptyFacts(),{...fullFacts(),on:null},{...fullFacts(),endpoint:'http://127.0.0.1/path'},{...fullFacts(),on:{method:'POST',path:'/x',body:{nested:{x:1}}}},{...fullFacts(),noObserve:true}]) assert.throws(()=>buildConnectivity(before,'fan',facts,'separate'));
    for(const text of ['http://127.0.0.1/path','https://127.0.0.1','http://127.0.0.1:bad','POST /x','POST /x {"nested":{"x":1}}','поле nested.on']) assert.throws(()=>collectFacts(emptyFacts(),text));
    for(const mutate of [c=>c.actions[0].parameters.push({id:'x'}),c=>c.devices[0].action_ids.push('extra'),c=>c.actions[0].assignments.push({field:'other',value:true}),c=>c.actions[1].assignments[0].value=true,c=>c.devices[0].kind='entrance_door']) {
        const c=clone(before);mutate(c);assert.equal(supportsPowerContract(c,'fan'),false);assert.throws(()=>buildConnectivity(c,'fan',fullFacts(),'separate'));
    }
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

test('physical coordinator validates before review and Accept, never calls AI; stale facts/version preserve accepted plan', async () => {
    const {manual}=await import('../.test-build/stores/manual.js');const s=await import('../.test-build/stores/setup.js');const {setupAuth}=await import('../.test-build/api/setup.js');
    const memory=new Map();globalThis.sessionStorage={getItem:k=>memory.get(k)??null,setItem:(k,v)=>memory.set(k,v),removeItem:k=>memory.delete(k)};
    setupAuth.token='';manual.base.value=null;manual.candidate.value=null;manual.task.value=null;s.proposal.value=null;s.session.value=null;
    const initial=fixture();let state={ok:true,active:false,status:'idle'},failValidation=false;const calls=[];const originalFetch=globalThis.fetch;
    globalThis.fetch=async(path,options={})=>{
        const body=options.body?JSON.parse(options.body):undefined;calls.push({path,body});let reply;
        if(path==='/api/config')reply={ok:true,revision:1,hash:'base',configuration:initial};
        else if(path==='/api/setup/status'||path==='/api/setup/heartbeat')reply=state;
        else if(path==='/api/setup/start')reply=state={ok:true,active:true,status:'editing',version:0,session_id:'owner',configuration:initial};
        else if(path==='/api/config/validate')reply=failValidation?{ok:false,errors:[{code:'invalid_endpoint',path:'/connections/0',message:'invalid endpoint'}]}:{ok:true,valid:true};
        else if(path==='/api/setup/draft'){assert.equal(body.version,state.version);reply=state={...state,version:state.version+1,configuration:body.configuration};}
        else throw Error('Unexpected request '+path);
        return new Response(JSON.stringify(reply),{status:reply.ok?200:422});
    };
    try {
        await s.startSetup();s.connectDevice('fan');s.sharedChoice.value='separate';
        s.composer.value='как обычно';await s.sendMessage();assert.equal(s.proposal.value,null);
        s.composer.value='http://127.0.0.1:8765 Включить POST /relay/1 {"on":true}. Выключить POST /relay/1 {"on":false}. Состояние GET /relay/1, поле on.';
        await s.sendMessage();assert.ok(s.proposal.value);assert.deepEqual(state.configuration,initial);assert.deepEqual(manual.candidate.value,initial);
        assert.equal(calls.filter(c=>c.path==='/api/config/validate').length,1);assert.equal(s.ready.value,false);
        await s.restoreSession();assert.equal(s.proposal.value.source,'explicit');
        s.facts.value.endpoint='http://127.0.0.2';await s.acceptProposal();assert.ok(s.setupIssues.value.length);assert.deepEqual(state.configuration,initial);
        s.facts.value.endpoint=fullFacts().endpoint;await s.acceptProposal();const accepted=clone(state.configuration);assert.equal(accepted.bindings.length,2);assert.equal(accepted.observations.length,1);assert.equal(s.ready.value,true);
        s.composer.value='http://127.0.0.2';await s.sendMessage();assert.ok(s.proposal.value);await s.rejectProposal();assert.deepEqual(state.configuration,accepted);
        failValidation=true;s.composer.value='http://127.0.0.3';await s.sendMessage();assert.equal(s.proposal.value,null);assert.deepEqual(manual.candidate.value,accepted);
        failValidation=false;s.composer.value='http://127.0.0.4';await s.sendMessage();assert.ok(s.proposal.value);state={...state,version:state.version+1};await s.acceptProposal();assert.ok(s.setupIssues.value.length);assert.deepEqual(state.configuration,accepted);
        assert.equal(calls.filter(c=>/copilot|setup\/message|apply|probe|command|poll|execute/.test(c.path)).length,0);
        assert.equal(calls.filter(c=>c.path==='/api/setup/draft').length,1,'only explicit successful Accept patches draft');
    } finally {globalThis.fetch=originalFetch;}
});
