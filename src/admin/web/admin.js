'use strict';
const $=id=>document.getElementById(id), M=window.AdminModel, json=v=>JSON.stringify(v,null,2);
let draft, stored, expected, activeConfig, activeRevision, dirty=false, validated=false, operation=false, savedIds=new Set(), wizard, templates=[], pendingPlan, pendingCopilot;
let setupToken=sessionStorage.getItem('fireflySetup')||'', setupActive=false, setupVersion=0, setupSnapshot='', setupReadOnly=false;
let voiceState='idle', voiceRec=null, sttReady=false, voiceIdleMessage='', voiceAttempt=0, voiceClosing=false;
const advanced=()=>document.body.dataset.mode==='advanced';
const node=(tag,text,cls)=>{const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(cls)n.className=cls;return n;};
function errors(items=[]) {
    const display=()=>items.map(e=>{const li=node('li',M.humanError(e,draft));li.append(node('pre',`${e.code} ${e.path||'/'}\n${e.message||''}`,'advanced'));return li;});
    $('errors').replaceChildren(...display());if($('wizard').open)$('wizard-errors').replaceChildren(...display());
}
async function api(path,method='GET',data) {
    const aiRoutes=['/api/admin/language-profile','/api/admin/plan','/api/admin/copilot','/api/admin/action-draft','/api/admin/draft','/api/admin/suggest'];
    if(method==='POST'&&aiRoutes.includes(path)&&!setupActive)
        throw Object.assign(new Error('setup_required'),{response:{errors:[{code:'setup_required'}]}});
    if(setupReadOnly&&method!=='GET')throw new Error('Настройка открыта в другом клиенте. Здесь доступен только просмотр.');
    if(setupActive&&data?.configuration&&!path.startsWith('/api/setup/')&&json(draft)!==setupSnapshot)await syncSetupDraft();
    if(setupActive&&data&&method!=='GET')data={...data,session_id:setupToken};
    const response=await fetch(path,{method,headers:{'Content-Type':'application/json','X-Firefly-Admin':'1'},body:data===undefined?undefined:JSON.stringify(data)});
    const result=await response.json();
    if(result.trace){$('ai-detail').textContent=json({retry_count:result.retry_count,trace:result.trace,suggestion:result.suggestion,errors:result.errors});$('ai-notice').textContent=result.retry_count?(result.ok?'Один из ответов ИИ не прошёл проверку. Повторная попытка подготовила корректный черновик.':'После повторной попытки ИИ не смог завершить предложение. Можно заполнить вручную.'):'Предложение ИИ нужно проверить и подтвердить.';}
    if(!response.ok||result.ok===false){const e=new Error('request rejected');e.response=result;throw e;}
    return result;
}
async function run(fn) {
    if(operation)return;operation=true;errors();$('progress').hidden=false;
    const controls=[...document.querySelectorAll('button,input,textarea,select')].map(e=>[e,e.disabled]);
    controls.forEach(([e])=>{if(e.id!=='setup-cancel'||!setupActive)e.disabled=true;});
    try{await fn();}catch(e){errors(e.response?.errors||[{code:'connection_error',message:e.message}]);if(e.response?.raw_rkllm||e.response?.trace)$('errors').append(node('pre',json(e.response.trace||e.response.raw_rkllm),'advanced'));}
    finally{operation=false;controls.forEach(([e,disabled])=>{if(e.isConnected)e.disabled=disabled;});$('progress').hidden=true;applySetupAccess();status();}
}
function validFields(host){const invalid=[...host.querySelectorAll('input,textarea,select')].find(e=>!e.checkValidity());if(invalid){invalid.reportValidity();return false;}return true;}
function modified(){dirty=true;validated=false;$('draft-state').textContent='Есть изменения в черновике';$('validation').textContent='Проверьте черновик перед сохранением.';$('raw-config').textContent=json(draft);}
function card(parent,title){const n=node('div',undefined,'card');n.append(node('h3',title));parent.append(n);return n;}
function button(parent,text,fn,cls){const b=node('button',text,cls);b.type='button';b.onclick=fn;parent.append(b);return b;}
function field(parent,label,value,set,opts={}) {
    const l=node('label',label,opts.advanced?'advanced':'');
    const input=node(opts.choices?'select':opts.multiline?'textarea':'input');
    if(opts.choices)for(const choice of opts.choices){const o=node('option',choice.name);o.value=choice.id;input.append(o);}
    if(opts.number)input.type='number';if(opts.checkbox)input.type='checkbox';
    if(opts.checkbox)input.checked=!!value;else input.value=value??'';
    input.readOnly=!!opts.readonly;input.setAttribute('aria-label',label);if(opts.id)input.id=opts.id;
    input.oninput=()=>{try{set(opts.checkbox?input.checked:opts.number?M.scalarNumber(input.value):input.value);input.setCustomValidity('');if(wizard&&$('wizard').open&&!['new-name','new-id'].includes(input.id))wizard.edited=true;if(!opts.local)modified();}catch(error){input.setCustomValidity(error.message);input.reportValidity();}};
    if(opts.change)input.onchange=opts.change;
    l.append(input);parent.append(l);return input;
}
const editorChanged=()=>{if(!wizard||!$('wizard').open)modified();};
const typed=window.TypedEditor.create({node,field,button,changed:editorChanged});
const simple=window.SimpleEditor.create({node,field,button,changed:editorChanged});
function ensureMeta(entity){if(!entity.metadata)entity.metadata={type:'',description:'',aliases:[]};return entity.metadata;}
const locationChoices=(exclude)=>[{id:'',name:'Без родителя'},...draft.rooms.filter(r=>r.id!==exclude).map(r=>({id:r.id,name:r.name}))];
function stableField(parent,entity){field(parent,'Постоянный идентификатор',entity.id,v=>entity.id=v,{readonly:savedIds.has(entity.id),advanced:true});}
function removeEntity(parent,array,entity,pinned){if(pinned){parent.append(node('small','System ID · встроенная сущность','advanced'));return;}
    button(parent,'Удалить',()=>{if(confirm(`Удалить «${entity.name}» из черновика?`)){array.splice(array.indexOf(entity),1);modified();render();}},'remove');}
