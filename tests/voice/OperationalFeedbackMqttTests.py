"""Real private loopback broker/PUBACK/state messages; no physical device/NPU/audio."""
import importlib.util,json,os,select,socket,subprocess,sys,tempfile,threading,time
from pathlib import Path
APP,ROOT,MOSQUITTO=sys.argv[1:];ROOT=Path(ROOT)
spec=importlib.util.spec_from_file_location('mqtt_wire',ROOT/'tests/language/GroundingMqttTests.py');wire=importlib.util.module_from_spec(spec);spec.loader.exec_module(wire)
with tempfile.TemporaryDirectory(prefix='firefly-operational-mqtt-') as temporary:
    root=Path(temporary);root.chmod(0o700);(root/'store').mkdir(mode=0o700)
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    cfg=root/'broker.conf';cfg.write_text(f'listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\nlog_type all\n')
    broker_log=(root/'broker.log').open('w+');broker=subprocess.Popen([MOSQUITTO,'-c',str(cfg)],stdout=broker_log,stderr=broker_log)
    child=None;monitor=None;thread=None;stop=threading.Event();commands=[];mode='none';failures=[];tx=threading.Lock()
    def publish(value,retain=False):
        with tx:monitor.sendall(wire.frame(0x31 if retain else 0x30,wire.string('natural/state')+value.encode()))
    try:
        deadline=time.monotonic()+5
        while monitor is None:
            assert broker.poll() is None and time.monotonic()<deadline
            try:monitor=socket.create_connection(('127.0.0.1',port),.1)
            except OSError:time.sleep(.02)
        monitor.settimeout(2);monitor.sendall(wire.frame(0x10,wire.string('MQTT')+bytes([4,2,0,0])+wire.string('independent-device')))
        assert wire.packet(monitor)==(0x20,b'\0\0')
        monitor.sendall(wire.frame(0x82,b'\0\1'+wire.string('natural/set')+b'\0'));assert wire.packet(monitor)==(0x90,b'\0\1\0')
        publish('ON',True) # retained before runtime subscription; must not become confirmation
        doc=json.loads((ROOT/'tests/admin/v2/system.json').read_text());doc['devices']=doc['devices'][:1]
        doc['devices'][0].update(id='lamp',name='Свет',spoken_aliases=['свет'],room_id='kitchen')
        doc['rooms']=doc['rooms'][:1];doc['rooms'][0].update(id='kitchen',name='Кухня',speech={'mentions':['кухня','кухне'],'locations':['на кухне']})
        doc['system']['voice_dialogue']={'responses':'brief','questions_enabled':True}
        doc['connections']=[{'id':'private_mqtt','name':'Private broker','enabled':True,'type':'mqtt','timeout_ms':1000,'network':{'host':'127.0.0.1','port':port},'mqtt':{'tls':False}}]
        doc['bindings']=[{'target_id':'lamp','action_id':a,'connection_id':'private_mqtt','executor':'mqtt','mqtt':{'topic':'natural/set','payload':v,'qos':1,'retain':False}} for a,v in [('action_on','ON'),('action_off','OFF')]]
        doc['observations']=[{'target_id':'lamp','connection_id':'private_mqtt','executor':'mqtt','mqtt':{'topic':'natural/state','format':'text','field':'power','true_payload':'ON','false_payload':'OFF','qos':0},'fields':{}}]
        initial=root/'initial.json';initial.write_text(json.dumps(doc));initial.chmod(0o600)
        errors=(root/'runtime.log').open('w+')
        child=subprocess.Popen([APP,str(root/'store'),str(initial)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors,text=True)
        assert select.select([child.stdout],[],[],10)[0];hello=json.loads(child.stdout.readline())
        def receive():
            try:
                while not stop.is_set():
                    if not select.select([monitor],[],[],.05)[0]:continue
                    header,body=wire.packet(monitor);assert header==0x30
                    n=int.from_bytes(body[:2],'big');assert body[2:2+n]==b'natural/set'
                    value=body[2+n:].decode();commands.append(value);selected=mode
                    if selected=='none':continue
                    # Actual command receipt is logged only after the publish
                    # window has ended; an opposite sample must not be injected
                    # into the protected pre-PUBACK buffer and silently ignored.
                    end=time.monotonic()+2
                    while (root/'runtime.log').read_text().count('COMMAND_TRACE ')<len(commands):
                        assert time.monotonic()<end;time.sleep(.005)
                    if selected=='delay':time.sleep(.8)
                    publish(('OFF' if value=='ON' else 'ON') if selected=='opposite' else value)
            except BaseException as e:failures.append(repr(e))
        thread=threading.Thread(target=receive,daemon=True);thread.start()
        def voice(text):
            assert not failures,failures
            child.stdin.write(json.dumps({'method':'voice','text':text},ensure_ascii=False)+'\n');child.stdin.flush()
            assert select.select([child.stdout],[],[],5)[0],'voice timeout'
            row=child.stdout.readline();assert row,'native exited';r=json.loads(row);assert not failures,failures;return r
        r=voice('Свет на кухне включен?');assert r['reply']=='Сейчас не могу проверить.' and commands==[],r
        r=voice('включи свет на кухне');assert r['kind']=='accepted' and r['reply']=='Команду отправила, ответа пока нет.' and len(commands)==1,r
        assert r['observation']['devices'][0]['properties']['power']['knowledge']=='predicted'
        mode='echo';r=voice('выключи свет на кухне');assert r['reply']=='' and r['reason']=='state_confirmed' and commands==['ON','OFF'],r
        assert voice('Свет на кухне включен?')['reply']=='Нет.'
        assert voice('Свет на кухне выключен?')['reply']=='Да.'
        mode='opposite';r=voice('включи свет на кухне');assert r['reply']=='Устройство ответило, но выполнение не подтвердилось.' and len(commands)==3,r
        mode='delay';r=voice('включи свет на кухне');assert r['reply']=='Команду отправила, ответа пока нет.' and len(commands)==4,r
        time.sleep(.45);assert voice('Свет на кухне включен?')['reply']=='Да.'
        for text in ['Не выключай свет','Да я включил свет','Чайник на кухне включен?','Свет на Марсе включен?','Что ты умеешь?']:voice(text)
        time.sleep(.1);assert commands==['ON','OFF','ON','ON'] and not failures,(commands,failures)
        received=[s for s in (root/'broker.log').read_text().splitlines() if 'Received PUBLISH' in s and "'natural/set'" in s]
        assert len(received)==4 and all('(d0, q1, r0,' in x for x in received),received
        print('PASS real MQTT: PUBACK-only not silent, fresh matching state silent, opposite not silent, retained rejected, late state/no replay, yes/no read-only, exact4 QoS1 nonretained PUBLISH; synthetic STT/inference only')
    finally:
        stop.set()
        if thread:thread.join(timeout=3)
        if child:
            if not child.stdin.closed:child.stdin.close()
            try:child.wait(timeout=5)
            except subprocess.TimeoutExpired:child.kill();child.wait(timeout=3)
            child.stdout.close()
            if child.returncode:
                errors.seek(0);print(errors.read()[-3500:],file=sys.stderr)
            errors.close()
        if monitor:monitor.close()
        broker.terminate()
        try:broker.wait(timeout=3)
        except subprocess.TimeoutExpired:broker.kill();broker.wait(timeout=3)
        broker_log.close()
