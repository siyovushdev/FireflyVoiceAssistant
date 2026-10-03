/* Connection and command/observe mappings edit the existing configuration draft. */
(function(root) {
    'use strict';
    function create({node,field,button,changed,api,run}) {
        const M=root.AdminModel;
        function editor(host,device,config) {
            config.connections ||= [];
            config.observations ||= [];
            config.bindings ||= [];
            let selected=config.bindings.find(b=>b.target_id===device.id)?.connection_id||config.observations.find(o=>o.target_id===device.id)?.connection_id||'';
            let network=!!selected, editing=false, actionOpen='', observeOpen=false;
            const update=fn=>v=>{fn(v);changed();};
            const states=device.kind==='light'?[{id:'brightness',name:'Яркость'}]:device.kind==='entrance_door'?[{id:'position',name:'Положение'}]:device.state||[];
            function render() {
                host.replaceChildren(node('h3',device.name));
                host.append(node('p',M.connectionStatusText(M.connectionCompleteness(device,config))));
                if(!network) {
                    host.append(node('h4','Как устройство подключено?'));
                    button(host,'По сети',()=>{network=true;render();});
                    for(const name of ['Последовательное подключение · UART / USB Serial / RS-485','CAN','Локальное устройство']) {
                        const b=button(host,name+' — запланировано',()=>{});b.disabled=true;
                    }
                    return;
                }
                host.append(node('h4','По сети · HTTP / REST'));
                const planned=node('div');host.append(planned);
                for(const name of ['MQTT','TCP','UDP']) {const b=button(planned,name+' — запланировано',()=>{});b.disabled=true;}
                field(host,'Подключение',selected,v=>{
                    selected=v;editing=false;
                    // Rebinding is a visible draft edit; all action mappings remain unchanged.
                    for(const b of config.bindings.filter(b=>b.target_id===device.id&&b.connection_id))b.connection_id=v;
                    const o=config.observations.find(o=>o.target_id===device.id);if(o)o.connection_id=v;
                    changed();render();
                },{local:true,choices:[{id:'',name:'Выберите существующее подключение'},...config.connections.filter(c=>c.type==='http').map(c=>({id:c.id,name:c.name}))],id:'connection-select'});
                button(host,'Создать подключение',()=>{
                    let n=1,id='connection_http_1';const used=M.occupied(config);
                    while(used.includes(id))id=`connection_http_${++n}`;
                    config.connections.push({id,name:'Новое подключение',type:'http',enabled:true,timeout_ms:2000,http:{base_url:'',base_path:''}});
                    selected=id;editing=true;changed();render();
                });
                const connection=config.connections.find(c=>c.id===selected);
                if(!connection)return;
                host.append(node('p',`${connection.name} · ${connection.http.base_url}${connection.http.base_path||''}`));
                if(!editing)button(host,'Изменить подключение',()=>{editing=true;render();});
                if(editing) {
                    host.append(node('small','Адрес общий для всех устройств, использующих это подключение.'));
                    field(host,'Название подключения',connection.name,update(v=>connection.name=v),{local:true,id:'connection-name'});
                    field(host,'Адрес / base URL',connection.http.base_url,update(v=>connection.http.base_url=v),{local:true,id:'connection-base-url'});
                    field(host,'Общий путь (необязательно)',connection.http.base_path||'',update(v=>connection.http.base_path=v),{local:true});
                    field(host,'Таймаут, мс',connection.timeout_ms,update(v=>connection.timeout_ms=v),{local:true,number:true});
                    field(host,'Подключение включено',connection.enabled,update(v=>connection.enabled=v),{local:true,checkbox:true});
                    host.append(node('small','Сейчас: http://IPv4[:порт]. HTTPS и DNS пока не поддерживаются. Секреты, пароли и токены не вводите.'));
                }
                button(host,'Проверить соединение',()=>run(async()=>{
                    await api('/api/connections/test','POST',{configuration:config,connection_id:selected});
                    result.textContent='TCP-соединение установлено. HTTP-команды не отправлялись; API устройства этим тестом не проверяется.';
                }));
                const result=node('p');result.setAttribute('role','status');host.append(result);
                button(host,'Удалить подключение',()=>{
                    if(config.bindings.some(b=>b.connection_id===selected)||config.observations.some(o=>o.connection_id===selected)) {
                        result.textContent='Подключение используется. Сначала удалите или переназначьте его действия и Observe.';return;
                    }
                    config.connections=config.connections.filter(c=>c.id!==selected);selected='';changed();render();
                },'remove');
                host.append(node('h4','Действия устройства'));
                for(const id of M.executableActionIds(device,config)) {
                    const action=config.actions.find(a=>a.id===id), binding=M.bindingFor(config,device.id,id);
                    const box=node('div',undefined,'connection-action');host.append(box);
                    box.append(node('strong',action.name));
                    box.append(node('small',binding?`${binding.http.method} ${binding.http.path||binding.http.url} · ${config.connections.find(c=>c.id===binding.connection_id)?.name||'прежний URL binding'}`:'Не настроено'));
                    button(box,'Настроить',()=>{
                        if(!binding)M.upsertBinding(config,{target_id:device.id,action_id:id,connection_id:selected,executor:'http',http:{method:'POST',path:'',timeout_ms:0,body:{},content_type:'application/json',expected_status:200}});
                        else if(!binding.connection_id) {
                            // Explicit migration: user reviews path, never silently changes the host.
                            const path=new URL(binding.http.url).pathname+new URL(binding.http.url).search;
                            binding.connection_id=selected;binding.http.path=path;delete binding.http.url;
                        }
                        if(binding)binding.connection_id=selected;
                        actionOpen=id;changed();render();
                    });
                    if(!binding||actionOpen!==id)continue;
                    field(box,'HTTP method',binding.http.method,update(v=>binding.http.method=v),{local:true,choices:['POST','PUT','PATCH','DELETE','GET'].map(id=>({id,name:id}))});
                    mapping(box,binding.http,false);
                    field(box,'Аргумент команды (необязательно)',binding.http.argument||'',update(v=>{if(v)binding.http.argument=v;else delete binding.http.argument;}),{local:true,choices:[{id:'',name:'Без аргумента'},...action.parameters.map(p=>({id:p.id,name:p.name}))]});
                    button(box,'Удалить mapping',()=>{M.removeBinding(config,device.id,id);changed();render();},'remove');
                }
                host.append(node('h4','Observe · чтение состояния'));
                let observation=config.observations.find(o=>o.target_id===device.id);
                host.append(node('p',observation?`GET ${observation.http.path} · ${Object.entries(observation.fields).map(([a,b])=>`${a} → ${b}`).join(', ')}`:'Не настроено. После команды состояние будет Predicted.'));
                button(host,'Настроить Observe',()=>{
                    if(!observation){observation={target_id:device.id,connection_id:selected,executor:'http',http:{method:'GET',path:'',timeout_ms:0,expected_status:200},fields:{}};config.observations.push(observation);}
                    observation.connection_id=selected;observeOpen=true;changed();render();
                });
                if(observation&&observeOpen) {
                    mapping(host,observation.http,true);
                    for(const state of states) {
                        field(host,`JSON-поле ответа → ${state.name}`,Object.keys(observation.fields).find(k=>observation.fields[k]===state.id)||'',update(v=>{
                            for(const key of Object.keys(observation.fields))if(observation.fields[key]===state.id)delete observation.fields[key];
                            if(v)observation.fields[v]=state.id;
                        }),{local:true});
                    }
                    host.append(node('small','Только поля верхнего уровня JSON, без преобразования типов. GET выполняется отдельно после Apply кнопкой «Прочитать состояние» в карточке состояния.'));
                    button(host,'Удалить Observe',()=>{config.observations=config.observations.filter(o=>o.target_id!==device.id);changed();render();},'remove');
                }
            }
            function mapping(box,http,observe) {
                field(box,'Путь',http.path||'',update(v=>http.path=v),{local:true});
                if(!observe)field(box,'Тело запроса · JSON',JSON.stringify(http.body||{},null,2),update(v=>{
                    const body=JSON.parse(v);if(!body||typeof body!=='object'||Array.isArray(body))throw Error('Нужен JSON-объект.');http.body=body;
                }),{local:true,multiline:true});
                field(box,'Успешный HTTP status',http.expected_status||200,update(v=>http.expected_status=v),{local:true,number:true});
                const more=node('details');more.append(node('summary','Дополнительные параметры'));box.append(more);
                field(more,'Таймаут, мс (0 — из подключения)',http.timeout_ms||0,update(v=>http.timeout_ms=v),{local:true,number:true});
                if(!observe)field(more,'Content-Type',http.content_type||'application/json',update(v=>http.content_type=v),{local:true});
                field(more,'Заголовки · JSON (Accept / X-*, без секретов)',JSON.stringify(http.headers||{},null,2),update(v=>http.headers=JSON.parse(v)),{local:true,multiline:true});
            }
            render();
        }
        return {editor};
    }
    root.ConnectionEditor={create};
})(window);