function renderTree(){
    $('tree').replaceChildren();const visited=new Set();
    function level(parent,host){const ul=node('ul');host.append(ul);for(const r of draft.rooms.filter(r=>(M.metadata(r).parent_id||null)===parent)){
        if(visited.has(r.id))continue;visited.add(r.id);const li=node('li',`▧ ${r.name}`);ul.append(li);level(r.id,li);
        for(const d of draft.devices.filter(d=>d.room_id===r.id))li.append(node('div',`↳ ${d.name}`));}}
    level(null,$('tree'));for(const r of draft.rooms.filter(r=>!visited.has(r.id)))$('tree').append(node('p',`⚠ ${r.name}: проверьте родителя`));
    for(const d of draft.devices.filter(d=>!d.room_id))$('tree').append(node('p',`◇ ${d.name}`));
}
function languagePanel(host,entity,item){
    host.append(node('h4','Язык команд'));
    const preview=node('div');host.append(preview);
    button(host,'Предложить варианты',()=>run(async()=>{
        const snapshot=json(draft);
        const r=await api('/api/admin/language-profile','POST',{configuration:draft,entity,id:item.id});
        if(snapshot!==json(draft))throw new Error('Черновик изменился. Повторите предложение.');
        const proposal=M.clone(r.suggestion);preview.replaceChildren(node('p','Предложение ИИ. Отредактируйте и подтвердите; сохранение выполняется отдельно.'));
        for(const key of ['aliases','mentions','locations'])if(proposal[key])field(preview,key,proposal[key].join('\n'),v=>proposal[key]=M.list(v),{multiline:true,local:true});
        preview.append(node('pre',json(proposal.conflicts||[])));
        button(preview,'Проверить и добавить в черновик',()=>run(async()=>{
            if(snapshot!==json(draft))throw new Error('Черновик изменился. Повторите предложение.');
            const next=M.clone(draft),target=next[entity==='location'?'rooms':entity==='device'?'devices':'actions'].find(e=>e.id===item.id);
            if(entity==='location'){target.speech.mentions=proposal.mentions;target.speech.locations=proposal.locations;ensureMeta(target).aliases=proposal.aliases;}
            else if(entity==='device')target.spoken_aliases=proposal.aliases;
            else target.speech_aliases=proposal.aliases;
            const checked=await api('/api/config/validate','POST',{configuration:next});draft=next;modified();render();
            $('validation').textContent='Язык добавлен в черновик. '+json(checked.language_conflicts||[]);
        }));
    }));
}
function render(){
    if(!draft)return;for(const id of ['rooms','devices','actions'])$(id).replaceChildren();
    const pinnedTargets=Object.values((draft.compatibility?.light_targets||{})),pinnedRooms=draft.devices.filter(d=>pinnedTargets.includes(d.id)).map(d=>d.room_id);
    for(const r of draft.rooms){const n=card($('rooms'),r.name);stableField(n,r);if(M.metadata(r).origin==='local_ai')n.append(node('span','AI suggested · User confirmed','badge'));
        field(n,'Название зоны',r.name,v=>r.name=v,{change:renderTree});
        field(n,'Родительская зона',M.metadata(r).parent_id||'',v=>ensureMeta(r).parent_id=v||null,{choices:locationChoices(r.id),change:renderTree});
        field(n,'Тип зоны',M.metadata(r).type||'room',v=>ensureMeta(r).type=v);
        field(n,'Описание',M.metadata(r).description||'',v=>ensureMeta(r).description=v);
        field(n,'Другие обозначения зоны',M.metadata(r).aliases?.join('\n')||'',v=>ensureMeta(r).aliases=M.list(v),{multiline:true});
        if(!M.metadata(r).container){field(n,'Слова зоны (по одному в строке)',r.speech.mentions.join('\n'),v=>r.speech.mentions=M.list(v),{multiline:true});field(n,'Фразы места (по одной в строке)',r.speech.locations.join('\n'),v=>r.speech.locations=M.list(v),{multiline:true});}
        else n.append(node('p','Группа зон · команды адресуются дочерним местам'));
        languagePanel(n,"location",r);
        removeEntity(n,draft.rooms,r,pinnedRooms.includes(r.id));
    }
    for(const d of draft.devices){const n=card($('devices'),d.name);stableField(n,d);if(M.metadata(d).origin==='local_ai')n.append(node('span','AI suggested · User confirmed','badge'));
        field(n,'Название устройства',d.name,v=>d.name=v,{change:renderTree});
        field(n,'Тип оборудования',M.metadata(d).type||d.kind,v=>ensureMeta(d).type=v);
        field(n,'Описание',M.metadata(d).description||'',v=>ensureMeta(d).description=v);
        if(d.kind!=='entrance_door')field(n,'Зона устройства',d.room_id||'',v=>d.room_id=v||null,{choices:locationChoices(),change:renderTree});
        field(n,'Обозначения устройства',M.metadata(d).aliases?.join('\n')||'',v=>ensureMeta(d).aliases=M.list(v),{multiline:true});
        field(n,'Словоформы устройства',d.spoken_aliases.join('\n'),v=>d.spoken_aliases=M.list(v),{multiline:true});
        n.append(node('small','Виртуальное устройство · обозначения участвуют в понимании команд.'));
        if(d.kind==='light')field(n,'Доступное действие',d.action_ids[0],v=>d.action_ids=[v],{choices:draft.actions.filter(a=>a.operation==='light.set_brightness.v1').map(a=>({id:a.id,name:a.name}))});
        if(d.kind==='generic'){
            n.append(node('h4','Что умеет устройство'));
            if(advanced())typed.parameters(n,d.state,render,true);
            else {for(const id of d.action_ids){const a=draft.actions.find(a=>a.id===id);if(a){n.append(node('strong',a.name));simple.capability(n,a,d);}}if(!d.action_ids.length)n.append(node('p','Добавьте действие и опишите, что должно измениться.'));
                if(d.state.length){n.append(node('h4','Допустимые значения'));for(const p of d.state)n.append(node('p',`${p.name}: ${M.constraintText(p)}`));button(n,'Изменить допустимые значения',()=>openConstraints('device:'+d.id));}}
            button(n,'Настроить по шаблону',()=>openExistingDevice(d));
            button(n,'Добавить действие',()=>openActionWizard(d.id));
        }
        if(d.kind==='entrance_door')n.append(node('p','Открыть входную дверь · только явная команда с указанием входа.'));
        if(!Array.isArray(draft.bindings))draft.bindings=[];
        simple.execution(n,advanced(),d,draft,openConnectionEditor);
        languagePanel(n,"device",d);
        removeEntity(n,draft.devices,d,pinnedTargets.includes(d.id)||d.id===draft.compatibility?.door_target_id);
    }
    for(const a of draft.actions){const n=card($('actions'),a.name);stableField(n,a);field(n,'Название действия',a.name,v=>a.name=v);n.append(node('small',a.operation,'advanced'));
        const targets=draft.devices.filter(d=>d.action_ids.includes(a.id));n.append(node('p',`Устройства: ${targets.map(d=>d.name).join(', ')||'не назначены'}`));
        if(a.operation==='mock.assign.v1'){if(advanced()){n.append(node('p','Виртуальная операция · только присваивания полям состояния'));typed.action(n,a,targets[0],render);}else {simple.capability(n,a,targets[0]);field(n,'Описание результата',a.description,v=>a.description=v);field(n,'Как попросить об этом',a.speech_aliases.join('\n'),v=>a.speech_aliases=M.list(v),{multiline:true});if(a.parameters.length)button(n,'Допустимые значения',()=>openConstraints('action:'+a.id));}}
        else {n.append(node('p',a.operation==='light.set_brightness.v1'?'Смысл операции: установить яркость. Переименование не меняет этот смысл.':'Смысл операции: открыть защищённую входную дверь. Обязательна явная команда с указанием входа.'));
            for(const p of a.parameters){field(n,`Минимум${p.unit==='percent'?', %':''}`,p.minimum,v=>p.minimum=v,{number:true});field(n,`Максимум${p.unit==='percent'?', %':''}`,p.maximum,v=>p.maximum=v,{number:true});}}
        languagePanel(n,"action",a);
        removeEntity(n,draft.actions,a,[draft.compatibility?.light_action_id,draft.compatibility?.door_action_id].includes(a.id));}
    renderTree();$('raw-config').textContent=json(draft);
}
async function migrateCandidate(){if(draft.schema_version===1){const r=await api('/api/config/migrate','POST',{configuration:draft});draft=r.configuration;modified();}}
async function load(){const r=await api('/api/config');draft=M.clone(r.configuration);stored=M.clone(draft);expected={revision:r.revision,hash:r.hash};savedIds=new Set(M.occupied(draft));dirty=false;validated=false;$('revision').textContent=`Schema v${draft.schema_version} · revision ${r.revision}`;$('draft-state').textContent='Черновик соответствует сохранённой версии';await migrateCandidate();render();}

