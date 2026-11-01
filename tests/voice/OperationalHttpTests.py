"""Real ConfigStore/API/generations/HTTP side effects; STT/model seams are explicit."""
import http.server,json,os,select,subprocess,sys,tempfile,threading,time,urllib.request
from pathlib import Path
APP,ROOT=sys.argv[1:]
class Device(http.server.BaseHTTPRequestHandler):
    on=False;posts=[]
    def log_message(self,*_):pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        type(self).posts.append(body);type(self).on=body['power'];self.send_response(200);self.end_headers();self.wfile.write(b'{}')
    def do_GET(self):
        body=json.dumps({'power':type(self).on}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
with tempfile.TemporaryDirectory(prefix='firefly-operational-') as directory:
    root=Path(directory);root.chmod(0o700);(root/'store').mkdir(mode=0o700)
    device=http.server.ThreadingHTTPServer(('127.0.0.1',0),Device);thread=threading.Thread(target=device.serve_forever,daemon=True);thread.start()
    doc=json.loads((Path(ROOT)/'tests/admin/v2/system.json').read_text());doc['devices']=doc['devices'][:1];d=doc['devices'][0];d.update(id='lamp',name='Свет',spoken_aliases=['свет','лампа','лампу'],room_id='kitchen')
    room=doc['rooms'][0];room.update(id='kitchen',name='Кухня',speech={'mentions':['кухня','кухне'],'locations':['на кухне']});doc['rooms']=[room]
    doc['system']['voice_dialogue']={'responses':'brief','questions_enabled':True}
    doc['connections']=[{'id':'loopback','name':'Изолированный HTTP','enabled':True,'type':'http','timeout_ms':300,'http':{'base_url':f'http://127.0.0.1:{device.server_port}'}}]
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
        assert select.select([child.stdout],[],[],15)[0];hello=json.loads(child.stdout.readline());url=hello['url'];pid=hello['pid']
        before=(root/'store/configuration.json').read_bytes()
        for text in ['Ты включила свет?','Что сейчас включено?','А на кухне?','А он сейчас работает?','Почему команда не выполнилась?','Как прошёл запуск?']:
            r=voice(text);assert r['route']=='OPERATIONAL_QUESTION' and r['reply'],r
        assert Device.posts==[]
        r=voice('включи свет на кухне');assert Device.posts==[{'power':True}],r
        assert r['kind']=='accepted' and 'не подтверждает состояние устройства' in r['reply'],r
        for text in ['Ты включила свет?','Что сейчас включено?','Что ты сделала последним?','А он сейчас работает?']:
            r=voice(text);assert 'подтверждённому наблюдению' not in r['reply'],r
        assert len(Device.posts)==1
        active=api('/api/config/active')['active'];state_poll=api('/api/devices/poll',{'target_id':'lamp','expected_hash':active['hash']});assert state_poll['ok']
        r=voice('Свет на кухне включен?');assert 'подтверждённому наблюдению' in r['reply'],r
        # PR43 independent findings, actual production classes and exact I/O.
        r=voice('Что ты сделала последним?');assert 'Свет' in r['reply'],r
        r=voice('А он сейчас работает?');assert 'подтверждённому наблюдению' in r['reply'],r
        r=voice('А она сейчас включена?');assert 'подтверждённому наблюдению' in r['reply'],r
        for text in ['Лампа на кухне включена?','Лампа на кухне выключена?']:
            r=voice(text);assert r['route']=='OPERATIONAL_QUESTION' and 'Свет' in r['reply'],r
        for text in ['Чайник на кухне включен?','Ты включила чайник на кухне?','Почему чайник на кухне не работает?','Свет и чайник на кухне включены?','Как там чайник на кухне?']:
            r=voice(text);assert r['route']=='OPERATIONAL_QUESTION' and '«Свет»' not in r['reply'],r
            assert len(Device.posts)==1,Device.posts
        r=voice('А он сейчас работает?');assert 'О каком устройстве' in r['reply'],r
        r=voice('Не выключай свет на кухне');assert r['kind']=='unsupported',r
        for text in ['Почему команда не выполнилась?','Почему ты не выключила свет на кухне?']:
            r=voice(text);assert 'Ничего не отправлено' in r['reply'] and 'принята исполнителем' not in r['reply'],r
        assert len(Device.posts)==1,Device.posts
        # Fresh unrelated questions cannot create transport calls, even though
        # the fake model always returns [1] (positive malicious control).
        for text in ['Сколько лет луне?','Не выключай свет','Да я выключил свет','Что сейчас включено?']:
            voice(text)
        assert len(Device.posts)==1
        r=voice('выключи свет на кухне');assert Device.posts==[{'power':True},{'power':False}],r
        assert (root/'store/configuration.json').read_bytes()==before
        # A leading location request reaches the unchanged safe execution path
        # with questions disabled AND enabled, once per explicit utterance.
        for enabled in [False,True]:
            stored=api('/api/config');candidate=stored['configuration'];candidate['system']['voice_dialogue']={'responses':'brief','questions_enabled':enabled}
            saved=api('/api/config',{'configuration':candidate,'expected':{'revision':stored['revision'],'hash':stored['hash']}},'PUT')
            api('/api/config/apply',{'expected':{'revision':saved['revision'],'hash':saved['hash']}})
            r=voice('А на кухне включи свет');assert r['kind']=='accepted' and r['route']!='OPERATIONAL_QUESTION',r
            r=voice('Лампа на кухне включена?');assert r['route']=='OPERATIONAL_QUESTION',r
            assert ('выключены' in r['reply']) is (not enabled),r
        assert Device.posts==[{'power':True},{'power':False},{'power':True},{'power':True}],Device.posts
        # Existing Save/CAS/Apply publishes preferences without a process restart.
        stored=api('/api/config');candidate=stored['configuration'];candidate['system']['voice_dialogue']={'responses':'off','questions_enabled':False}
        saved=api('/api/config',{'configuration':candidate,'expected':{'revision':stored['revision'],'hash':stored['hash']}},'PUT')
        assert voice('Что сейчас включено?')['mode']=='brief','Save alone must not change active preference'
        api('/api/config/apply',{'expected':{'revision':saved['revision'],'hash':saved['hash']}})
        r=voice('Что сейчас включено?');assert r['mode']=='off' and r['route']=='OPERATIONAL_QUESTION' and 'выключены' in r['reply'],r
        assert api('/api/status')['worker_pid']==pid
        assert len(Device.posts)==4
        epoch_stop=api('/api/voice/stop',{});assert epoch_stop['ok']
        # Recovery/restart reads the persisted preferences, keeps all observations
        # Unknown until fresh evidence (no config.initial pretending to be state).
        child.stdin.close();assert child.wait(timeout=10)==0
        child=subprocess.Popen([APP,str(root/'store'),str(cfg)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True)
        assert select.select([child.stdout],[],[],15)[0];url=json.loads(child.stdout.readline())['url']
        r=voice('Что сейчас включено?');assert r['mode']=='off' and 'выключены' in r['reply']
        obs=r['observation'];assert all(p['knowledge']=='unknown' for dev in obs['devices'] for p in dev['properties'].values())
        print('PASS operational HTTP: 4 exact explicit POST controls incl. leading-location commands with questions off/on; review questions/unknown objects/scoped rejections/pronouns zero extra POST; independent GET; live preferences/restart; no NPU/mic/production')
    finally:
        if not child.stdin.closed:child.stdin.close()
        try:child.wait(timeout=10)
        except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
        child.stdout.close();device.shutdown();device.server_close();thread.join(timeout=2)
        log.seek(0);text=log.read();log.close()
        if child.returncode:print(text[-4000:],file=sys.stderr)
