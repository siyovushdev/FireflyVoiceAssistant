/* Human-oriented forms compiling to the existing typed configuration. */
(function(root) {
    'use strict';
    function create({node,field,button,changed}) {
        const M=root.AdminModel;
        const types=[['boolean','Да / нет'],['integer','Целое число'],['number','Число с дробной частью'],['string','Текст'],['enum','Выбор из списка']].map(([id,name])=>({id,name}));
        function scalar(host,label,p,value,set,id) {
            const options={local:true,id};
            if(p.type==='boolean')options.choices=[{id:'true',name:'Да / Включено'},{id:'false',name:'Нет / Выключено'}];
            if(p.type==='enum')options.choices=p.values.map(id=>({id,name:id}));
            if(['integer','number'].includes(p.type))options.number=true;
            const input=field(host,label,value,v=>{if(p.type==='integer'&&!Number.isSafeInteger(v))throw Error(`${p.name}: требуется целое число.`);set(p.type==='boolean'?v==='true':v);changed();},options);
            if(p.type==='enum')input.onfocus=()=>{
                const selected=input.value;input.replaceChildren();
                if(!p.values.includes(selected)){const placeholder=node('option','Выберите вариант');placeholder.value='';placeholder.disabled=true;input.append(placeholder);}
                for(const value of p.values){const option=node('option',value);option.value=value;input.append(option);}
                input.value=p.values.includes(selected)?selected:'';
            };
        }
        function limits(host,p,onchange=()=>{},optional=false) {
            const description=node('p',`${p.name}: ${M.constraintText(p)}`,'value-description');host.append(description);
            const update=(key,value)=>{p[key]=value;description.textContent=`${p.name}: ${M.constraintText(p)}`;onchange();changed();};
            if(['integer','number'].includes(p.type)) {
                field(host,'От',p.minimum,v=>update('minimum',v),{number:true,local:true});
                field(host,'До',p.maximum,v=>update('maximum',v),{number:true,local:true});
                field(host,'Единица измерения',M.unitText(p),v=>update('unit',v==='%'?'percent':v),{local:true});
            }
            if(p.type==='enum')field(host,'Варианты (по одному в строке)',p.values.join('\n'),v=>update('values',M.list(v)),{multiline:true,local:true});
            if(p.type==='string') {
                field(host,'Не короче, символов',p.min_length,v=>update('min_length',v),{number:true,local:true});
                field(host,'Не длиннее, символов',p.max_length,v=>update('max_length',v),{number:true,local:true});
            }
            if(optional)field(host,'Значение обязательно в команде',p.required,v=>update('required',v),{checkbox:true,local:true});
        }
        function capability(host,a,target){
            const lines=M.effectLines(a,target),plain=text=>text.trim().replace(/[.!]+$/,'');
            if(a.description&&!lines.some(line=>plain(line)===plain(a.description)))host.append(node('p',a.description));
            for(const line of lines)host.append(node('p',line,'effect-description'));
        }
        function deviceLimits(host,device,actions) {
            host.append(node('h4','Допустимые значения'));
            for(const p of device.state||[]) {
                const box=node('div',undefined,'typed-field');host.append(box);
                limits(box,p,()=>{
                    // One reviewed template property drives its setter's argument.
                    // This edits a local draft only; Apply still reconciles live state.
                    for(const a of actions)for(const op of a.assignments||[])if(op.field===p.id&&op.argument) {
                        const arg=a.parameters.find(x=>x.id===op.argument);if(arg){const id=arg.id,required=arg.required;Object.assign(arg,M.clone(p));delete arg.initial;arg.id=id;arg.required=required;}
                    }
                });
                scalar(box,'Начальное виртуальное значение',p,p.initial,v=>{p.initial=v;});
            }
            if(!device.state?.length)host.append(node('p','Характеристики появятся при добавлении действия. Например: доза, режим или текстовая метка.'));
        }
        function execution(host,advanced=false) {
            const section=node('div',undefined,'execution-section');host.append(section);
            section.append(node('h4','Исполнение / Execution'),node('strong','Mock · Доступно'),
                node('p','Команды меняют только виртуальное состояние Firefly. Реальное оборудование не подключено.'));
            if(advanced){const planned=node('div',undefined,'planned-bindings');planned.setAttribute('aria-label','Будущие подключения');
                for(const name of ['HTTP','MQTT','TCP','UDP','RS-485','CAN','GPIO','Custom adapter']) {
                    const choice=node('button',name+' — Planned');choice.type='button';choice.disabled=true;choice.dataset.plannedBinding=name;planned.append(choice);
                }
                section.append(planned,node('p','Будущий адаптер преобразует GenericAction в протокол устройства. Эти подключения пока не реализованы.'));
            }
        }
        function action(host,w,target,rerender) {
            const answer=w.answer,a=w.entity;
            field(host,'Описание результата',a.description,v=>a.description=v,{local:true});
            field(host,'Как попросить об этом (по одной фразе)',a.speech_aliases.join('\n'),v=>a.speech_aliases=M.list(v),{multiline:true,local:true,id:'simple-intents'});
            field(host,'Нужно ли указывать значение в команде?',String(answer.needsValue),v=>{answer.needsValue=v==='true';rerender();},{choices:[{id:'true',name:'Да, пользователь назовёт значение'},{id:'false',name:'Нет, результат всегда один'}],local:true,id:'simple-needs-value'});
            field(host,'Что должно измениться?',answer.fieldId,v=>{answer.fieldId=v;const p=target?.state.find(p=>p.id===v);if(p)answer.fixed=M.initialValue(p);rerender();},{choices:[{id:'',name:'Новая характеристика'},...(target?.state||[]).map(p=>({id:p.id,name:p.name}))],local:true,id:'simple-property'});
            const p=target?.state.find(p=>p.id===answer.fieldId)||answer.property;
            if(!answer.fieldId) {
                field(host,'Название характеристики',p.name,v=>p.name=v,{local:true,id:'simple-property-name'});
                field(host,'Какое значение?',p.type,v=>{for(const k of ['minimum','maximum','min_length','max_length','values'])delete p[k];p.type=v;
                    if(['integer','number'].includes(v))Object.assign(p,{minimum:0,maximum:100});
                    if(v==='string')Object.assign(p,{min_length:0,max_length:128});
                    if(v==='enum')p.values=['normal','eco'];answer.fixed=M.initialValue(p);answer.initial=undefined;rerender();},{choices:types,local:true,id:'simple-value-type'});
                limits(host,p,()=>{},false);
                scalar(host,'Начальное виртуальное значение',p,answer.initial??M.initialValue(p),v=>answer.initial=v,'simple-initial');
            }else host.append(node('p',`${p.name}: ${M.constraintText(p)}`));
            if(answer.needsValue)field(host,'Значение обязательно в команде',answer.required,v=>answer.required=v,{checkbox:true,local:true});
            else scalar(host,'После команды будет',p,answer.fixed,v=>answer.fixed=v,'simple-fixed');
            host.append(node('small','Это настройка виртуального результата. Конфигурацию ещё нужно проверить, сохранить и применить.'));
        }
        return {limits,capability,deviceLimits,execution,action};
    }
    root.SimpleEditor={create};
})(window);