function dashboardCard(title,body){const n=card($('dashboard'),title);n.append(node('p',body));return n;}
async function status(){
    if(!draft)return;
    try{const [r,states,history]=await Promise.all([api('/api/status'),api('/api/devices/state'),api('/api/executions')]);
        activeConfig=states.active?.configuration||activeConfig||stored;activeRevision=r.active?.revision;$('dashboard').replaceChildren();
        sttReady=!!r.stt_ready;
        dashboardCard('Помощник',r.runtime_busy?'Обрабатывает запрос':r.runtime_ready?'Готов к командам':'Модель недоступна');
        const health=M.healthSummary?M.healthSummary(r):{tone:'degraded',title:'Статус',detail:''};
        const healthCard=dashboardCard('Состояние системы',health.title);if(health.detail)healthCard.append(node('small',health.detail));
        dashboardCard('Голос',r.voice==='disabled'?'Выключен':r.voice==='unavailable'?'Микрофон недоступен':`Включён · ${r.microphone}`);
        dashboardCard('Голос помощника',sttReady?'Локальный STT готов к записи из браузера':'STT недоступен — только текст');
        const executorCard=dashboardCard('Устройства',`${states.devices.length} · playground: Mock`);const lastExecution=history.executions.at(-1);if(lastExecution)executorCard.append(node('small',`Последнее выполнение: ${new Date(lastExecution.timestamp_ms).toLocaleTimeString()} · ${activeConfig.devices.find(d=>d.id===lastExecution.execution.targetId)?.name||'Устройство'}`));
        const connected=(draft.bindings||[]).length;dashboardCard('Подключения',connected?`${connected} HTTP в черновике`:'Не настроены');
        dashboardCard('Конфигурация',dirty?'Есть изменения в черновике':r.needs_apply?'Сохранена новая версия — нужно применить':'Сохранённая версия применяется');
        $('status').textContent=`Время работы: ${Math.floor((r.uptime_seconds||0)/60)} мин. · AI ${r.runtime_ready?'доступен':'suggestions unavailable'}`;
        $('status-detail').textContent=json({...r,vad:r.voice==='disabled'?'disabled':r.voice==='unavailable'?'unavailable':'Silero',stt:r.voice==='disabled'?'disabled':r.voice==='unavailable'?'unavailable':'Russian Zipformer'});
        $('last-command').textContent=(()=>{
            const c=r.last_command;if(!c?.input)return '';
            const ex=c.execution;
            if(!ex)return `Последняя команда: ${c.input}`;
            if(ex.success||ex.status==='accepted'){
                const brightness=ex.current?.brightness;
                const expected=brightness===undefined?'':' · Ожидается: brightness '+brightness;
                return `Команда: ${c.input} · Результат: Accepted${expected}`;
            }
            return `Команда: ${c.input} · Не выполнено · ${ex.error||'ошибка'}`;
        })();
        $('voice-detail').textContent=r.last_voice?json(r.last_voice):'Голосовых запросов ещё не было.';
        renderStates(states.devices,history.executions);
        if(voiceState==='idle') setVoiceUi();
    }catch(e){$('status').textContent='Нет соединения с Firefly';}
}
function renderStates(devices,history){$('device-state').replaceChildren();const config=activeConfig||stored;
    for(const d of devices){const def=config.devices.find(x=>x.id===d.targetId),zone=config.rooms.find(x=>x.id===def?.room_id),last=history.filter(x=>x.execution.targetId===d.targetId).at(-1),n=card($('device-state'),d.name);
        n.append(node('p',`${zone?.name||'Общая зона'} · ${M.typeName(M.metadata(def||{}).type||def?.kind||'Устройство')}`),
            node('h3',M.productionStateText?M.productionStateText(d,def):M.stateText(d.state,def)));
        if(def)n.append(node('small',M.connectionStatusText(M.connectionCompleteness(def,config))));
        n.append(node('small',def?.action_ids.map(id=>config.actions.find(a=>a.id===id)?.name||id).join(', ')||'Нет исполняемых операций'),
            node('small',last?`Последнее действие: ${config.actions.find(a=>a.id===last.execution.actionId)?.name||'Действие'} · ${new Date(last.timestamp_ms).toLocaleString()}`:'С момента запуска действий не было'));
        n.append(node('pre',json(d),'advanced'));}
    $('execution-history').replaceChildren(...history.slice().reverse().map(e=>{const n=node('p'),x=e.execution;n.textContent=`${new Date(e.timestamp_ms).toLocaleTimeString()} · ${config.devices.find(d=>d.id===x.targetId)?.name||'Удалённое устройство'} · ${M.stateText(x.previous,config.devices.find(d=>d.id===x.targetId))} → ${M.stateText(x.current,config.devices.find(d=>d.id===x.targetId))} · ${x.success?'Выполнено':'Ошибка'}`;n.append(node('small',`${e.source} · generation ${e.generation_id} · event ${e.sequence}`,'advanced'));return n;}));
    if(!history.length)$('execution-history').append(node('p','Действий пока нет. Предварительные проверки сюда не добавляются.'));
}
function openConnectionEditor(device){
    if(!Array.isArray(draft.bindings))draft.bindings=[];
    const current=draft.devices.find(d=>d.id===device.id)||device;
    simple.connectionEditor($('connection-content'),current,draft,advanced());
    $('connection-dialog').showModal();
}
$('connection-close').onclick=$('connection-done').onclick=()=>{$('connection-dialog').close();render();};
async function validate(){if(!validFields(document))throw Object.assign(new Error('invalid form'),{response:{errors:[{code:'invalid_form',human:'Исправьте отмеченные поля.'}]}});const r=await api('/api/config/validate','POST',{configuration:draft});validated=true;$('validation').textContent=r.apply_ready?'✓ Конфигурация проверена. Можно сохранить.':'✓ Структура конфигурации проверена. Модель недоступна; можно сохранить, а применить после её восстановления.';if(r.language_conflicts?.length)$('validation').textContent+=' Язык: '+json(r.language_conflicts);if(r.language_examples?.length)$('validation').textContent+=' Примеры: '+r.language_examples.join('; ');return r;}
async function diff(base='active'){return api('/api/config/diff','POST',{configuration:draft,base});}
function showDiff(changes,title,canApply=false){$('diff-title').textContent=title;$('diff-list').replaceChildren(...M.diffLines(changes,draft).map(x=>node('li',x)));if(!changes.length)$('diff-list').append(node('li','Изменений нет.'));$('diff-raw').textContent=json(changes);$('confirm-apply').hidden=!canApply;$('diff-dialog').showModal();}
$('reload').onclick=()=>run(async()=>{if(!dirty||confirm('Заменить несохранённый черновик?'))await load();});
$('validate').onclick=()=>run(validate);
$('save').onclick=()=>run(async()=>{if(!validated)await validate();await api('/api/config','PUT',{configuration:draft,expected});await load();$('validation').textContent='✓ Версия сохранена. Нажмите «Применить», чтобы использовать её для команд.';});
$('apply').onclick=()=>run(async()=>{if(dirty){$('validation').textContent='Сначала проверьте и сохраните черновик.';return;}showDiff((await diff()).changes,'Будет применено',true);});
$('confirm-apply').onclick=()=>run(async()=>{await api('/api/config/apply','POST',{expected});$('diff-dialog').close();$('validation').textContent='✓ Изменения применены. Помощник продолжает работать.';await status();});
$('diff-close').onclick=()=>$('diff-dialog').close();
$('test-form').onsubmit=e=>{e.preventDefault();const execute=e.submitter?.id==='execute';const production=!!$('production-execute')?.checked;
run(async()=>{$('test-summary').textContent=execute?(production?'Production-выполнение…':'Виртуальное выполнение…'):'Проверка без выполнения…';$('test-result').textContent='';$('executor-contract').textContent='Проверка…';
    try{const body={text:$('command').value,execute};if(execute&&production)body.production=true;
    const r=await api('/api/test-command','POST',body);const summary=M.commandSummary(r,activeConfig||stored),device=(activeConfig||stored).devices.find(d=>d.id===r.result?.targetId);$('executor-contract').textContent=M.executorContract(r.result)?json(M.executorContract(r.result)):'Исполняемое действие не сформировано (Unsupported).';$('test-summary').replaceChildren(node('p','Language route: '+(r.language_route||'—')+' / '+(r.index_status||'—')));const box=node('div',undefined,summary.execution==='Не выполнено'?'error':'success');
    box.append(node('p',`Команда: ${$('command').value}`));
    for(const [key,label]of Object.entries({message:'Понимание',device:'Устройство',action:'Действие',value:'Значение',safety:'Безопасность',execution:'Результат',state:'Состояние'}))if(summary[key])box.append(node('p',`${label}: ${summary[key]}`));
    if(r.execution&&!production)box.append(node('p',`${M.stateText(r.execution.previous,device)} → ${M.stateText(r.execution.current,device)}`));
    else if(!r.execution&&summary.understood)box.append(node('p',`Предполагаемое изменение: ${M.stateText(r.predicted_execution?.previous,device)} → ${M.stateText(r.predicted_execution?.current,device)}. Состояние и история не изменены.`));
    box.append(node('p',production?'Режим: Production (binding → Predicted)':'Режим: Виртуальное устройство'));
    for(const m of r.matching_evidence||[])if(m.distance){const entity=(m.category==='location'?(activeConfig||stored).rooms:(activeConfig||stored).devices).find(e=>e.id===m.entity_id);box.append(node('p',`«${m.span}» сопоставлено с ${m.category==='location'?'зоной':'устройством'} «${entity?.name||m.alias}»`));}$('test-summary').append(box);$('test-result').textContent=json(r);await status();}
    catch(error){$('executor-contract').textContent='Исполняемое действие не сформировано.';$('test-summary').textContent='Не удалось обработать команду.';$('test-result').textContent=json(error.response||{});throw error;}});};
