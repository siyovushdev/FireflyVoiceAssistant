"""Real bounded Modbus TCP sockets and RTU PTYs; no real serial ports/devices."""
import copy,fcntl,json,os,pathlib,pty,select,socket,struct,subprocess,sys,termios,threading,time,unittest
DRIVER,ROOT=sys.argv[1:3]
sys.argv=sys.argv[:1]
def configuration(kind='modbus_tcp',port=502,serial='/dev/pts/0',register=False):
    c=json.loads(pathlib.Path(ROOT,'config/empty-home.json').read_text())
    field='level' if register else 'power';typ='integer' if register else 'boolean'
    state={'id':field,'name':'Значение','type':typ,'required':True,'description':'','unit':'','initial':0 if register else False}
    if register:state.update(minimum=-32768,maximum=32767)
    c['devices']=[{'id':'device_relay','name':'Реле','kind':'generic','room_id':None,'enabled':True,'spoken_aliases':['реле'],'state':[state],'action_ids':['action_on']}]
    c['actions']=[{'id':'action_on','name':'Установить' if register else 'Включить','enabled':True,'operation':'mock.assign.v1','description':'','speech_aliases':['установи' if register else 'включи'],'parameters':[],'assignments':[{'field':field,'value':-123 if register else True}]}]
    con={'id':'connection_modbus','name':'Тест','type':kind,'enabled':True,'timeout_ms':200,'modbus':{'unit_id':1}}
    if kind=='modbus_tcp':con['network']={'host':'127.0.0.1','port':port}
    else:con['serial']={'port':serial,'baud_rate':9600,'parity':'none','stop_bits':2}
    c['connections']=[con]
    c['bindings']=[{'target_id':'device_relay','action_id':'action_on','connection_id':'connection_modbus','executor':kind,'modbus':{'function':6 if register else 5,'address':17,'data_type':'int16' if register else 'boolean','value':-123 if register else True}}]
    c['observations']=[{'target_id':'device_relay','connection_id':'connection_modbus','executor':kind,'fields':{},'modbus':{'function':3 if register else 1,'address':17,'data_type':'int16' if register else 'boolean','field':field}}]
    return c
def run(c,ops):
    r=subprocess.run([DRIVER],input=json.dumps({'configuration':c,'operations':ops}),text=True,capture_output=True,timeout=15)
    assert r.returncode==0,(r.returncode,r.stderr)
    return json.loads(r.stdout)
def crc(data):
    n=65535
    for b in data:
        n^=b
        for _ in range(8):n=(n>>1)^0xa001 if n&1 else n>>1
    return struct.pack('<H',n)
def response(pdu):
    if pdu[0]<=2:return bytes([pdu[0],1,1])
    if pdu[0]<=4:return bytes([pdu[0],2])+struct.pack('>h',-123)
    return pdu
class Tcp:
    def __init__(self,mode='normal'):
        self.mode=mode;self.requests=[];self.stop=False;self.errors=[];self.overlap=False
        self.s=socket.socket();self.s.bind(('127.0.0.1',0));self.s.listen();self.s.settimeout(.1);self.port=self.s.getsockname()[1]
        self.t=threading.Thread(target=self.loop);self.t.start()
    def loop(self):
        while not self.stop:
            try:client,_=self.s.accept()
            except socket.timeout:continue
            except OSError:return
            with client:
                client.settimeout(.5)
                try:
                    buf=b''
                    while len(buf)<12:
                        b=client.recv(12-len(buf))
                        if not b:break
                        buf+=b
                    if len(buf)<12:continue
                    self.requests.append(buf);pdu=buf[7:];reply=response(pdu)
                    if self.mode=='disconnect':continue
                    if self.mode=='timeout':time.sleep(.35);continue
                    if self.mode=='timeout_once' and len(self.requests)==1:time.sleep(.26);continue
                    if self.mode=='slow':
                        self.overlap|=bool(select.select([self.s],[],[],.12)[0])
                    if self.mode=='exception':reply=bytes([pdu[0]|128,2])
                    if self.mode=='echo':reply=reply[:-1]+bytes([reply[-1]^1])
                    tid=buf[:2] if self.mode!='transaction' else b'\xff\xff'
                    out=tid+b'\0\0'+struct.pack('>H',len(reply)+1)+buf[6:7]+reply
                    if self.mode=='length':out=out[:4]+b'\xff\xff'+out[6:]
                    if self.mode=='unit':out=out[:6]+b'\x02'+out[7:]
                    if self.mode=='fragment':
                        for x in out:client.sendall(bytes([x]));time.sleep(.001)
                    else:client.sendall(out)
                except (OSError,TimeoutError):pass
    def __enter__(self):return self
    def __exit__(self,*args):self.stop=True;self.s.close();self.t.join(timeout=2);assert not self.t.is_alive()
