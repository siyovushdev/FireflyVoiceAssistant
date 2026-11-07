"""Opt-in local Qwen language check; not CTest and not native RKLLM evidence.
Usage: python3 ... DRIVER ROOT EVIDENCE_DIR LOCAL_SERVER_URL API_KEY_FILE
No device commands, live ConfigStore, network outside explicit loopback, or model downloads.
"""
import json,pathlib,subprocess,sys,time,urllib.request,urllib.parse
DRIVER,ROOT,OUTPUT,URL,KEY,*selected=sys.argv[1:]
assert urllib.parse.urlparse(URL).hostname in ['127.0.0.1','localhost']
out=pathlib.Path(OUTPUT);out.mkdir(parents=True,exist_ok=True)
base=json.loads(pathlib.Path(ROOT,'config/empty-home.json').read_text())
long_intro='''Хочу настроить домашнего голосового помощника. Ниже описываю всё своими словами, без технического JSON. В квартире есть кухня и кабинет. Мне важно говорить привычными названиями, чтобы потом не запоминать номера устройств. На кухне нужен свет и вытяжка, а в кабинете — настольная лампа. Прошу создать обе зоны и эти три устройства. Кухня — отдельное помещение, кабинет тоже отдельное помещение. Свет и вытяжка относятся именно к кухне; настольная лампа относится только к кабинету. Одинаковые слова в этом описании не означают, что нужно создавать повторные устройства.

Я часто готовлю вечером, работаю в кабинете днём и хочу сначала подготовить понятный черновик. Оборудование физически подключу позднее, адреса и параметры брокера сейчас не задаю. Не нужно придумывать сетевые адреса, topics, аппаратные подключения или проверки реальных устройств. Я просто описываю будущую конфигурацию для проверки в интерфейсе.

Названия хотелось бы сохранить понятными: Кухня, Кабинет, Свет, Вытяжка и Настольная лампа. Не добавляйте слова вроде «умное устройство номер один» и не переводите названия на английский язык. Ничего не удаляйте из уже существующей системы. Здесь описывается создание новых объектов. Дополнительные примеры и мои бытовые пояснения не являются просьбой добавить ещё какое-либо оборудование.

Пожалуйста, сначала внимательно прочитайте описание целиком, а затем предложите единый законченный план. Если для описанных объектов чего-либо не хватает, задайте конкретный вопрос. Мне не нужен ответ на первые две строки, за которым будут потеряны остальные требования. Устройства из разных помещений не следует объединять. Повторное упоминание кухни в тексте не создаёт вторую кухню. Это одно и то же помещение. Также одно и то же название Свет ниже относится к уже указанному кухонному свету.

Итоговый список: две зоны, в кухне Свет и Вытяжка, в кабинете Настольная лампа. Для всех трёх устройств нужны только включение и выключение питания. Яркость, регулировку скорости, громкость, расписания и автоматические сценарии не добавлять. Физические подключения пока не настраивать. Проверьте, что последние ограничения тоже вошли в план.'''
cases=[
 {'id':'natural','text':'У меня мастерская, в ней вентилятор. Хочу включать и выключать его голосом. Добавь мастерскую и вентилятор с таким управлением.','zones':1,'devices':1,'bindings':0},
 {'id':'long_document','text':long_intro,'zones':2,'devices':3,'bindings':0},
 {'id':'same_names','text':'Создай зоны Кухня и Кабинет. В каждой зоне добавь устройство Свет. Оба света только включаются и выключаются. Подключений пока не надо.','zones':2,'devices':2,'bindings':0},
 {'id':'mqtt','text':'Создай зону Кухня и устройство Свет с включением и выключением. Создай MQTT подключение Брокер: host 127.0.0.1, port 1883, TLS false. Включение Света: topic home/kitchen/set, payload ON. Выключение: topic home/kitchen/set, payload OFF. QoS 0, retain false. Чтение: topic home/kitchen/state, format text, field power, true_payload ON, false_payload OFF, QoS 0. Все подключения относятся к Свету в Кухне.','zones':1,'devices':1,'bindings':2,'connections':1,'observations':1},
 {'id':'modbus_tcp','text':'Создай зону Стенд и устройство Реле, которое включается и выключается. Создай подключение Контроллер: Modbus TCP, host 127.0.0.1, port 1502, unit_id 1. Включение Реле: function 5, address 17, data_type boolean, value true. Выключение: function 5, address 17, data_type boolean, value false. Чтение Реле: function 1, address 17, field power, data_type boolean. Адреса с нуля. Все привязки относятся к Контроллеру и Реле в зоне Стенд.','zones':1,'devices':1,'bindings':2,'connections':1,'observations':1},
 {'id':'missing_facts','text':'Создай Кухню и свет с включением и выключением, подключи его по MQTT к моему брокеру. Точный адрес и topics я ещё не сообщил.','rejected':True},
 {'id':'unsupported_tail','text':'Создай кухню и свет, который включается и выключается. Также добавь управление вентилятором по UDP.','rejected':True},
 {'id':'late_exclusion','text':'Создай зону Кухня, добавь свет с включением и выключением. Также планировал добавить Вытяжку. Но Вытяжку не добавляй, нужен только Свет.','zones':1,'devices':1,'bindings':0},
]
cases += [
 {'id':'modbus_rtu','text':'Создай зону Стенд и устройство Реле с включением и выключением. Создай Modbus RTU подключение Шина: serial_port /dev/ttyUSB0, baud_rate 9600, parity even, stop_bits 1, unit_id 2. Включение Реле: function 5, address 20, data_type boolean, value true. Выключение: function 5, address 20, data_type boolean, value false. Чтение: function 1, address 20, data_type boolean, field power. Все команды и чтение относятся к Реле в Стенде и подключению Шина.', 'zones':1,'devices':1,'connections':1,'bindings':2,'observations':1},
 {'id':'http','text':'Создай зону Кухня и устройство Свет с включением и выключением. Создай HTTP подключение Стенд: base_url http://127.0.0.1:9001. Включение Света через Стенд: POST /on body {"power":true}. Выключение Света через Стенд: POST /off body {"power":false}. Чтение Света через Стенд: GET /state, JSON поле on сопоставь состоянию power. Все ответы HTTP 200. Все действия относятся к Свету в Кухне.', 'zones':1,'devices':1,'connections':1,'bindings':2,'observations':1},
]
all_count=len(cases)
if selected:
    assert set(selected)<=set(c['id'] for c in cases),'Unknown case filter'
    cases=[c for c in cases if c['id'] in selected]