function wizardField(label,key,opts={}){return field($('wizard-content'),label,wizard.entity[key],v=>wizard.entity[key]=v,{local:true,...opts});}
function wizardMeta(label,key,opts={}){return field($('wizard-content'),label,wizard.entity.metadata[key],v=>wizard.entity.metadata[key]=v,{local:true,...opts});}
function renderWizard(){
    if(wizard.kind==='action'){renderActionWizard();return;}
    const w=wizard,e=w.entity,c=$('wizard-content'),steps=w.kind==='location'?['Название','Иерархия и тип','Речевые обозначения','Проверка предложения']:['Название','Зона','Тип и шаблон','Обозначения','Что умеет устройство','Допустимые значения','Проверка предложения'];
    c.replaceChildren();$('wizard-errors').replaceChildren();$('wizard-title').textContent=w.kind==='location'?'Новая зона':'Новое устройство';$('wizard-step').textContent=`${w.step+1} / ${steps.length} · ${steps[w.step]}`;$('wizard-back').hidden=w.step===0;$('wizard-next').hidden=w.step===steps.length-1;$('wizard-confirm').hidden=w.step!==steps.length-1;
    if(w.step===0){wizardField('Название','name',{id:'new-name'});field(c,'Постоянный идентификатор',e.id,v=>{e.id=v;w.manualId=true;},{id:'new-id',local:true,advanced:true,readonly:!!w.existing});if(advanced())c.append(node('small','Идентификатор создаётся из названия. После первого сохранения он не меняется при переименовании.'));
        button(c,'Предложить с локальным ИИ',()=>run(async()=>{if(!e.id)await assignId();if((w.raw||w.edited||e.metadata.description||e.metadata.aliases.length)&&!confirm('Заменить текущие предложения и исправленные поля новым AI-черновиком?'))return;const r=await api('/api/admin/draft','POST',{entity:w.kind,name:e.name});w.raw=r;w.entity=M.suggestion(e,r.suggestion,w.kind);w.provenance='ai_suggested';w.capabilities=r.suggestion.capabilities;if(w.kind==='device')await selectTemplate(r.suggestion.template);renderWizard();}));
        if(w.provenance==='ai_suggested'){c.append(node('span','AI suggestion · проверьте и отредактируйте','badge'));c.append(node('p',`Тип: ${e.metadata.type}. Обозначения: ${e.metadata.aliases.join(', ')}${w.capabilities?.length?`. Возможные операции: ${w.capabilities.join(', ')} (предложение, не разрешение на выполнение)`:''}`));c.append(node('p',e.metadata.description||''));if(w.raw?.retry_count)c.append(node('small','Корректное предложение получено после повторной попытки.'));c.append(node('pre',json(w.raw),'advanced'));}
    }else if(w.kind==='location'){
        if(w.step===1){wizardMeta('Тип зоны','type');field(c,'Родительская зона',e.metadata.parent_id||'',v=>e.metadata.parent_id=v||null,{choices:locationChoices(),local:true,id:'new-parent'});wizardMeta('Группа зон без собственных команд','container',{checkbox:true,change:()=>{if(e.metadata.container)e.speech={mentions:[],locations:[]};},id:'new-container'});wizardMeta('Описание','description');}
        if(w.step===2){field(c,'Другие обозначения зоны',e.metadata.aliases.join('\n'),v=>e.metadata.aliases=M.list(v),{multiline:true,local:true,id:'new-zone-aliases'});if(e.metadata.container)c.append(node('p','Эта зона объединяет дочерние места; речевые обозначения для неё не требуются.'));else {field(c,'Слова зоны',e.speech.mentions.join('\n'),v=>e.speech.mentions=M.list(v),{multiline:true,local:true,id:'new-mentions'});field(c,'Фразы места',e.speech.locations.join('\n'),v=>e.speech.locations=M.list(v),{multiline:true,local:true,id:'new-locations'});c.append(node('small','Например: слово «упаковки», фраза «в цехе упаковки». Устройства в этой зоне могут иметь любые настроенные виртуальные операции.'));}}
    }else{
        if(w.step===1)field(c,'Зона устройства',e.room_id||'',v=>e.room_id=v||null,{choices:locationChoices(),local:true,id:'new-location'});
        if(w.step===2){field(c,'Шаблон',w.template||'custom',v=>run(async()=>{if((w.pendingActions?.length||w.existing)&&!confirm('Заменить редактируемые действия и схему состояния другим шаблоном?'))return;await selectTemplate(v);renderWizard();}),{local:true,choices:templates.map(t=>({id:t.id,name:t.name})),id:'new-template'});wizardMeta('Тип оборудования','type');c.append(node('p','Шаблон создаёт редактируемые действия и виртуальное состояние. Аппаратного управления нет.'));}

        if(w.step===3){field(c,'Обозначения устройства',e.metadata.aliases.join('\n'),v=>e.metadata.aliases=M.list(v),{multiline:true,local:true,id:'new-aliases'});field(c,'Словоформы устройства',e.spoken_aliases.join('\n'),v=>e.spoken_aliases=M.list(v),{multiline:true,local:true,id:'new-device-mentions'});}
        if(w.step===4){if(e.kind==='light')field(c,'Действие',e.action_ids[0],v=>e.action_ids=[v],{choices:draft.actions.filter(a=>a.operation==='light.set_brightness.v1').map(a=>({id:a.id,name:a.name})),local:true,id:'new-action'});
            else if(e.kind==='generic'){
                for(const a of w.pendingActions||[]){const box=card(c,a.name);field(box,'Поддерживать действие',e.action_ids.includes(a.id),v=>{e.action_ids=e.action_ids.filter(id=>id!==a.id);if(v)e.action_ids.push(a.id);},{checkbox:true,local:true});if(advanced())typed.action(box,a,e,renderWizard);else simple.capability(box,a,e);}
                if(advanced())for(const a of draft.actions.filter(a=>a.operation==='mock.assign.v1'))field(c,`Использовать: ${a.name}`,e.action_ids.includes(a.id),v=>{e.action_ids=e.action_ids.filter(id=>id!==a.id);if(v)e.action_ids.push(a.id);},{checkbox:true,local:true});
                if(!w.pendingActions?.length)c.append(node('p','Собственное действие можно добавить из карточки устройства после подтверждения. Мастер спросит, что должно измениться.'));
            } else c.append(node('p','Защищённая входная дверь уже зарегистрирована.'));
        }
        if(w.step===5){const a=draft.actions.find(a=>a.id===e.action_ids[0]);if(e.kind==='light'&&a?.parameters.length){if(!w.range||w.rangeAction!==a.id){w.range={minimum:a.parameters[0].minimum,maximum:a.parameters[0].maximum};w.rangeAction=a.id;}field(c,'Минимум, %',w.range.minimum,v=>w.range.minimum=v,{number:true,local:true,id:'new-minimum'});field(c,'Максимум, %',w.range.maximum,v=>w.range.maximum=v,{number:true,local:true,id:'new-maximum'});c.append(node('small','Если изменить диапазон, будет создано отдельное действие для этого устройства.'));}else if(e.kind==='generic'){if(advanced())typed.parameters(c,e.state,renderWizard,true);else simple.deviceLimits(c,e,w.pendingActions||[]);}else c.append(node('p','Дополнительных исполняемых параметров нет.'));wizardMeta('Описание','description');}
    }
    if(w.step===steps.length-1){c.append(node('span',w.provenance==='ai_suggested'?'AI suggestion · ожидает подтверждения':'Ручной черновик','badge'),node('h3',e.name),node('p',`Тип: ${e.metadata.type||e.kind||'Зона'}`),node('p',`Обозначения: ${e.metadata.aliases.join(', ')||'—'}`));if(w.kind==='location')c.append(node('p',`Слова: ${e.speech.mentions.join(', ')}. Фразы: ${e.speech.locations.join(', ')}`));else c.append(node('p',`Зона: ${draft.rooms.find(r=>r.id===e.room_id)?.name||'—'}. Действия: ${e.action_ids.map(id=>[...draft.actions,...(w.pendingActions||[])].find(a=>a.id===id)?.name).join(', ')||'нет'}`));if(w.kind==='device')simple.execution(c,advanced());if(w.range)c.append(node('p',`Диапазон: ${w.range.minimum}..${w.range.maximum}%`));c.append(node('pre',json(e),'advanced'));}
}
async function assignId(){const r=await api('/api/admin/id','POST',{category:wizard.kind,name:wizard.entity.name,occupied:M.occupied(draft)});wizard.entity.id=r.id;}
function openExistingDevice(device){wizard={kind:'device',entity:M.clone(device),pendingActions:[],template:'custom',existing:device.id,step:0,provenance:'manual'};renderWizard();$('wizard').showModal();}
function openWizard(kind){wizard={kind,entity:kind==='location'?M.location('',''):M.device('','',draft),pendingActions:[],template:'custom',step:0,provenance:'manual'};renderWizard();$('wizard').showModal();}
$('add-location').onclick=()=>openWizard('location');$('add-device').onclick=()=>openWizard('device');
$('wizard-close').onclick=()=>{if(!wizard.entity.name||confirm('Закрыть предложение без добавления в черновик?'))$('wizard').close();};
$('wizard-back').onclick=()=>{wizard.step--;renderWizard();};
$('wizard-next').onclick=()=>run(async()=>{if(!validFields($('wizard')))return;if(wizard.step===0){if(!wizard.entity.name.trim()){$('wizard-errors').replaceChildren(node('li','Введите название.'));return;}if(!wizard.entity.id)await assignId();}if(wizard.kind==='action'&&wizard.simple&&!wizard.aiComplex&&wizard.step===2){const target=draft.devices.find(d=>d.id===wizard.target);try{const compiled=M.simpleAction(wizard.entity,target,wizard.answer);wizard.entity=compiled.action;wizard.addedState=compiled.added;}catch(e){$('wizard-errors').replaceChildren(node('li',e.message));return;}}wizard.step++;renderWizard();});
$('wizard-form').onsubmit=e=>{e.preventDefault();run(async()=>{
    const problems=[...M.wizardErrors(wizard.entity,wizard.kind,wizard.existing?{...draft,devices:draft.devices.filter(d=>d.id!==wizard.existing)}:draft),...M.constraintErrors(wizard.range)];
    if(problems.length){$('wizard-errors').replaceChildren(...problems.map(x=>node('li',x)));return;}
    if(wizard.kind==='device' && wizard.entity.kind==='light'){
        const proposed=M.constrainedAction(wizard.entity,wizard.range,draft,'pending');
        if(proposed){const r=await api('/api/admin/id','POST',{category:'action',name:`${wizard.entity.name}: яркость`,occupied:M.occupied(draft)});proposed.id=r.id;draft.actions.push(proposed);wizard.entity.action_ids=[r.id];}
    }
    if(wizard.kind==='action'){
        wizard.entity.origin=wizard.provenance==='ai_suggested'?'local_ai':'manual';
        const target=draft.devices.find(d=>d.id===wizard.target);if(!target){$('wizard-errors').replaceChildren(node('li','Выберите устройство.'));return;}
        const candidate=M.clone(draft);candidate.actions.push(M.clone(wizard.entity));const candidateTarget=candidate.devices.find(d=>d.id===target.id);candidateTarget.action_ids.push(wizard.entity.id);if(wizard.addedState)candidateTarget.state.push(M.clone(wizard.addedState));
        await api('/api/config/validate','POST',{configuration:candidate});draft=candidate;
    }else {
        wizard.entity.metadata.origin=wizard.provenance==='ai_suggested'?'local_ai':'manual';wizard.entity.metadata.confirmed=true;
        if(wizard.kind==='device')draft.actions.push(...(wizard.pendingActions||[]).filter(a=>wizard.entity.action_ids.includes(a.id)).map(M.clone));
        if(wizard.existing)draft.devices[draft.devices.findIndex(d=>d.id===wizard.existing)]=M.clone(wizard.entity);else draft[wizard.kind==='location'?'rooms':'devices'].push(M.clone(wizard.entity));
    }modified();render();$('wizard').close();$('validation').textContent='Вы подтвердили предложение. Оно добавлено в черновик; проверьте его перед сохранением.';
});};
async function selectTemplate(id){if(!draft.compatibility&&id==='light')id='generic_light';if(!draft.compatibility&&id==='door')throw new Error('Защищённая входная дверь требует существующей системной конфигурации.');const w=wizard,e=w.entity,t=templates.find(t=>t.id===id);if(!t)return;w.template=id;w.range=null;w.pendingActions=[];e.state=[];
    e.kind=id==='light'?'light':id==='door'?'entrance_door':'generic';e.action_ids=id==='light'?[draft.compatibility?.light_action_id]:id==='door'?[draft.compatibility?.door_action_id]:[];
    if(!e.metadata.type||e.metadata.type==='generic')e.metadata.type=t.type;
    if(e.kind!=='generic')return;e.state=M.clone(t.state);e.spoken_aliases=[...new Set([...e.spoken_aliases,...t.aliases])];
    for(const source of t.actions){const a=M.clone(source);const r=await api('/api/admin/id','POST',{category:'action',name:`${e.name} ${a.name}`,occupied:[...M.occupied(draft),...w.pendingActions.map(a=>a.id)]});a.id=r.id;w.pendingActions.push(a);e.action_ids.push(a.id);}
}
function openActionWizard(target){wizard={kind:'action',simple:!advanced(),answer:{fieldId:'',needsValue:true,required:true,property:{name:'',type:'integer',required:true,description:'',unit:'',minimum:0,maximum:100},fixed:0},target:target||draft.devices.find(d=>d.kind==='generic')?.id||'',entity:{id:'',name:'',operation:'mock.assign.v1',enabled:true,description:'',parameters:[],speech_aliases:[],assignments:[]},step:0,provenance:'manual'};renderWizard();$('wizard').showModal();}
function renderActionWizard(){if(wizard.simple){renderSimpleActionWizard();return;}const w=wizard,a=w.entity,c=$('wizard-content');c.replaceChildren();$('wizard-errors').replaceChildren();const steps=['Название','Устройство','Аргументы и изменения','Проверка'];$('wizard-title').textContent='Новое виртуальное действие';$('wizard-step').textContent=`${w.step+1} / 4 · ${steps[w.step]}`;$('wizard-back').hidden=w.step===0;$('wizard-next').hidden=w.step===3;$('wizard-confirm').hidden=w.step!==3;
    if(w.step===0){wizardField('Название','name',{id:'new-name'});wizardField('Постоянный идентификатор','id',{id:'new-id',advanced:true});c.append(node('p','Название не определяет поведение. На следующих шагах задайте аргументы и присваивания.'));}
    if(w.step===1)field(c,'Устройство',w.target,v=>w.target=v,{choices:draft.devices.filter(d=>d.kind==='generic').map(d=>({id:d.id,name:d.name})),local:true,id:'action-target'});
    if(w.step===2){button(c,'Предложить аргументы и присваивания с ИИ',()=>run(async()=>{if((a.assignments.length||a.parameters.length||a.description)&&!confirm('Заменить уже исправленные поля новым AI-предложением?'))return;const r=await api('/api/admin/action-draft','POST',{name:a.name,target:w.target,configuration:draft});w.entity=r.suggestion;w.provenance='ai_suggested';w.raw=r;renderWizard();}));if(w.raw)c.append(node('span','AI suggestion · проверьте поведение','badge'),node('pre',json(w.raw),'advanced'));typed.action(c,a,draft.devices.find(d=>d.id===w.target),renderWizard);}
    if(w.step===3){c.append(node('h3',a.name),node('p',a.description),node('p',`Устройство: ${draft.devices.find(d=>d.id===w.target)?.name||'не выбрано'}`),node('p',`Обозначения: ${a.speech_aliases.join(', ')}`));for(const op of a.assignments)c.append(node('p',`${op.field} ← ${op.argument?'аргумент '+op.argument:JSON.stringify(op.value)}`));c.append(node('p','Подтверждение добавит только черновик. Выполнение — исключительно MOCK.'),node('pre',json(a),'advanced'));}
}
$('add-action').onclick=()=>openActionWizard();
function renderSimpleActionWizard(){
    const w=wizard,a=w.entity,c=$('wizard-content'),target=draft.devices.find(d=>d.id===w.target);
    c.replaceChildren();$('wizard-errors').replaceChildren();
    const steps=['Название','Устройство','Значение и результат','Проверка предложения'];
    $('wizard-title').textContent='Новое действие';$('wizard-step').textContent=`${w.step+1} / 4 · ${steps[w.step]}`;
    $('wizard-back').hidden=w.step===0;$('wizard-next').hidden=w.step===3;$('wizard-confirm').hidden=w.step!==3;
    if(w.step===0){wizardField('Название','name',{id:'new-name'});c.append(node('p','Например: «Включить» или «Установить дозу». Дальше выберите устройство и опишите результат.'));}
    if(w.step===1)field(c,'Устройство',w.target,v=>{w.target=v;w.answer.fieldId='';},{choices:draft.devices.filter(d=>d.kind==='generic').map(d=>({id:d.id,name:d.name})),local:true,id:'action-target'});
    if(w.step===2){
        if(target?.state.length)button(c,'Предложить результат с ИИ',()=>run(async()=>{
            if((a.assignments.length||a.description||a.speech_aliases.length)&&!confirm('Заменить текущие предложения и исправления новым AI-черновиком?'))return;
            const r=await api('/api/admin/action-draft','POST',{name:a.name,target:w.target,configuration:draft});
            w.entity=r.suggestion;w.raw=r;w.provenance='ai_suggested';w.aiComplex=w.entity.assignments.length!==1;
            if(!w.aiComplex){const op=w.entity.assignments[0];w.answer.fieldId=op.field;w.answer.needsValue=op.argument!==undefined;w.answer.fixed=op.value;w.answer.required=w.entity.parameters.find(p=>p.id===op.argument)?.required??true;}
            renderWizard();
        }));
        if(w.raw)c.append(node('span','AI suggestion · проверьте результат','badge'),node('pre',json(w.raw),'advanced'));
        if(w.aiComplex){simple.capability(c,a,target);c.append(node('p','Предложено несколько изменений. Их точное редактирование доступно в Advanced.'));}
        else simple.action(c,w,target,renderWizard);
    }
    if(w.step===3){const projected={...target,state:[...(target?.state||[]),...(w.addedState?[w.addedState]:[])]};
        c.append(node('h3',a.name),node('p',`Устройство: ${target?.name||'не выбрано'}`),node('p',`Как попросить: ${a.speech_aliases.join(', ')||'добавьте фразу на предыдущем шаге'}`));
        simple.capability(c,a,projected);simple.execution(c);
        if(w.addedState)c.append(node('p',`Начальное значение «${w.addedState.name}»: ${M.valueText(w.addedState.initial,w.addedState)}`));
        c.append(node('p','Подтверждение добавит предложение только в черновик.'));
    }
}
function openConstraints(selected){
    const c=$('constraints-content');c.replaceChildren();
    const choices=[...draft.devices.filter(d=>d.kind==='generic').map(d=>({id:'device:'+d.id,name:'Устройство: '+d.name})),...draft.actions.filter(a=>a.parameters.length).map(a=>({id:'action:'+a.id,name:'Действие: '+a.name}))];
    if(!choices.length){c.append(node('p','Сначала добавьте устройство и действие со значением.'));}
    else {selected=selected||choices[0].id;field(c,'Для чего задать допустимые значения?',selected,v=>openConstraints(v),{choices,local:true});
        const [kind,id]=selected.split(':');
        if(kind==='device'){const d=draft.devices.find(d=>d.id===id);simple.deviceLimits(c,d,[]);}
        else {const a=draft.actions.find(a=>a.id===id);c.append(node('p','Это действие используют: '+draft.devices.filter(d=>d.action_ids.includes(id)).map(d=>d.name).join(', ')));
            for(const p of a.parameters){const box=card(c,p.name);simple.limits(box,p,()=>{},true);}}
    }
    if(!$('constraints-dialog').open)$('constraints-dialog').showModal();
}
$('add-constraint').onclick=()=>openConstraints();
$('constraints-close').onclick=()=>{if(validFields($('constraints-dialog'))){$('constraints-dialog').close();render();}};
$('describe-plan').onclick=()=>run(async()=>{const text=prompt('Опишите одну новую зону и до четырёх устройств. Например: добавь зону упаковки, освещение и два вентилятора.');if(!text)return;const r=await api('/api/admin/plan','POST',{text,configuration:draft});pendingPlan=r.draft.configuration;$('plan-content').replaceChildren();
    for(const key of ['rooms','devices'])for(const e of pendingPlan[key].filter(e=>!draft[key].some(old=>old.id===e.id))){const box=card($('plan-content'),`${key==='rooms'?'Зона':'Устройство'}: ${e.name}`);field(box,'Название',e.name,v=>e.name=v,{local:true});field(box,'Описание',e.metadata.description,v=>e.metadata.description=v,{local:true});if(key==='rooms'){field(box,'Обозначения зоны',e.speech.mentions.join('\n'),v=>e.speech.mentions=M.list(v),{multiline:true,local:true});field(box,'Фразы места',e.speech.locations.join('\n'),v=>e.speech.locations=M.list(v),{multiline:true,local:true});}else{field(box,'Обозначения устройства',e.spoken_aliases.join('\n'),v=>e.spoken_aliases=M.list(v),{multiline:true,local:true});box.append(node('h4','Что умеет устройство'));for(const id of e.action_ids){const a=pendingPlan.actions.find(a=>a.id===id);if(a){box.append(node('strong',a.name));simple.capability(box,a,e);}}simple.execution(box,advanced());}}
    $('plan-trace').textContent=json(r);$('plan-dialog').showModal();});