class Rtu:
    def __init__(self,mode='normal'):
        self.mode=mode;self.requests=[];self.stop=False;self.master,self.slave=pty.openpty();self.path=os.ttyname(self.slave)
        self.original=termios.tcgetattr(self.slave);self.t=threading.Thread(target=self.loop);self.t.start()
    def loop(self):
        buf=b''
        while not self.stop:
            try:
                if not select.select([self.master],[],[],.05)[0]:continue
                buf+=os.read(self.master,256)
            except OSError:continue
            while len(buf)>=8:
                frame,buf=buf[:8],buf[8:]
                if crc(frame[:-2])!=frame[-2:]:continue
                self.requests.append(frame);pdu=frame[1:-2]
                if self.mode=='timeout' or (self.mode=='timeout_once' and len(self.requests)==1):continue
                reply=bytes([pdu[0]|128,2]) if self.mode=='exception' else response(pdu)
                data=frame[:1]+reply;data+=crc(data)
                if self.mode=='crc':data=data[:-1]+bytes([data[-1]^1])
                if self.mode=='unit':data=bytes([2])+data[1:-2];data+=crc(data)
                if self.mode=='gap':
                    os.write(self.master,data[:1]);time.sleep(.02);os.write(self.master,data[1:])
                elif self.mode=='trailing':os.write(self.master,data+b'X')
                else:os.write(self.master,data)
    def __enter__(self):return self
    def __exit__(self,*args):
        self.stop=True;self.t.join(timeout=2);assert not self.t.is_alive();assert termios.tcgetattr(self.slave)==self.original,'termios not restored'
        os.close(self.master);os.close(self.slave)
