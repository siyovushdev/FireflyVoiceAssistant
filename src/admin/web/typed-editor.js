/* Form controls for the same v2 scalar and assignment schema used by the backend. */
(function(root){
    'use strict';
    function create({node,field,button,changed}) {
        const types=['boolean','integer','number','string','enum'];
        const basic=(id='value')=>({id,name:'Значение',type:'integer',required:true,description:'',unit:'',minimum:0,maximum:100});
        function resetType(p,type){for(const k of ['minimum','maximum','min_length','max_length','values'])delete p[k];p.type=type;
            if(type==='integer'||type==='number'){p.minimum=0;p.maximum=100;}if(type==='string'){p.min_length=0;p.max_length=128;}if(type==='enum')p.values=['normal','eco'];}
        function initial(p){return p.type==='boolean'?false:p.type==='enum'?p.values[0]:p.type==='string'?'':p.minimum;}
        function value(host,label,p,current,set){
            if(p.type==='boolean')field(host,label,current,set,{checkbox:true,local:true});
            else if(p.type==='enum')field(host,label,current,set,{choices:p.values.map(v=>({id:v,name:v})),local:true});
            else field(host,label,current,v=>{if(['integer','number'].includes(p.type)){if(!Number.isFinite(v)||p.type==='integer'&&!Number.isSafeInteger(v))throw Error('Требуется точное конечное число');}set(v);},{number:['integer','number'].includes(p.type),local:true});
        }
        function parameter(host,p,rerender,state=false){const box=node('div',undefined,'typed-field');host.append(box);
            field(box,'Название поля',p.name,v=>{p.name=v;changed();},{local:true});
            field(box,'Ключ поля',p.id,v=>{p.id=v;changed();},{local:true,advanced:true});
            field(box,'Тип значения',p.type,v=>{resetType(p,v);if(state)p.initial=initial(p);changed();rerender();},{choices:types.map(id=>({id,name:{boolean:'Да / нет',integer:'Целое число',number:'Число',string:'Текст',enum:'Выбор из списка'}[id]})),local:true});
            field(box,'Описание поля',p.description,v=>{p.description=v;changed();},{local:true});
            if(!state)field(box,'Обязательный аргумент',p.required,v=>{p.required=v;changed();},{checkbox:true,local:true});
            if(['integer','number'].includes(p.type)){
                if(p.type==='number')box.append(node('small','Число: до 15 значащих цифр, хранение binary64.'));
                field(box,'Единица измерения',p.unit,v=>{p.unit=v;changed();},{local:true});
                field(box,'Минимум',p.minimum,v=>{p.minimum=v;changed();},{number:true,local:true});field(box,'Максимум',p.maximum,v=>{p.maximum=v;changed();},{number:true,local:true});
            }
            if(p.type==='string'){field(box,'Минимальная длина',p.min_length,v=>{p.min_length=v;changed();},{number:true,local:true});field(box,'Максимальная длина',p.max_length,v=>{p.max_length=v;changed();},{number:true,local:true});}
            if(p.type==='enum')field(box,'Разрешённые значения (по одному в строке)',p.values.join('\n'),v=>{p.values=window.AdminModel.list(v);changed();},{multiline:true,local:true,change:rerender});
            if(state)value(box,'Начальное значение',p,p.initial,v=>{p.initial=v;changed();});
        }
        function parameters(host,list,rerender,state=false){for(const p of list){parameter(host,p,rerender,state);button(host,'Удалить поле',()=>{if(confirm(`Удалить поле «${p.name}»?`)){list.splice(list.indexOf(p),1);changed();rerender();}},'remove');}
            button(host,state?'Добавить поле состояния':'Добавить аргумент',()=>{let id='value';for(let n=2;list.some(p=>p.id===id);n++)id=`value_${n}`;const p=basic(id);if(state)p.initial=0;list.push(p);changed();rerender();});}
        function assignments(host,action,target,rerender){host.append(node('h4','Что изменится в виртуальном состоянии'));
            for(const assignment of action.assignments){const box=node('div',undefined,'typed-field');host.append(box);
                const fields=target?.state||[],p=fields.find(p=>p.id===assignment.field);
                field(box,'Поле состояния',assignment.field,v=>{assignment.field=v;delete assignment.argument;assignment.value=initial(fields.find(p=>p.id===v));changed();rerender();},{choices:fields.map(p=>({id:p.id,name:p.name})),local:true});
                field(box,'Источник значения',assignment.argument!==undefined?'argument':'fixed',v=>{if(v==='argument'){delete assignment.value;assignment.argument=action.parameters[0]?.id||'';}else{delete assignment.argument;assignment.value=p?initial(p):'';}changed();rerender();},{choices:[{id:'fixed',name:'Фиксированное значение'},{id:'argument',name:'Из аргумента команды'}],local:true});
                if(assignment.argument!==undefined)field(box,'Аргумент',assignment.argument,v=>{assignment.argument=v;changed();},{choices:action.parameters.map(p=>({id:p.id,name:p.name})),local:true});
                else if(p)value(box,'Присвоить',p,assignment.value,v=>{assignment.value=v;changed();});
                button(box,'Удалить присваивание',()=>{action.assignments.splice(action.assignments.indexOf(assignment),1);changed();rerender();},'remove');
            }
            button(host,'Добавить присваивание',()=>{const p=target?.state?.[0];if(!p){alert('Сначала добавьте поле виртуального состояния устройству.');return;}action.assignments.push({field:p.id,value:initial(p)});changed();rerender();});
        }
        function action(host,a,target,rerender){field(host,'Описание операции',a.description,v=>{a.description=v;changed();},{local:true});field(host,'Фразы действия (точное намерение)',a.speech_aliases.join('\n'),v=>{a.speech_aliases=window.AdminModel.list(v);changed();},{multiline:true,local:true});
            host.append(node('h4','Аргументы команды'));parameters(host,a.parameters,rerender);assignments(host,a,target,rerender);}
        return {basic,initial,value,parameters,action};
    }
    root.TypedEditor={create};
})(window);