$('plan-confirm').onclick=()=>run(async()=>{if(!validFields($('plan-dialog')))return;for(const key of ['rooms','devices'])for(const e of pendingPlan[key])if(!draft[key].some(old=>old.id===e.id))ensureMeta(e).confirmed=true;const r=await api('/api/config/import','POST',{document:JSON.stringify(pendingPlan)});draft=r.configuration;pendingPlan=null;modified();render();$('plan-dialog').close();$('validation').textContent='User confirmed · связанные сущности добавлены только в черновик.';});
$('plan-close').onclick=()=>{pendingPlan=null;$('plan-dialog').close();};
function clearCopilot(){
    pendingCopilot=null;$('copilot-summary').textContent='';$('copilot-diff').replaceChildren();
    $('copilot-raw').textContent='';$('copilot-accept').hidden=true;$('copilot-cancel').hidden=true;
}
function setVoiceUi(){
    const mic=$('copilot-mic'), statusEl=$('copilot-voice-status');
    if(!mic||!statusEl)return;
    mic.dataset.state=voiceState;
    if(voiceState==='idle'){
        mic.textContent='Микрофон';
        mic.title='Голосовой ввод';
        statusEl.textContent=M.idleVoiceStatus(sttReady, voiceIdleMessage);
        mic.disabled=!sttReady;
    } else if(voiceState==='requesting'){
        mic.textContent='…';
        mic.title='Подготовка микрофона';
        statusEl.textContent='Запрос доступа к микрофону…';
        mic.disabled=true;
    } else if(voiceState==='recording'){
        mic.textContent='Стоп';
        mic.title='Остановить';
        statusEl.textContent='Запись… Нажмите ещё раз, чтобы остановить.';
        mic.disabled=false;
    } else {
        mic.textContent='…';
        mic.title='Распознавание';
        statusEl.textContent='Распознавание…';
        mic.disabled=true;
    }
    if(voiceClosing)mic.disabled=true;
}
async function releaseVoiceCapture(rec){
    if(!rec)return;
    try{rec.processor&&rec.processor.disconnect();}catch(_){}
    try{rec.source&&rec.source.disconnect();}catch(_){}
    try{rec.stream&&rec.stream.getTracks().forEach(t=>t.stop());}catch(_){}
    try{if(rec.ctx)await rec.ctx.close();}catch(_){}
}
async function startCopilotVoice(){
    if(voiceState!=='idle'||voiceClosing)return;
    const attempt=++voiceAttempt;
    const gate=M.voiceGate();
    if(!gate.ok){voiceIdleMessage=M.humanError({code:gate.code});setVoiceUi();return;}
    if(!sttReady){voiceIdleMessage=M.humanError({code:'stt_unavailable'});setVoiceUi();return;}
    voiceIdleMessage='';
    voiceState=M.nextVoiceState('idle','start');
    setVoiceUi();
    let ctx=null, stream=null, source=null, processor=null;
    try{
        const AudioCtx=window.AudioContext||window.webkitAudioContext;
        ctx=new AudioCtx();
        stream=await navigator.mediaDevices.getUserMedia({audio:{channelCount:1,echoCancellation:true,noiseSuppression:true}});
        if(attempt!==voiceAttempt){await releaseVoiceCapture({ctx,stream});return;}
        source=ctx.createMediaStreamSource(stream);
        processor=ctx.createScriptProcessor(4096,1,1);
        const chunks=[], sampleRate=ctx.sampleRate, maxSamples=sampleRate*(setupActive?60:M.MaxCopilotVoiceSeconds);
        let samples=0;
        processor.onaudioprocess=e=>{
            if(voiceState!=='recording')return;
            const input=e.inputBuffer.getChannelData(0);
            const kept=input.subarray(0,Math.max(0,maxSamples-samples));
            chunks.push(new Float32Array(kept));
            samples+=kept.length;
            $('copilot-voice-status').textContent=`Запись: ${Math.floor(samples/sampleRate)} / ${setupActive?60:M.MaxCopilotVoiceSeconds} с`;
            if(samples>=maxSamples) stopCopilotVoice(true);
        };
        source.connect(processor);
        processor.connect(ctx.destination);
        voiceRec={ctx,stream,source,processor,chunks,sampleRate};
        voiceState=M.nextVoiceState('requesting','ready');
        setVoiceUi();
    }catch(error){
        await releaseVoiceCapture({ctx,stream,source,processor});
        if(attempt!==voiceAttempt)return;
        voiceRec=null;
        voiceIdleMessage=M.humanError({code:M.mapMicError(error)});
        voiceState=M.nextVoiceState('requesting','fail');
        setVoiceUi();
    }
}
async function stopCopilotVoice(tooLong){
    if(voiceState!=='recording'||!voiceRec)return;
    const attempt=voiceAttempt, rec=voiceRec; voiceRec=null;
    await releaseVoiceCapture(rec);
    if(attempt!==voiceAttempt)return;
    if(tooLong)voiceIdleMessage='Достигнут лимит записи. Распознаём записанное; можно продолжить следующим сообщением.';
    const total=rec.chunks.reduce((n,c)=>n+c.length,0);
    const merged=new Float32Array(total);
    let offset=0; for(const chunk of rec.chunks){merged.set(chunk,offset);offset+=chunk.length;}
    const wav=M.encodeWavPcm16(merged, rec.sampleRate);
    voiceState=M.nextVoiceState('recording','stop');
    setVoiceUi();
    try{
        const response=await fetch('/api/admin/transcribe',{method:'POST',headers:{'Content-Type':'audio/wav','X-Firefly-Admin':'1','X-Firefly-Setup':setupToken},body:wav});
        const result=await response.json();
        if(attempt!==voiceAttempt)return;
        if(!response.ok||result.ok===false){
            const err=new Error('stt'); err.response=result; throw err;
        }
        $('copilot-text').value=result.text||'';updateSetupCounter();
        voiceIdleMessage=(tooLong?'Достигнут лимит 60 секунд. Записанная часть сохранена в тексте. ':'')+(result.segmented?'Длинная запись распознана по частям; проверьте слова на стыках. ':'')+'Текст распознан. Исправьте при необходимости и отправьте просьбу.';
        voiceState=M.nextVoiceState('transcribing','done');
    }catch(e){
        if(attempt!==voiceAttempt)return;
        const code=e.response?.errors?.[0]?.code||'stt_failed';
        voiceIdleMessage=M.humanError({code});
        if(e.response?.errors) errors(e.response.errors);
        voiceState=M.nextVoiceState('transcribing','fail');
    }
    setVoiceUi();
}
$('copilot-mic').onclick=()=>{
    if(operation||voiceState==='transcribing'||voiceState==='requesting')return;
    if(voiceState==='recording') stopCopilotVoice(false);
    else startCopilotVoice();
};
setVoiceUi();
function showCopilot(proposal, meta){
    pendingCopilot=proposal;
    $('copilot-summary').textContent=proposal.summary||'';
    $('copilot-diff').replaceChildren(...M.diffLines(proposal.changes||[], proposal.configuration||draft).map(x=>node('li',x)));
    if(!(proposal.changes||[]).length)$('copilot-diff').append(node('li','Изменений нет.'));
    $('copilot-raw').textContent=json({proposal,meta});
    $('copilot-accept').hidden=false;$('copilot-cancel').hidden=false;
}
$('copilot-propose').onclick=()=>run(async()=>{
    const text=$('copilot-text').value.trim();
    if(!text){$('copilot-status').textContent='Введите запрос.';return;}
    clearCopilot();
    $('copilot-status').textContent='Готовим предложение…';
    try{
        const r=await api('/api/admin/copilot','POST',{text,configuration:draft});
        showCopilot(r.draft,{retry_count:r.retry_count,trace:r.trace});
        $('copilot-status').textContent='Проверьте предложение. «Применить к черновику» меняет только локальный черновик.';
    }catch(e){
        clearCopilot();
        const err=e.response?.errors?.[0]||{code:'invalid_ai_response'};
        $('copilot-status').textContent=M.humanError(err,draft);
        throw e;
    }
});
$('copilot-cancel').onclick=()=>{clearCopilot();$('copilot-status').textContent='Предложение отменено.';};
$('copilot-accept').onclick=()=>run(async()=>{
    if(!pendingCopilot)return;
    const r=await api('/api/admin/copilot/accept','POST',{configuration:draft,proposal:pendingCopilot});
    draft=r.configuration;clearCopilot();modified();render();
    $('copilot-status').textContent='Предложение принято в черновик. Сохранение и применение — отдельными кнопками.';
    $('validation').textContent='User confirmed · Copilot изменил только черновик.';
});
async function revisions(){const r=await api('/api/config/revisions');const entries=[...r.revisions.slice(-9),{revision:expected.revision,hash:expected.hash,current:true}].reverse();$('revisions').replaceChildren();
    for(const entry of entries){const version=await api('/api/config/revision','POST',{revision:entry.revision});const change=await api('/api/config/diff','POST',{configuration:version.configuration,base:'stored'});const row=node('div',undefined,'revision-row');row.append(node('strong',`Версия ${entry.revision}${entry.current?' · сохранённая':''}`),node('span',`${entry.timestamp_ms?'Архивирована '+new Date(entry.timestamp_ms).toLocaleString():'Текущая версия'} · ${change.changes.length} изменений к сохранённой · ${entry.revision===activeRevision?'активна':entry.current?'ожидает применения':'архив'}`));const buttons=node('div',undefined,'buttons');row.append(buttons);
        button(buttons,'Изменения',()=>run(async()=>{const rev=await api('/api/config/revision','POST',{revision:entry.revision});const r=await api('/api/config/diff','POST',{configuration:rev.configuration,base:'stored'});showDiff(r.changes,`Версия ${entry.revision} относительно сохранённой`);}));
        button(buttons,'В черновик',()=>run(async()=>{if(dirty&&!confirm('Заменить несохранённый черновик выбранной версией?'))return;const r=await api('/api/config/restore-candidate','POST',{revision:entry.revision,expected});draft=r.configuration;await migrateCandidate();modified();render();showDiff((await diff('stored')).changes,'Восстановлено только в черновик');}));$('revisions').append(row);}}
