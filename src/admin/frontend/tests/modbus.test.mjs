import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { clone, powerOnly, connectionActionSummary, connectionObservationSummary, connectionEndpoint, readableDiff } from '../.test-build/adapters/editing.js';
import { newModbusConnection, modbusConnectionError, modbusMappingError, ModbusDeviceDrafts, initialModbusDeviceDraft, projectModbusWizard } from '../.test-build/adapters/modbus.js';
import { createManualStore } from '../.test-build/stores/manual.js';
const base = JSON.parse(readFileSync(new URL('../../../../config/empty-home.json', import.meta.url)));
function fixture() {
    const c=clone(base); c.extension={kept:true};
    for(const id of ['a','b','c']) { const d={id,name:id,kind:'generic',room_id:null,enabled:true,state:[],action_ids:[]};c.devices.push(d);powerOnly(c,d); }
    return c;
}
function connection(c,type='modbus_tcp') { const m=newModbusConnection(c,type);m.name='Контроллер';if(type==='modbus_tcp')m.network.host='127.0.0.1';else m.serial.port='/dev/ttyUSB0';return m; }
function editing(c,id,conn) {
    const draft=initialModbusDeviceDraft(c,id,conn.type), device=c.devices.find(d=>d.id===id);
    draft.mappings=[{target_id:id,action_id:device.action_ids[0],executor:conn.type,connection_id:conn.id,modbus:{function:5,address:7,data_type:'boolean',value:true}}];
    draft.observing=true;draft.observeConnectionId=conn.id;draft.observation={function:1,address:9,data_type:'boolean',field:'power'};return draft;
}
test('Modbus TCP/RTU pure draft projection preserves input and produces explicit typed contracts with readable Review',()=>{
    for(const type of ['modbus_tcp','modbus_rtu']) {
        const c=fixture(), before=clone(c), m=connection(c,type), draft=editing(c,'a',m);
        const after=projectModbusWizard(c,m,type,[['a',draft]]);
        assert.deepEqual(c,before);assert.deepEqual(after.extension,{kept:true});
        assert.equal(after.connections[0].type,type);assert.equal(after.connections[0].modbus.unit_id,1);
        assert.match(connectionEndpoint(m),type==='modbus_tcp'?/127.0.0.1:502.*Unit 1/:/ttyUSB0.*9600.*even.*Unit 1/);
        assert.match(connectionActionSummary(after,'a',draft.mappings[0].action_id),/FC5.*адрес 7.*boolean.*true/);
        assert.match(connectionObservationSummary(after,'a'),/FC1.*адрес 9.*power/);
        const review=readableDiff(c,after).map(d=>d.afterText).join('\n');assert.match(review,/Modbus/);assert.match(review,/с нуля/);
    }
});
test('Modbus input validation rejects broadcast, unsupported serial ports, type confusion, ranges and multiwrite',()=>{
    const c=fixture(), m=connection(c);assert.equal(modbusConnectionError(m),'');
    for(const value of [0,248,1.5]) { const x=clone(m);x.modbus.unit_id=value;assert.ok(modbusConnectionError(x)); }
    for(const host of ['example.com','01.2.3.4','256.1.1.1','127.0.0.1:502']) { const x=clone(m);x.network.host=host;assert.ok(modbusConnectionError(x)); }
    for(const port of ['/dev/../etc/passwd','/dev/ttyUSB0/x','/dev/disk/by-id/test','/tmp/ttyUSB0']) { const x=connection(c,'modbus_rtu');x.serial.port=port;assert.ok(modbusConnectionError(x)); }
    const rtu=connection(c,'modbus_rtu');rtu.serial.baud_rate=12345;assert.ok(modbusConnectionError(rtu));
    const good={function:6,address:0,data_type:'int16',value:-32768};assert.equal(modbusMappingError(good),'');
    for(const changes of [{address:-1},{address:65536},{function:16},{value:-32769},{value:32768},{value:'1'},{value:true},{argument:'level'}]) assert.ok(modbusMappingError({...good,...changes}));
    assert.ok(modbusMappingError({function:5,address:0,data_type:'boolean',value:1}));assert.ok(modbusMappingError({function:1,address:0,data_type:'uint16',field:'power'},true));
});
test('Modbus supports one declared required parameter and rejects fixed parameterized commands or incompatible state fields',()=>{
    const c=fixture(), m=connection(c), draft=editing(c,'a',m), action=c.actions.find(a=>a.id===draft.mappings[0].action_id);
    action.parameters=[{id:'on',name:'Питание',type:'boolean',required:true}];
    assert.throws(()=>projectModbusWizard(c,m,m.type,[['a',draft]]),/параметром/);
    delete draft.mappings[0].modbus.value;draft.mappings[0].modbus.argument='on';
    assert.equal(projectModbusWizard(c,m,m.type,[['a',draft]]).bindings[0].modbus.argument,'on');
    action.parameters[0].required=false;assert.throws(()=>projectModbusWizard(c,m,m.type,[['a',draft]]),/обязательный/);
    action.parameters[0].required=true;draft.observation={function:3,address:9,data_type:'uint16',field:'power'};assert.throws(()=>projectModbusWizard(c,m,m.type,[['a',draft]]),/Свойство/);
});
test('mixed HTTP/MQTT/Modbus edits preserve other bindings and require explicit observation replacement',()=>{
    const c=fixture(), m=connection(c), d=editing(c,'a',m), a=c.devices[0];
    c.bindings=[{target_id:'a',action_id:a.action_ids[1],executor:'mqtt',connection_id:'broker',mqtt:{topic:'lamp/set',payload:'OFF',qos:1,retain:false}}, {target_id:'b',action_id:c.devices[1].action_ids[0],executor:'http',http:{method:'POST',url:'http://127.0.0.1/on',body:{on:true}}}];
    c.observations=[{target_id:'a',executor:'mqtt',connection_id:'broker',mqtt:{topic:'lamp/state',format:'text',qos:1,field:'power',true_payload:'ON',false_payload:'OFF'}}];
    const before=clone(c);assert.throws(()=>projectModbusWizard(c,m,m.type,[['a',d]]),/Подтвердите/);assert.deepEqual(c,before);
    d.replaceOtherObservation=true;const next=projectModbusWizard(c,m,m.type,[['a',d]]);assert.ok(next.bindings.some(b=>JSON.stringify(b)===JSON.stringify(c.bindings[0])));assert.ok(next.bindings.some(b=>JSON.stringify(b)===JSON.stringify(c.bindings[1])));assert.equal(next.observations.length,1);assert.equal(next.observations[0].executor,'modbus_tcp');
    d.mappings[0].action_id=a.action_ids[1];assert.throws(()=>projectModbusWizard(c,m,m.type,[['a',d]]),/другого протокола/);
});
test('Modbus A/B/A draft navigation retains edits, keeps old connections pinned, and leaves unvisited devices unchanged',()=>{
    let c=fixture();const a=connection(c), d=editing(c,'a',a);c=projectModbusWizard(c,a,a.type,[['a',d]]);c.observations[0].extension='retained';
    const b=connection(c);b.id='other';b.name='Другой контроллер';b.network.host='127.0.0.2';c.connections.push(b);
    const drafts=new ModbusDeviceDrafts(c,a.type), da=drafts.restore('a');da.mappings[0].modbus.address=100;drafts.remember('a',da);
    const db=editing(c,'b',b);db.mappings[0].modbus.address=200;drafts.remember('b',db);const again=drafts.restore('a');assert.equal(again.mappings[0].modbus.address,100);
    const next=projectModbusWizard(c,b,b.type,drafts.changed('a',again));assert.equal(next.bindings.find(m=>m.target_id==='a').connection_id,a.id);assert.equal(next.observations.find(o=>o.target_id==='a').connection_id,a.id);assert.equal(next.observations.find(o=>o.target_id==='a').extension,'retained');assert.equal(next.bindings.find(m=>m.target_id==='b').modbus.address,200);assert.deepEqual(next.devices.find(x=>x.id==='c'),c.devices.find(x=>x.id==='c'));
    assert.equal(drafts.changed('a',again).length,2);assert.equal(c.bindings[0].modbus.address,7);
});
test('Modbus Cancel and final Review use real manual store: zero hardware calls and exactly one CAS Save then Apply',async()=>{
    for(const cancel of [true,false]) {
        const configuration=fixture();let stored={ok:true,configuration,revision:1,hash:'a'},active=clone(stored);const calls=[];
        const api=async(path,body,method)=>{calls.push({path,body,method});if(path==='/api/setup/status')return{ok:true,active:false,status:'idle'};if(path==='/api/config/validate')return{ok:true,valid:true,apply_ready:true};if(path==='/api/config/diff')return{ok:true,changes:[]};if(path==='/api/config'&&method==='PUT'){assert.equal(body.expected.hash,'a');return stored={ok:true,configuration:clone(body.configuration),revision:2,hash:'b'};}if(path==='/api/config/apply'){assert.equal(body.expected.hash,'b');active=clone(stored);return{ok:true};}throw Error('Unexpected '+path);};
        const store=createManualStore(api,async path=>path==='/api/config'?stored:{ok:true,active},async()=>{});
        await store.begin('connection','a');const c=store.task.value.buffer,m=connection(c);Object.assign(c,projectModbusWizard(c,m,m.type,[['a',editing(c,'a',m)]]));
        assert.equal(stored.configuration.connections?.length??0,0);assert.equal(store.candidate.value.connections?.length??0,0);
        if(cancel){store.cancel();assert.equal(store.candidate.value.connections?.length??0,0);}else{assert.equal(await store.stage(),true);await store.prepare();assert.equal(calls.filter(c=>c.method==='PUT'||c.path==='/api/config/apply').length,0);await store.confirm();assert.equal(active.configuration.connections[0].type,'modbus_tcp');}
        assert.equal(calls.filter(c=>c.method==='PUT').length,cancel?0:1);assert.equal(calls.filter(c=>c.path==='/api/config/apply').length,cancel?0:1);assert.equal(calls.some(c=>/probe|command|poll|execute/.test(c.path)),false);
    }
});
