"""Real ConfigStore/API/generations/HTTP side effects; STT/model seams are explicit."""
import http.server,json,os,select,subprocess,sys,tempfile,threading,time,urllib.request
from pathlib import Path
APP,ROOT=sys.argv[1:]
class Device(http.server.BaseHTTPRequestHandler):
    on=False;posts=[];gets=0;read_mode="valid";post_mode="valid"
    def log_message(self,*_):pass
    def do_POST(self):
        cls=type(self);body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        cls.posts.append(body);cls.on=body['power']
        if cls.post_mode=='delay':time.sleep(.3)
        try:self.send_response(200);self.end_headers();self.wfile.write(b'{}')
        except OSError:pass
    def do_GET(self):
        cls=type(self);cls.gets+=1;mode=cls.read_mode
        value=not cls.on if mode=='opposite' else cls.on
        body=b'broken-json' if mode=='bad' else json.dumps({'power':value}).encode()
        if mode=='delay':time.sleep(1.25)
        try:self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        except OSError:pass
with tempfile.TemporaryDirectory(prefix='firefly-operational-') as directory:
    root=Path(directory);root.chmod(0o700);(root/'store').mkdir(mode=0o700)
    device=http.server.ThreadingHTTPServer(('127.0.0.1',0),Device);thread=threading.Thread(target=device.serve_forever,daemon=True);thread.start()
    doc=json.loads((Path(ROOT)/'tests/admin/v2/system.json').read_text());doc['devices']=doc['devices'][:1];d=doc['devices'][0];d.update(id='lamp',name='Свет',spoken_aliases=['свет','лампа','лампу'],room_id='kitchen')
    room=doc['rooms'][0];room.update(id='kitchen',name='Кухня',speech={'mentions':['кухня','кухне'],'locations':['на кухне']});doc['rooms']=[room]
    doc['system']['voice_dialogue']={'responses':'brief','questions_enabled':True}
    doc['connections']=[{'id':'loopback','name':'Изолированный HTTP','enabled':True,'type':'http','timeout_ms':5000,'http':{'base_url':f'http://127.0.0.1:{device.server_port}'}}]
    doc['bindings']=[{'target_id':'lamp','action_id':a,'connection_id':'loopback','executor':'http','http':{'method':'POST','path':'/relay','body':{'power':on}}} for a,on in [('action_on',True),('action_off',False)]]
    doc['observations']=[{'target_id':'lamp','connection_id':'loopback','executor':'http','http':{'method':'GET','path':'/state','body':{}},'fields':{'power':'power'}}]
    cfg=root/'initial.json';cfg.write_text(json.dumps(doc,ensure_ascii=False));cfg.chmod(0o600)
    log=(root/'runtime.log').open('w+')
    child=subprocess.Popen([APP,str(root/'store'),str(cfg)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True)
    def message(value):
        child.stdin.write(json.dumps(value,ensure_ascii=False)+'\n');child.stdin.flush()
        assert select.select([child.stdout],[],[],15)[0],'fixture timeout'
        line=child.stdout.readline();assert line,'fixture exited';return json.loads(line)
    def voice(text,**kw):return message(dict(method='voice',text=text,**kw))
    def api(path,body=None,method=None):
        data=None if body is None else json.dumps(body,ensure_ascii=False).encode()
        r=urllib.request.Request(url+path,data=data,method=method,headers={'Content-Type':'application/json','Origin':url,'X-Firefly-Admin':'1'})
        with urllib.request.urlopen(r,timeout=15) as f:return json.load(f)
    try:
        assert select.select([child.stdout],[],[],15)[0];hello=json.loads(child.stdout.readline());url=hello['url']
        before=(root/'store/configuration.json').read_bytes()
        r=voice('включи свет на кухне');assert r['kind']=='accepted' and r['reason']=='state_confirmed' and r['reply']=='',r
        assert Device.posts==[{'power':True}] and Device.gets==1
        assert voice('Почему команда не выполнилась?')['reply']=='Выполнение было подтверждено.'
        Device.on=False
        assert voice('Свет на кухне включен?')['reply']=='Нет.'
        assert voice('Свет на кухне выключен?')['reply']=='Да.'
        Device.read_mode='opposite';r=voice('включи свет на кухне')
        assert r['reply']=='Устройство ответило, но выполнение не подтвердилось.' and r['reason']!='state_confirmed',r
        Device.read_mode='valid';r=voice('Свет на кухне включен?');assert r['reply']=='Да.'
        last_observed=r['observation']['devices'][0]['last_observation_at_ms']
        historical='Последнее показание: питание включено. Сейчас не могу проверить.'
        Device.read_mode='bad';r=voice('Свет на кухне включен?');assert r['reply']==historical,r
        assert r['observation']['devices'][0]['last_observation_at_ms']==last_observed,'malformed HTTP reply refreshed telemetry'
        assert r['observation']['devices'][0]['properties']['power']['last_observed_value'] is True
        Device.read_mode='delay';start=time.monotonic();r=voice('Свет на кухне включен?');elapsed=time.monotonic()-start
        assert elapsed<.9 and r['reply']==historical,(elapsed,r)
        assert r['observation']['devices'][0]['last_observation_at_ms']==last_observed,'timed-out HTTP read refreshed telemetry'
        start=time.monotonic();r=voice('выключи свет на кухне');elapsed=time.monotonic()-start
        assert elapsed<.9 and r['reply']=='Команду отправила, ответа пока нет.' and r['kind']=='accepted',(elapsed,r)
        assert voice('Почему команда не выполнилась?')['reply']=='Не дождалась подтверждения.'
        count=len(Device.posts);reads=Device.gets
        for text in ['Не выключай свет','Да я включил свет','Чайник на кухне включен?','Свет на Марсе включен?','Ты включила чайник?','Что ты умеешь?']:
            voice(text)
        assert len(Device.posts)==count and Device.gets==reads,'negative input caused a command/read'
        Device.read_mode='valid';assert voice('Свет на кухне включен?')['reply']=='Нет.'
        assert len(Device.posts)==3 and (root/'store/configuration.json').read_bytes()==before
        stored=api('/api/config');candidate=stored['configuration'];candidate['connections'][0]['timeout_ms']=100
        saved=api('/api/config',dict(configuration=candidate,expected={'revision':stored['revision'],'hash':stored['hash']}),'PUT')
        api('/api/config/apply',{'expected':{'revision':saved['revision'],'hash':saved['hash']}})
        Device.post_mode='delay';reads=Device.gets;r=voice('включи свет на кухне')
        assert r['kind']=='failed' and r['reply']=='Не удалось подтвердить выполнение.',r
        assert len(Device.posts)==4 and Device.gets==reads
        time.sleep(.4);assert len(Device.posts)==4,'automatic replay after delivery timeout'
        print('PASS natural HTTP: confirmed silence, ON/OFF yes/no, independent contradictory state, cached state/read failure, 500ms read cap against configured5000ms, delivery uncertainty/no retry; exact4 POST; unknown/negative zero GET/POST')
    finally:
        if not child.stdin.closed:child.stdin.close()
        try:child.wait(timeout=10)
        except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
        child.stdout.close();device.shutdown();device.server_close();thread.join(timeout=2);log.close()