$('backups').onclick=()=>run(revisions);
$('export').onclick=()=>run(async()=>{const r=await api('/api/config/export');const url=URL.createObjectURL(new Blob([json(r)+'\n'],{type:'application/json'}));const a=node('a');a.href=url;a.download=`firefly-config-v${r.schema_version}-revision-${r.revision}.json`;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);});
$('import').onchange=e=>{const file=e.target.files[0];if(!file)return;run(async()=>{if(file.size>256*1024)throw Object.assign(new Error('file too large'),{response:{errors:[{code:'size_limit',human:'Файл превышает 256 КБ.'}]}});if(dirty&&!confirm('Заменить текущий черновик импортируемым файлом?'))return;let document;try{document=M.decodeImport(await file.arrayBuffer());}catch(_){throw Object.assign(new Error('UTF-8'),{response:{errors:[{code:'invalid_text'}]}});}const r=await api('/api/config/import','POST',{document});draft=r.configuration;await migrateCandidate();modified();render();showDiff((await diff('stored')).changes,'Импортировано в черновик — изменения ещё не сохранены');});e.target.value='';};
$('show-active').onclick=()=>run(async()=>{const r=await api('/api/config/active');$('active-context').textContent=r.active?.context||'Нет активного контекста';});
$('reconcile').onclick=()=>run(async()=>{if(confirm('Проверить и завершить прерванную транзакцию сохранения?')){await api('/api/config/reconcile','POST',{});await load();}});
$('mode').onchange=()=>{document.body.dataset.mode=$('mode').value;localStorage.setItem('firefly-admin-mode',$('mode').value);if(draft)render();};
$('mode').value=localStorage.getItem('firefly-admin-mode')||'simple';$('mode').onchange();
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});
run(async()=>{templates=(await api('/api/admin/templates')).templates;await load();await status();await revisions();await restoreSetupView();});setInterval(()=>{if(!operation)status();},4000);