class Tests(unittest.TestCase):
    def test_timeout_then_success_same_executor_without_replay(self):
        for kind,fixture in [('modbus_tcp',Tcp),('modbus_rtu',Rtu)]:
            with self.subTest(kind=kind),fixture('timeout_once') as peer:
                c=configuration(kind,port=peer.port) if kind=='modbus_tcp' else configuration(kind,serial=peer.path)
                r=run(c,[{'mode':'execute'},{'mode':'execute'},{'mode':'poll'}])
                self.assertTrue(r['ok'],r);self.assertFalse(r['results'][0]['success'],r)
                self.assertEqual(r['results'][0]['error'],'modbus_timeout')
                self.assertTrue(all(x['success'] for x in r['results'][1:]),r)
                self.assertEqual(r['results'][0]['properties']['power']['knowledge'],'unknown')
                self.assertEqual(r['results'][1]['properties']['power']['knowledge'],'predicted')
                self.assertEqual(r['results'][2]['properties']['power']['knowledge'],'confirmed')
                self.assertEqual(r['fd_delta'],0)
                time.sleep(.25)
                self.assertEqual(len(peer.requests),3,'timeout must not enqueue a retry or a background write')
    def test_same_bus_serialization_and_lock_deadline(self):
        with Tcp('slow') as peer:
            c=configuration(port=peer.port);c['connections'][0]['timeout_ms']=400
            r=run(c,[{'mode':'parallel','operations':[{'mode':'execute'},{'mode':'execute','start_after_ms':10}]}])
            outcomes=r['results'][0]['parallel']
            self.assertTrue(all(x['success'] for x in outcomes),r)
            self.assertEqual(len(peer.requests),2);self.assertFalse(peer.overlap,'same bus must not have overlapping exchanges')
            self.assertLess(max(x['wall_ms'] for x in outcomes),500);self.assertEqual(r['fd_delta'],0)
        with Tcp('slow') as peer:
            c=configuration(port=peer.port);c['connections'][0]['timeout_ms']=400
            r=run(c,[{'mode':'parallel','operations':[{'mode':'execute'},{'mode':'poll','timeout_ms':50,'start_after_ms':10}]}])
            command,poll=r['results'][0]['parallel']
            self.assertTrue(command['success'],r);self.assertFalse(poll['success'],r)
            self.assertEqual(poll['error'],'modbus_busy');self.assertLess(poll['wall_ms'],150)
            self.assertEqual(len(peer.requests),1);self.assertFalse(peer.overlap);self.assertEqual(r['fd_delta'],0)
    def test_tcp_acceptance_readback_reconnect(self):
        for register in [False,True]:
            with Tcp('fragment') as peer:
                c=configuration(port=peer.port,register=register);r=run(c,[{'mode':'execute'},{'mode':'poll'},{'mode':'execute'}])
                self.assertTrue(r['ok'],r);self.assertTrue(all(x['success'] for x in r['results']),r)
                field='level' if register else 'power'
                self.assertEqual(r['results'][0]['properties'][field]['knowledge'],'predicted')
                self.assertEqual(r['results'][1]['properties'][field]['knowledge'],'confirmed')
                self.assertEqual(r['results'][1]['properties'][field]['value'],-123 if register else True)
                self.assertEqual(len(peer.requests),3);self.assertEqual(peer.requests[0][7:],bytes.fromhex('060011ff85' if register else '050011ff00'))
                self.assertEqual(r['fd_delta'],0)
    def test_tcp_rejects(self):
        for mode,error in [('transaction','modbus_response_mismatch'),('length','modbus_response_mismatch'),('unit','modbus_response_mismatch'),('echo','modbus_response_mismatch'),('exception','modbus_exception'),('disconnect','modbus_disconnected'),('timeout','modbus_timeout')]:
            with self.subTest(mode=mode),Tcp(mode) as peer:
                r=run(configuration(port=peer.port),[{'mode':'execute'}]);x=r['results'][0]
                self.assertFalse(x['success'],r);self.assertTrue(x['error'].startswith(error),r);self.assertLess(x['wall_ms'],600);self.assertEqual(x['properties']['power']['knowledge'],'unknown');self.assertEqual(len(peer.requests),1)
    def test_no_auto_io_or_playground(self):
        with Tcp() as peer:
            r=run(configuration(port=peer.port),[{'mode':'validate'},{'mode':'playground'},{'mode':'none'}])
            self.assertTrue(r['ok'],r);self.assertEqual(len(peer.requests),0)
    def test_rtu_acceptance_readback(self):
        for register in [False,True]:
            with Rtu() as peer:
                r=run(configuration('modbus_rtu',serial=peer.path,register=register),[{'mode':'execute'},{'mode':'poll'},{'mode':'execute'}])
                self.assertTrue(r['ok'],r);self.assertTrue(all(x['success'] for x in r['results']),r);self.assertEqual(r['fd_delta'],0);self.assertEqual(len(peer.requests),3)
                self.assertEqual(peer.requests[0][1:-2],bytes.fromhex('060011ff85' if register else '050011ff00'))
    def test_rtu_errors_and_cleanup(self):
        for mode,error in [('crc','modbus_crc_error'),('unit','modbus_response_mismatch'),('exception','modbus_exception'),('timeout','modbus_timeout'),('gap','modbus_timeout'),('trailing','modbus_response_mismatch')]:
            with self.subTest(mode=mode),Rtu(mode) as peer:
                c=configuration('modbus_rtu',serial=peer.path);count=1 if mode=='gap' else 4;r=run(c,[{'mode':'execute'}]*count)
                self.assertTrue(r['ok'],r);self.assertTrue(all(not x['success'] and x['error'].startswith(error) and x['wall_ms']<600 for x in r['results']),r);self.assertEqual(r['fd_delta'],0);self.assertEqual(len(peer.requests),count)
    def test_rtu_cooperative_busy(self):
        with Rtu() as peer:
            fcntl.flock(peer.slave,fcntl.LOCK_EX|fcntl.LOCK_NB)
            r=run(configuration('modbus_rtu',serial=peer.path),[{'mode':'execute'}])
            self.assertFalse(r['results'][0]['success']);self.assertEqual(r['results'][0]['error'],'modbus_port_busy');self.assertEqual(peer.requests,[])
            fcntl.flock(peer.slave,fcntl.LOCK_UN)
    def test_validation_no_io(self):
        with Tcp() as peer:
            c=configuration(port=peer.port)
            mutations=[(['connections',0,'modbus','unit_id'],0),(['connections',0,'network','host'],'example.com'),(['bindings',0,'modbus','address'],65536),(['bindings',0,'modbus','value'],1),(['bindings',0,'modbus','function'],15),(['bindings',0,'modbus','data_type'],'float32'),(['bindings',0,'executor'],'http'),(['observations',0,'modbus','field'],'invented')]
            for keys,value in mutations:
                bad=copy.deepcopy(c);p=bad
                for k in keys[:-1]:p=p[k]
                p[keys[-1]]=value;self.assertFalse(run(bad,[{'mode':'execute'}])['ok'],keys)
            self.assertEqual(len(peer.requests),0)
            rt=configuration('modbus_rtu',serial='/dev/../tmp/file');self.assertFalse(run(rt,[{'mode':'execute'}])['ok'])
    def test_read_functions_and_poll_deadline(self):
        for f in [1,2,3,4]:
            with Tcp() as peer:
                c=configuration(port=peer.port,register=f>=3);c['observations'][0]['modbus']['function']=f
                r=run(c,[{'mode':'poll'}]);self.assertTrue(r['results'][0]['success'],r)
        with Tcp('timeout') as peer:
            r=run(configuration(port=peer.port),[{'mode':'poll','timeout_ms':50}]);self.assertFalse(r['results'][0]['success']);self.assertLess(r['results'][0]['wall_ms'],250)
    def test_explicit_parameter_mapping(self):
        with Tcp() as peer:
            c=configuration(port=peer.port,register=True)
            c['actions'][0]['parameters']=[{'id':'value','name':'Значение','type':'integer','minimum':-1000,'maximum':1000,'required':True,'description':'','unit':''}]
            c['actions'][0]['assignments']=[{'field':'level','argument':'value'}]
            del c['bindings'][0]['modbus']['value'];c['bindings'][0]['modbus']['argument']='value'
            r=run(c,[{'mode':'execute','arguments':{'value':-7}},{'mode':'execute','arguments':{'value':2000}}])
            self.assertTrue(r['ok'],r);self.assertTrue(r['results'][0]['success'],r);self.assertFalse(r['results'][1]['success'],r);self.assertEqual(len(peer.requests),1);self.assertEqual(peer.requests[0][7:],bytes.fromhex('060011fff9'))
if __name__=='__main__':unittest.main()
