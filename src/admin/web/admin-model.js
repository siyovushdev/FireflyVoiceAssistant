/* Pure presentation/draft helpers, shared by the browser and dependency-free tests. */
(function(root) {
    'use strict';
    const decodeImport = bytes => new TextDecoder('utf-8',{fatal:true,ignoreBOM:true}).decode(bytes);
    function scalarNumber(raw) {
        if(!raw.trim()||!Number.isFinite(Number(raw)))throw Error('Введите конечное число.');
        if(/[.eE]/.test(raw)) {
            const digits=raw.split(/[eE]/)[0].replace(/[^0-9]/g,'').replace(/^0+/,'');
            if(digits.length>15)throw Error('Допустимо не более 15 значащих десятичных цифр.');
            if(Number(raw)===0&&/[1-9]/.test(raw.split(/[eE]/)[0]))throw Error('Число слишком мало для точного хранения.');
        } else if(!Number.isSafeInteger(Number(raw)))throw Error('Целое число выходит за точный диапазон браузера ±9007199254740991.');
        return Number(raw);
    }
    const clone = value => JSON.parse(JSON.stringify(value));
    const list = value => value.split('\n').map(s => s.trim()).filter(Boolean);
    const metadata = entity => entity.metadata || {};
    const typeName=value=>({light:'Освещение',entrance_door:'Входная дверь',generic:'Другое устройство',custom:'Свои возможности',tv:'Телевизор',conveyor:'Конвейер',switch:'Переключатель',numeric:'Числовой регулятор',enum:'Выбор режима',room:'Помещение',integer:'Целое число',number:'Число',boolean:'Да / нет',string:'Текст'}[value]||value);
    const occupied = config => [config.system.id, ...config.retired_ids, ...['rooms','devices','actions'].flatMap(k => config[k].map(x => x.id))];
    function location(name, id) {
        return {id, name, speech:{mentions:[],locations:[]}, metadata:{type:'custom',description:'',aliases:[],parent_id:null,container:false}};
    }
    function device(name, id, config, template='generic') {
        return {id,name,kind:template,room_id:config.rooms.find(r => !metadata(r).container)?.id || null,enabled:true,
            action_ids:template==='light'?[config.compatibility.light_action_id]:template==='entrance_door'?[config.compatibility.door_action_id]:[],
            spoken_aliases:[],state:[],metadata:{type:template,description:'',aliases:[]}};
    }
    function suggestion(draft, proposed, kind) {
        const result = clone(draft);
        result.metadata.type = proposed.type; result.metadata.aliases = [...new Set([...proposed.aliases,...(kind==='device'?[draft.name.toLocaleLowerCase('ru').trim().replace(/\s+/g,' ')]:[])])];
        if(proposed.description!==undefined)result.metadata.description=proposed.description;
        if(kind==='device')result.spoken_aliases=[...(proposed.mentions||[])];
        if (kind==='location' && !result.metadata.container) result.speech = {mentions:[...proposed.mentions],locations:[...proposed.locations]};
        // Advisory capability names never become executable action IDs.
        return result;
    }
    function wizardErrors(entity, kind, config) {
        const errors=[];
        if (!entity.name.trim()) errors.push('Введите название.');
        if (!/^[a-z][a-z0-9_]{0,63}$/.test(entity.id)) errors.push('Идентификатор: латинские буквы, цифры и _, до 64 символов.');
        if (occupied(config).includes(entity.id)) errors.push('Идентификатор уже используется.');
        if (kind==='location' && !metadata(entity).container && (!entity.speech.mentions.length || !entity.speech.locations.length)) errors.push('Добавьте слова и фразы для команд либо выберите «Группа зон».');
        if (kind==='device') {
            if (entity.kind==='light' && (!config.rooms.some(r=>r.id===entity.room_id && !metadata(r).container) || !entity.action_ids.length)) errors.push('Выберите зону для команд и доступное действие.');
            if (entity.kind==='entrance_door') errors.push('Операция входной двери уже назначена системному устройству. Измените его карточку.');

        }
        return errors;
    }
    function constraintErrors(range) {
        return range && (!Number.isInteger(range.minimum)||!Number.isInteger(range.maximum)||range.minimum<0||range.maximum>100||range.minimum>range.maximum)
            ? ['Диапазон должен содержать целые значения 0..100; минимум не больше максимума.'] : [];
    }
    function constrainedAction(device, range, config, id) {
        const original=config.actions.find(a=>a.id===device.action_ids[0]);
        if(!range||!original?.parameters.length)return null;
        if(constraintErrors(range).length)throw Error('Invalid range');
        if(original.parameters[0].minimum===range.minimum && original.parameters[0].maximum===range.maximum)return null;
        const action=clone(original);action.id=id;action.name=`${device.name}: яркость`;Object.assign(action.parameters[0],range);return action;
    }
    function humanError(error, config) {
        const code=error.code, path=error.path||'', value=path.split('/').slice(1).reduce((v,k)=>v?.[k],config);
        const messages={invalid_input:'Введите непустую команду: не более 192 байт UTF-8, без управляющих символов.',ai_unavailable:'ИИ недоступен — продолжайте ручное редактирование.',invalid_ai_response:'ИИ не смог подготовить корректное предложение. Можно повторить или заполнить вручную. Конфигурация не изменена.',
            runtime_busy:'Сейчас обрабатывается другой запрос. Повторите после его завершения.',runtime_unavailable:'Модель недоступна. Ручное редактирование и сохранение доступны.',
            revision_conflict:'Конфигурация уже изменена. Загрузите сохранённую версию перед сохранением.',hierarchy_cycle:'Зона не может быть родителем самой себя или своего предка.',
            retired_id:'Этот постоянный идентификатор ранее удалён. Создайте сущность с новым идентификатором.',identity_conflict:'Нельзя изменить категорию существующего идентификатора.',
            invalid_alias:'Проверьте речевые обозначения: строчные слова, один пробел; слово зоны должно присутствовать в её фразе.',
            reserved_alias:'Речевое обозначение содержит служебное слово команды или безопасности.',
            unsupported_runtime_policy:'Политики безопасности обязательны. Отключённые действия и дополнительные запреты пока не поддерживаются.',
            unsupported_runtime_target:'Открытие доступно только для настроенной входной двери.',
            state_conflict:'Текущее состояние устройства вне нового диапазона. Сначала задайте значение, допустимое в обоих диапазонах.',
            invalid_initial_state:'Начальное виртуальное значение не соответствует допустимым значениям. Проверьте его в карточке устройства.',
            invalid_assignment:'Результат действия несовместим с характеристиками устройства. Проверьте выбранное изменение и допустимые значения.',
            unsupported_operation:'Такая операция для выбранного устройства не поддерживается. Проверьте устройство и описание результата.',
            unknown_revision:'Эта версия не найдена.',invalid_runtime_domain:'Не удалось создать командный контекст. Проверьте однозначность устройств и размер конфигурации.',
            command_failed:'Команду не удалось обработать. Проверьте текст и готовность модели.',
            runtime_rejected:'Модель отклонила запрос или размер контекста. Активная конфигурация сохранена.',
            invalid_text:'Файл содержит некорректный UTF-8 или запрещённые управляющие символы.',malformed_json:'Файл не является корректным JSON.',unknown_field:'В конфигурации есть неизвестное поле.',missing_field:'Не заполнено обязательное поле.',
            unsupported_version:'Эта версия формата конфигурации не поддерживается.',unsafe_constraint:'Проверьте диапазон и совместимость устройства с выбранным действием.',
            duplicate_id:'Постоянный идентификатор уже используется.',invalid_name:'Название должно содержать от 1 до 128 байт UTF-8.',
            connection_error:'Нет соединения с Firefly. Проверьте доступность устройства.',
            stale_proposal:'Предложение устарело: черновик уже изменился. Запросите новое предложение.',
            ambiguous:'Запрос неоднозначен. Уточните зону или устройство. Черновик не изменён.',
            unsupported:'Запрос нельзя безопасно выполнить через помощника. Черновик не изменён.',
            context_overflow:'Конфигурация слишком велика для контекста помощника. Упростите черновик или запрос.'};
        if (code==='ambiguous_alias') {
            const alias=typeof value==='string'?value:'';
            const owners=(config?.rooms||[]).filter(r=>[...r.speech.mentions,...r.speech.locations].includes(alias)).map(r=>r.name);
            return `Обозначение «${alias||'из указанного поля'}» неоднозначно${owners.length?`: ${owners.join(', ')}`:''}.`;
        }
        if (code==='ambiguous_target') return 'Подходит несколько устройств или зон. Уточните обозначение устройства и зону.';
        if (code==='dangling_reference') return 'Выбранная зона, родитель или действие не найдены. Обновите выбор.';
        if (['schema_error','invalid_parameter','invalid_state','unsafe_constraint','argument_schema_type_mismatch'].includes(code)) {
            const parts=path.split('/').slice(1);let object=config;
            for(const key of parts){if(object?.type&&object?.name)return `${object.name}: ${constraintText(object)}. Проверьте введённое значение.`;object=Array.isArray(object)?object[key]??object.find(x=>x.id===key):object?.[key];}
        }
        return messages[code] || (error.human || 'Не удалось завершить операцию. Проверьте заполненные поля; подробности доступны в Advanced.');
    }
    const fieldNames={state:'Виртуальное состояние',assignments:'Присваивания',schema_version:'Версия формата',name:'Название',speech:'Речевые обозначения',mentions:'Слова зоны',locations:'Фразы зоны',metadata:'Описание',type:'Тип',description:'Описание',aliases:'Обозначения',parent_id:'Родитель',container:'Группа зон',parameters:'Параметры',minimum:'Минимум',maximum:'Максимум',action_ids:'Действия',room_id:'Зона',enabled:'Включено',kind:'Шаблон'};
    function diffLines(changes, config) {
        const names=new Map(['rooms','devices','actions'].flatMap(k=>(config[k]||[]).map(x=>[x.id,x.name])));
        const display=(v,key)=>v===null?'—':Array.isArray(v)?v.map(x=>display(x,key)).join(', '):typeof v==='object'?Object.entries(v).filter(([k])=>!['id','required','unit','origin','confirmed'].includes(k)).map(([k,v])=>`${fieldNames[k]||({min_length:'Минимальная длина',max_length:'Максимальная длина',initial:'Начальное значение',values:'Варианты'}[k])||k}: ${display(v,k)}`).join('; '):key==='type'?typeName(v):names.get(v)||String(v);
        return changes.flatMap(c=>{
            const title=`${{location:'Зона',device:'Устройство',capability:'Действие',state:'Состояние'}[c.entity]||'Настройки'}: ${{system:'Пространство',safety:'Безопасность',compatibility:'Системные связи',bindings:'Подключения'}[c.entity]||c.name}`;
            if(c.change!=='changed') {
                const lines=[`${c.change==='added'?'+':'−'} ${title}`];
                if(c.change==='added' && ['location','device','capability'].includes(c.entity))
                    for(const k of ['speech','metadata','room_id','action_ids','parameters'])
                        if(c.after[k] && (!Array.isArray(c.after[k]) || c.after[k].length))lines.push(`${fieldNames[k]||k}: ${display(c.after[k])}`);
                return lines;
            }
            return [title,...Object.keys(c.after).filter(k=>!['origin','confirmed'].includes(k)&&JSON.stringify(c.before[k])!==JSON.stringify(c.after[k])).map(k=>k==='assignments'?`Результат действия: ${effectLines(c.after,config.devices.find(d=>d.action_ids.includes(c.after.id))).join('; ')}`:`${fieldNames[k]||k}: ${display(c.before[k],k)} → ${display(c.after[k],k)}`)];
        });
    }
    function commandSummary(result, config) {
        if (result.result?.unsupported) return {understood:false,message:result.reason|| (result.ambiguous?'Указано несколько подходящих устройств или зон. Уточните команду.':({grounding:'Не найдено однозначное устройство по обозначениям и зоне.',safety:'Команда отклонена: намерение или устройство не подтверждены исходным текстом.',decoder:'Модель вернула некорректный ответ.',model_unsupported:'Модель не смогла однозначно понять команду.',domain:'Аргументы не соответствуют ограничениям действия.',runtime:'Не удалось выполнить распознавание.'}[result.rejection_stage]||'Команда не поддерживается.')),execution:'Не выполнялось'};
        const action=result.result||{}, target=config.devices.find(d=>d.id===action.targetId), capability=config.actions.find(a=>a.id===action.actionId);
        return {understood:true,message:'✓ Команда понятна',device:target?.name||'Неизвестное устройство',action:capability?.name||'Действие',
            value:Object.entries(action.arguments||{}).map(([id,v])=>{const p=capability?.parameters.find(p=>p.id===id);return `${p?.name||id}: ${v}${p?.unit==='percent'?'%':p?.unit?' '+p.unit:''}`;}).join(', '),safety:result.safety==='PASS'?'✓ Проверено':'Не пройдено',
            execution:result.execution?(result.execution.success?'Выполнено':result.execution.error==='invalid_state_assignment'?'Значения несовместимы с ограничениями виртуального состояния. Изменений нет.':'Виртуальное действие отклонено. Изменений нет.'):'Не выполнялось — предварительная проверка'};
    }
    const unitText=p=>p?.unit==='percent'?'%':p?.unit||'';
    const valueText=(v,p)=>typeof v==='boolean'?(p?.name==='Питание'?(v?'Включено':'Выключено'):(v?'Да':'Нет')):typeof v==='object'?JSON.stringify(v):String(v)+(unitText(p)?' '+unitText(p):'');
    function constraintText(p){
        if(['integer','number'].includes(p.type))return `${p.type==='integer'?'целое число':'число'} от ${p.minimum} до ${p.maximum}${unitText(p)?' '+unitText(p):''}`;
        if(p.type==='enum')return `одно из: ${p.values.join(', ')}`;
        if(p.type==='string')return `текст от ${p.min_length} до ${p.max_length} символов`;
        return 'Да или нет';
    }
    function effectLines(a,target){
        if(a.operation==='light.set_brightness.v1')return ['Яркость → указанное значение',...a.parameters.map(p=>`${p.name}: ${constraintText(p)}`)];
        if(a.operation==='entrance_door.open.v1')return ['Входная дверь → Открыто. Только явная разрешённая команда.'];
        return (a.assignments||[]).map(op=>{const p=target?.state?.find(p=>p.id===op.field),argument=a.parameters.find(p=>p.id===op.argument);return `${p?.name||'Характеристика'} → ${argument?`указанное значение (${constraintText(argument)})`:valueText(op.value,p)}`;});
    }
    function initialValue(p){return p.type==='boolean'?false:p.type==='enum'?p.values[0]:p.type==='string'?'':p.minimum;}
    function simpleAction(action,target,answer){
        if(!target||target.kind!=='generic')throw Error('Выберите устройство с настраиваемыми виртуальными действиями.');
        const existing=target.state.find(p=>p.id===answer.fieldId);
        const property=clone(existing||answer.property);
        if(!property.name?.trim())throw Error('Назовите то, что должно измениться, например «Доза» или «Метка».');
        let added=null;
        if(!existing){let n=1;while(target.state.some(p=>p.id===`value_${n}`))n++;property.id=`value_${n}`;property.initial=answer.initial??initialValue(property);property.required=true;added=property;}
        const result=clone(action);result.operation='mock.assign.v1';result.parameters=[];
        if(answer.needsValue){const parameter=clone(property);delete parameter.initial;parameter.required=answer.required!==false;result.parameters=[parameter];result.assignments=[{field:property.id,argument:parameter.id}];}
        else result.assignments=[{field:property.id,value:answer.fixed}];
        return {action:result,added};
    }
    const executorContract=result=>result?.actionId&&result?.targetId?clone(result):null;
    const stateText=(s,device)=>!s?'—':s.brightness!==undefined?`${s.brightness}%`:s.state==='OPEN'?'OPEN · Открыто':s.state==='CLOSED'?'CLOSED · Закрыто':s.status==='unconfigured'?'Операции не настроены':Object.entries(s).map(([k,v])=>{const p=device?.state?.find(p=>p.id===k);return `${p?.name||k}: ${valueText(v,p)}`;}).join(', ');
    const result={scalarNumber,decodeImport,clone,list,metadata,typeName,occupied,location,device,suggestion,wizardErrors,constraintErrors,constrainedAction,humanError,diffLines,commandSummary,stateText,unitText,valueText,constraintText,effectLines,initialValue,simpleAction,executorContract};
    if(typeof module!=='undefined')module.exports=result; else root.AdminModel=result;
})(typeof window!=='undefined'?window:globalThis);