function updateSetupCounter(){
    const count=M.adminCharacterCount($('copilot-text').value);
    $('setup-counter').textContent=`${count} / 4000`;
    $('copilot-text').setCustomValidity(count>4000?'Максимум 4000 символов. Текст не обрезан.':'');
}
$('copilot-text').addEventListener('input',updateSetupCounter);
function showSetup(result){
    setupActive=!!result.active;setupVersion=result.version||0;
    if(result.session_id){setupToken=result.session_id;sessionStorage.setItem('fireflySetup',setupToken);}
    if(result.configuration){draft=result.configuration;setupSnapshot=json(draft);modified();render();}
    const labels={editing:'Можно продолжать диалог.',planning:'Готовлю изменения…',clarification_required:'Нужно уточнение.',entering:'Подготавливаю режим настройки…',restoring:'Возвращаю ассистента…',restoring_for_admission:'Проверяю и применяю настройки…',cancel_pending:'Дожидаюсь завершения текущего шага для отмены.',applied:'Настройки применены. Ассистент включён.',cancelled_draft_saved:'Ассистент включён. Неприменённый черновик сохранён.',restart_required:'Не удалось безопасно восстановить модель. Требуется перезапуск.',planning_failed:'План не завершён. Рабочая конфигурация не изменена.',resume_required:'Есть незавершённый черновик. Его можно продолжить.'};
    $('setup-state').textContent=(setupActive?'Бытовой ассистент приостановлен. ':'')+(labels[result.status]||result.status);
    $('setup-preview').hidden=$('setup-apply').hidden=$('setup-cancel').hidden=!setupActive;
    $('setup-start').hidden=setupActive;$('setup-resume').hidden=setupActive||result.status==='idle'||result.status==='applied';
    const plan=$('setup-plan');plan.replaceChildren();
    if(result.configuration){
        for(const room of draft.rooms){const box=card(plan,room.name);
            for(const device of draft.devices.filter(d=>d.room_id===room.id))box.append(node('p',device.name+' — '+device.action_ids.map(id=>draft.actions.find(a=>a.id===id)?.name||id).join(', ')));
        }
    }
    if(result.progress&&setupActive)plan.append(node('p',`Шаг ${result.progress.step} / ${result.progress.total_steps}`));
    for(const question of result.questions||[])plan.append(node('p','Требует уточнения: '+question));
    if(result.language_warning)plan.append(node('p',result.language_warning));
    plan.append(node('p',result.connection_note||''));
    if(!setupActive){setupToken='';sessionStorage.removeItem('fireflySetup');voiceClosing=false;setVoiceUi();}
}
async function syncSetupDraft(){
    if(setupActive&&json(draft)!==setupSnapshot){
        const reply=await api('/api/setup/draft','POST',{session_id:setupToken,version:setupVersion,configuration:draft});
        showSetup(reply);pendingCopilot=null;pendingPlan=null;
    }
}
for(const route of ['start','resume'])$('setup-'+route).onclick=()=>run(async()=>{
    const pending=dirty?M.clone(draft):null;
    $('setup-state').textContent='Завершаю принятые команды и подготавливаю модель настройки…';
    showSetup(await api('/api/setup/'+route,'POST',{}));
    if(route==='start'&&pending){draft=pending;await syncSetupDraft();}
});
$('setup-preview').onclick=()=>run(async()=>{await syncSetupDraft();showDiff((await diff('stored')).changes,'Изменения перед подтверждением');});
$('setup-apply').onclick=()=>run(async()=>{
    if(voiceState!=='idle')throw Object.assign(new Error('recording_active'),{response:{errors:[{code:'recording_active'}]}});
    await syncSetupDraft();
    const result=await api('/api/setup/apply','POST',{session_id:setupToken,version:setupVersion});
    await load();showSetup(result);dirty=false;
});
$('setup-cancel').onclick=async()=>{
    try {
        voiceClosing=true;++voiceAttempt;const recording=voiceRec;voiceRec=null;
        await releaseVoiceCapture(recording);voiceState='idle';
        voiceIdleMessage='Запись остановлена при отмене настройки.';setVoiceUi();
        const result=await api('/api/setup/cancel','POST',{session_id:setupToken});
        if(result.status!=='cancel_pending'){showSetup(result);return;}
        $('setup-state').textContent='Дожидаюсь завершения текущего шага для отмены…';
        const poll=async()=>{try{const state=await api('/api/setup/status');
            if(!state.active||state.status==='restart_required')showSetup(state);else setTimeout(poll,1000);
        }catch(e){$('setup-state').textContent='Не удалось получить состояние отмены. Черновик не применяется автоматически.';}};
        setTimeout(poll,1000);
    }catch(e){errors(e.response?.errors||[{code:'setup_failed',message:e.message}]);}
};
$('copilot-propose').textContent='Отправить просьбу';
$('copilot-propose').onclick=()=>run(async()=>{
    if(!setupActive)throw Object.assign(new Error('setup_required'),{response:{errors:[{code:'setup_required'}]}});
    updateSetupCounter();if(!$('copilot-text').reportValidity())return;
    await syncSetupDraft();
    const text=$('copilot-text').value;
    if(!text.trim())throw new Error('Введите просьбу.');
    const requestId=Date.now().toString(36)+'-'+Math.random().toString(36).slice(2);
    try {showSetup(await api('/api/setup/message','POST',{session_id:setupToken,version:setupVersion,request_id:requestId,text}));}
    catch(error){
        // A failed model step still advances the server draft version. Recover
        // that authoritative snapshot so a corrected message can continue.
        try{showSetup(await api('/api/setup/status'));}catch(_){}
        throw error;
    }
});
setInterval(async()=>{
    if(!setupToken||operation)return;
    try {const reply=await api('/api/setup/heartbeat','POST',{session_id:setupToken});
        if(!setupActive)showSetup(reply);
    } catch(e){$('setup-state').textContent='Связь с настройкой потеряна. Черновик не применяется автоматически.';}
},10000);