results=[]
for case in cases:
    start=time.monotonic();record={'case':case,'model_calls':[]};proc=subprocess.Popen([DRIVER],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
    proc.stdin.write(json.dumps({'configuration':base,'text':case['text']},ensure_ascii=False)+'\n');proc.stdin.flush()
    try:
        while True:
            line=proc.stdout.readline()
            if not line:raise RuntimeError('model driver closed: '+proc.stderr.read()[-2000:])
            event=json.loads(line)
            if event['event']=='model_request':
                body={'messages':[{'role':'system','content':event['system']},{'role':'user','content':event['input']}],'temperature':0.1,'top_k':1,'top_p':1.0,'repeat_penalty':1.0,'max_tokens':event['budget'],'stream':False,'chat_template_kwargs':{'enable_thinking':False}}
                request=urllib.request.Request(URL.rstrip('/')+'/v1/chat/completions',data=json.dumps(body).encode(),headers={'Content-Type':'application/json','Authorization':'Bearer '+pathlib.Path(KEY).read_text().strip()})
                t=time.monotonic()
                with urllib.request.urlopen(request,timeout=240) as response:reply=json.load(response)
                raw=reply['choices'][0]['message']['content'];usage=reply.get('usage',{})
                record['model_calls'].append({'system':event['system'],'input':event['input'],'raw':raw,'usage':usage,'finish_reason':reply['choices'][0]['finish_reason'],'seconds':time.monotonic()-t})
                # Match bounded appliance output admission; do not turn a
                # truncated generation into a positive language result.
                if reply['choices'][0]['finish_reason']!='stop' or usage.get('completion_tokens',0)>=event['budget']-1:raw=''
                proc.stdin.write(json.dumps({'raw':raw},ensure_ascii=False)+'\n');proc.stdin.flush()
            elif event['event']=='result':record['result']=event['result'];break
            else:raise RuntimeError(str(event))
        proc.stdin.close();proc.wait(timeout=10)
        result=record['result'];suggestion=result.get('suggestion',{});config=suggestion.get('configuration')
        if case.get('rejected'):record['pass']=not config
        else:
            record['pass']=bool(config) and len(config['rooms'])==case['zones'] and len(config['devices'])==case['devices'] and len(config.get('bindings',[]))==case['bindings']
            if config:
                for key in ['connections','observations']:
                    if key in case:record['pass'] &= len(config.get(key,[]))==case[key]
                record['pass'] &= all(len(d['action_ids'])==2 for d in config['devices'])
                expected_inventory={
                    'natural':{('мастерская','вентилятор')},
                    'long_document':{('кухня','свет'),('кухня','вытяжка'),('кабинет','настольная лампа')},
                    'same_names':{('кухня','свет'),('кабинет','свет')},
                    'mqtt':{('кухня','свет')},'http':{('кухня','свет')},'late_exclusion':{('кухня','свет')},
                    'modbus_tcp':{('стенд','реле')},'modbus_rtu':{('стенд','реле')},
                }
                rooms={r['id']:r['name'].casefold() for r in config['rooms']}
                actual={(rooms[d['room_id']],d['name'].casefold()) for d in config['devices']}
                record['pass'] &= actual==expected_inventory[case['id']]
                actions={a['id']:a for a in config['actions']}
                for d in config['devices']:
                    record['pass'] &= len(d['state'])==1 and d['state'][0]['id']=='power'
                    record['pass'] &= {actions[a]['assignments'][0]['value'] for a in d['action_ids']}=={True,False}
                if case['id'] in ['mqtt','http','modbus_tcp','modbus_rtu']:
                    connection=config['connections'][0];observation=config['observations'][0]
                    record['pass'] &= connection['type']==case['id'] and connection['enabled'] is True
                    for binding in config['bindings']:
                        on=actions[binding['action_id']]['assignments'][0]['value']
                        record['pass'] &= binding['connection_id']==connection['id']
                        if case['id']=='mqtt':
                            record['pass'] &= binding['mqtt']['topic']=='home/kitchen/set' and binding['mqtt']['payload']==('ON' if on else 'OFF') and binding['mqtt']['qos']==0 and binding['mqtt']['retain'] is False
                        elif case['id']=='http':
                            record['pass'] &= binding['http']['method']=='POST' and binding['http']['path']==('/on' if on else '/off') and binding['http']['body']=={'power':on}
                        else:
                            record['pass'] &= binding['modbus']=={'function':5,'address':17 if case['id']=='modbus_tcp' else 20,'data_type':'boolean','value':on}
                    record['pass'] &= observation['connection_id']==connection['id']
                    if case['id']=='mqtt':
                        record['pass'] &= connection['network']=={'host':'127.0.0.1','port':1883} and connection['mqtt']['tls'] is False
                        record['pass'] &= observation['mqtt']['topic']=='home/kitchen/state' and observation['mqtt']['true_payload']=='ON' and observation['mqtt']['false_payload']=='OFF' and observation['mqtt']['field']=='power'
                    elif case['id']=='http':
                        record['pass'] &= connection['http']['base_url']=='http://127.0.0.1:9001' and observation['http']['path']=='/state' and observation['fields']=={'on':'power'}
                    else:
                        record['pass'] &= connection['modbus']['unit_id']==(1 if case['id']=='modbus_tcp' else 2)
                        record['pass'] &= observation['modbus']=={'function':1,'address':17 if case['id']=='modbus_tcp' else 20,'data_type':'boolean','field':'power'}
                        record['pass'] &= connection['network']=={'host':'127.0.0.1','port':1502} if case['id']=='modbus_tcp' else connection['serial']=={'port':'/dev/ttyUSB0','baud_rate':9600,'parity':'even','stop_bits':1}
                if case['id']=='late_exclusion':record['pass'] &= all('вытяж' not in d['name'].lower() for d in config['devices'])
        record['seconds']=time.monotonic()-start
    except Exception as error:record.update({'pass':False,'error':str(error),'seconds':time.monotonic()-start})
    finally:
        if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
    results.append(record);(out/'real-model-results.json').write_text(json.dumps({'backend':'Local Qwen3-4B GGUF Q4_K_M CPU; not native RKLLM acceptance','selected_cases':[c['id'] for c in cases],'all_case_count':all_count,'results':results},ensure_ascii=False,indent=2))
    print(case['id'],'PASS' if record['pass'] else 'FAIL',round(record['seconds'],2),'s',flush=True)
raise SystemExit(0 if all(x['pass'] for x in results) else 1)
