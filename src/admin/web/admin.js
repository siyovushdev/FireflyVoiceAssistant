'use strict';
const $ = id => document.getElementById(id);
let draft, expected, dirty = false, operation = false;
const json = value => JSON.stringify(value, null, 2);
const words = value => value.split('\n').map(x => x.trim()).filter(Boolean);
function errors(items = []) {
    $('errors').replaceChildren(...items.map(item => {
        const li = document.createElement('li'); li.textContent = `${item.path || '/'} · ${item.code}: ${item.message}`; return li;
    }));
}
async function api(path, method = 'GET', data) {
    const response = await fetch(path, {method, headers: {'Content-Type':'application/json', 'X-Firefly-Admin':'1'}, body: data === undefined ? undefined : JSON.stringify(data)});
    const result = await response.json();
    if (!response.ok || result.ok === false) { errors(result.errors || [{code:'http_error', message:`HTTP ${response.status}`}]); const error=new Error('request rejected'); error.response=result; throw error; }
    return result;
}
function modified() { dirty = true; $('draft-state').textContent = 'Есть несохранённые изменения'; $('validation').textContent = 'Черновик изменён — требуется Validate'; $('raw-config').textContent = json(draft); }
function field(parent, name, value, setter, options = {}) {
    const label = document.createElement('label'); label.textContent = name;
    const input = document.createElement(options.multiline ? 'textarea' : options.choices ? 'select' : 'input');
    if (options.choices) for (const choice of options.choices) { const option = document.createElement('option'); option.value = choice.id; option.textContent = choice.name; input.append(option); }
    if (options.number) input.type = 'number';
    input.value = value ?? ''; input.readOnly = !!options.readonly;
    input.setAttribute('aria-label', name); label.append(input); parent.append(label);
    input.addEventListener('input', () => { setter(options.number ? Number(input.value) : input.value); modified(); });
    if (options.refresh) input.addEventListener('change', render);
    return input;
}
function card(parent, title) { const node = document.createElement('div'); node.className = 'card'; const h = document.createElement('h3'); h.textContent = title; node.append(h); parent.append(node); return node; }
function removeButton(node, array, value, protectedId) {
    if (protectedId) { const text = document.createElement('small'); text.textContent = 'Базовая identity M4 закреплена'; node.append(text); return; }
    const button = document.createElement('button'); button.className = 'remove'; button.textContent = 'Удалить';
    button.onclick = () => { array.splice(array.indexOf(value),1); modified(); render(); }; node.append(button);
}
function render() {
    if (!draft) return;
    for (const id of ['rooms','devices','actions','entrance']) $(id).replaceChildren();
    const fixedTargets = Object.values(draft.compatibility.light_targets);
    const fixedRooms = draft.devices.filter(d => fixedTargets.includes(d.id)).map(d => d.room_id);
    for (const room of draft.rooms) {
        const node = card($('rooms'), room.name || 'Новая комната');
        const pinned = fixedRooms.includes(room.id);
        field(node,'Room ID',room.id,v=>room.id=v,{readonly:pinned,refresh:true});
        field(node,'Название комнаты',room.name,v=>room.name=v,{refresh:true});
        field(node,'Aliases / mentions (по одному в строке)',room.speech.mentions.join('\n'),v=>room.speech.mentions=words(v),{multiline:true});
        field(node,'Locations (по одной фразе в строке)',room.speech.locations.join('\n'),v=>room.speech.locations=words(v),{multiline:true});
        removeButton(node,draft.rooms,room,pinned);
    }
    for (const device of draft.devices) {
        const door = device.kind === 'entrance_door';
        const node = card($(door ? 'entrance' : 'devices'), device.name || 'Новый light target');
        const pinned = fixedTargets.includes(device.id) || draft.compatibility.door_target_id === device.id;
        field(node,'Target ID',device.id,v=>device.id=v,{readonly:pinned,refresh:true});
        field(node,'Название устройства',device.name,v=>device.name=v,{refresh:true});
        if (!door) {
            field(node,'Комната',device.room_id,v=>device.room_id=v,{choices:draft.rooms.map(r=>({id:r.id,name:`${r.name} (${r.id})`}))});
            field(node,'Action',device.action_ids[0],v=>device.action_ids=[v],{choices:draft.actions.filter(a=>a.operation==='light.set_brightness.v1').map(a=>({id:a.id,name:`${a.name} (${a.id})`}))});
        } else { const p = document.createElement('p'); p.textContent = device.action_ids.join(', '); node.append(p); }
        removeButton(node,draft.devices,device,pinned);
    }
    for (const action of draft.actions) {
        const node = card($('actions'),action.name);
        const pinned = [draft.compatibility.light_action_id,draft.compatibility.door_action_id].includes(action.id);
        field(node,'Action ID',action.id,v=>action.id=v,{readonly:pinned,refresh:true});
        field(node,'Название действия',action.name,v=>action.name=v,{refresh:true});
        const p = document.createElement('p'); p.className = 'muted'; p.textContent = action.operation; node.append(p);
        for (const argument of action.parameters) {
            const range = document.createElement('div'); range.className = 'range'; node.append(range);
            field(range,`${argument.id} minimum`,argument.minimum,v=>argument.minimum=v,{number:true});
            field(range,`${argument.id} maximum`,argument.maximum,v=>argument.maximum=v,{number:true});
        }
        removeButton(node,draft.actions,action,pinned);
    }
    $('raw-config').textContent = json(draft);
}
async function load() {
    const result = await api('/api/config'); draft = result.configuration;
    expected = {revision:result.revision,hash:result.hash}; dirty = false;
    $('revision').textContent = `Schema v${draft.schema_version} · stored revision ${result.revision}`;
    $('draft-state').textContent = 'Черновик соответствует stored'; render();
}
async function status() {
    try {
        const result = await api('/api/status');
        $('status').textContent = `Runtime: ${result.runtime_busy?'занят':result.runtime_ready?'готов':'недоступен'} · active ${result.active?.revision ?? '—'} · stored ${result.stored?.revision ?? '—'} · ${result.needs_apply?'нужен Apply':'active = stored'}${result.store_error ? ' · '+result.store_error : ''}`;
    } catch (_) { $('status').textContent = 'Нет соединения с Admin'; }
}
async function run(fn) {
    if (operation) return; operation = true; errors();
    document.querySelectorAll('button,input,textarea,select').forEach(b=>b.disabled=true);
    try { await fn(); } catch (error) { if (error.message !== 'request rejected') errors([{code:'connection_error',message:'Операция не завершена. Проверьте соединение и status; перед повторным Save загрузите stored.'}]); }
    finally { operation=false; document.querySelectorAll('button,input,textarea,select').forEach(b=>b.disabled=false); await status(); }
}
$('reload').onclick=()=>run(async()=>{if(!dirty || confirm('Заменить несохранённый черновик stored-конфигурацией?')) await load();});
$('validate').onclick=()=>run(async()=>{const r=await api('/api/config/validate','POST',{configuration:draft});$('validation').textContent=`Конфигурация валидна · context ${r.context_bytes} bytes · NPU preflight: ${r.runtime_preflight}. ${r.apply_ready?'Apply admission проверен.':'Точный token admission будет выполнен при Apply; runtime пока недоступен.'}`;});
$('save').onclick=()=>run(async()=>{await api('/api/config','PUT',{configuration:draft,expected});await load();$('validation').textContent='Сохранено атомарно. Active runtime не изменён. Следующий шаг — Apply stored.';});
$('apply').onclick=()=>run(async()=>{if(dirty) {errors([{code:'unsaved_draft',message:'Сначала Save или загрузите stored. Apply использует только сохранённую ревизию.'}]);return;}await api('/api/config/apply','POST',{expected});$('validation').textContent='Apply завершён: новая immutable generation активна.';});
function showCommand(r) {
    $('test-summary').textContent = `${r.route} · ${r.result.unsupported?'UNSUPPORTED':'ACTION'} · Safety ${r.safety} · Domain ${r.domain} · revision ${r.revision}`;
    const diagnostic = {input:r.input,route:r.route,result:r.result,safety:r.safety,domain:r.domain,revision:r.revision,
        latency_ms:r.latency_ms,candidate:r.candidate,decode:r.decode,safety_result:r.safety_result};
    if ('raw_rkllm' in r) diagnostic.raw_rkllm = r.raw_rkllm;
    $('test-result').textContent = json(diagnostic);
}
$('test-form').onsubmit = event => {
    event.preventDefault();
    run(async () => {
        $('test-summary').textContent = 'Выполняется…';
        $('test-result').textContent = '';
        try { showCommand(await api('/api/test-command','POST',{text:$('command').value})); }
        catch (error) {
            if (error.response?.result) showCommand(error.response);
            else $('test-summary').textContent = 'Запрос не выполнен. См. список ошибок; предыдущий результат сброшен.';
            throw error;
        }
    });
};
$('add-room').onclick=()=>{draft.rooms.push({id:'',name:'',speech:{mentions:[],locations:[]}});modified();render();$('rooms').lastElementChild.scrollIntoView({block:'center'});};
$('add-device').onclick=()=>{draft.devices.push({id:'',name:'',kind:'light',room_id:draft.rooms.at(-1)?.id || null,enabled:true,action_ids:[draft.compatibility.light_action_id],spoken_aliases:[]});modified();render();};
$('add-action').onclick=()=>{const action=structuredClone(draft.actions.find(a=>a.operation==='light.set_brightness.v1'));action.id='';action.name='';draft.actions.push(action);modified();render();};
$('backups').onclick=()=>run(async()=>{const r=await api('/api/config/revisions');$('backup-revision').replaceChildren(...r.revisions.map(x=>{const option=document.createElement('option');option.value=x.revision;option.textContent=`Revision ${x.revision} · ${x.hash.slice(0,12)}`;return option;}));$('recovery').textContent=json(r);});
$('restore').onclick=()=>run(async()=>{if(!confirm('Сохранить backup новой ревизией? Active runtime изменится только после Apply.'))return;const r=await api('/api/config/restore','POST',{revision:Number($('backup-revision').value),expected});$('recovery').textContent=json(r);await load();});
$('reconcile').onclick=()=>run(async()=>{const r=await api('/api/config/reconcile','POST',{});$('recovery').textContent=json(r);await load();});
$('show-active').onclick=()=>run(async()=>{const r=await api('/api/config/active');$('active-context').textContent=r.active?.context || 'Нет активного runtime';});
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});
run(load);setInterval(status,4000);