function applySetupAccess(){
    if(setupReadOnly){
        document.querySelectorAll('button,input,textarea,select').forEach(e=>{e.disabled=true;});
        $('setup-state').textContent='Настройка открыта в другом клиенте. Доступен просмотр текущего плана.';
    }
}
async function restoreSetupView(){
    const state=await api('/api/setup/status');
    if(state.active){
        if(setupToken){
            try{await api('/api/setup/heartbeat','POST',{session_id:setupToken});}
            catch(_){setupToken='';sessionStorage.removeItem('fireflySetup');}
        }
        setupReadOnly=!setupToken;showSetup(state);applySetupAccess();
    } else if(state.status==='resume_required'||state.status==='cancelled_draft_saved') {
        $('setup-state').textContent='Есть сохранённый неприменённый черновик. Нажмите «Продолжить сохранённый черновик».';
    } else $('setup-resume').hidden=true;
}

setInterval(async()=>{
    if(!setupActive||!operation)return;
    try{const state=await api('/api/setup/status');
        if(state.progress)$('setup-state').textContent=`Ассистент приостановлен. Шаг ${state.progress.step} / ${state.progress.total_steps}: ${state.progress.phase==='language_profile'?'готовлю язык команд':'готовлю план'}`;
    }catch(_){}
},2000);
