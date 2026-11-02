#!/usr/bin/env python3
"""Finite operator experiment, no PCM capture, no device commands, no flash writes.

Protocol/IDs: official Seeed python_control/xvf_host.py at
4b49bfd19977c63cf6e90dfe9e5827e74e20bf6d. Uses the installed libusb; no pip.
Only volatile fixed-beam azimuth/mode and the same ASR channel's beam selector
may be written. Never detach an audio driver, claim an audio interface, reset
USB, change gain/rate/format, save flash or replace firmware.
"""
import argparse
import collections
import ctypes as C
import fcntl
import json
import math
import os
from pathlib import Path
import selectors
import signal
import socket
import struct
import subprocess
import time

PARAMS = {
    'VERSION': (48, 0, '3B'),
    'AEC_AZIMUTH_VALUES': (33, 75, '4f'),
    'AEC_SPENERGY_VALUES': (33, 80, '4f'),
    'AEC_FIXEDBEAMSAZIMUTH_VALUES': (33, 81, '2f'),
    'AEC_FIXEDBEAMSONOFF': (33, 37, 'i'),
    'AEC_FIXEDBEAMSGATING': (33, 83, 'B'),
    'AEC_ASROUTONOFF': (33, 35, 'i'),
    'AUDIO_MGR_OP_R': (35, 19, '2B'),
}
WRITABLE = {'AEC_FIXEDBEAMSAZIMUTH_VALUES', 'AEC_FIXEDBEAMSONOFF', 'AUDIO_MGR_OP_R'}
TAU = 2 * math.pi


def log(event, **fields):
    print(json.dumps(dict(event=event, monotonic=time.monotonic(), **fields),
                     ensure_ascii=False), flush=True)


class USB:
    """Bounded endpoint0 vendor transfers; interface configuration untouched."""
    def __init__(self):
        devices = [p for p in Path('/sys/bus/usb/devices').iterdir()
                   if (p/'idVendor').exists() and (p/'idProduct').exists()
                   and (p/'idVendor').read_text().strip() == '2886'
                   and (p/'idProduct').read_text().strip() == '001a']
        if len(devices) != 1:
            raise RuntimeError('Require exactly one Seeed2886:001a device')
        self.identity = dict(bus=int((devices[0]/'busnum').read_text()),
                             address=int((devices[0]/'devnum').read_text()))
        self.lib = C.CDLL('libusb-1.0.so.0')
        self.ctx, self.handle = C.c_void_p(), None
        self.lib.libusb_init.argtypes = [C.POINTER(C.c_void_p)]
        self.lib.libusb_exit.argtypes = [C.c_void_p]
        self.lib.libusb_exit.restype = None
        self.lib.libusb_open_device_with_vid_pid.argtypes = [C.c_void_p, C.c_uint16, C.c_uint16]
        self.lib.libusb_open_device_with_vid_pid.restype = C.c_void_p
        self.lib.libusb_close.argtypes = [C.c_void_p]
        self.lib.libusb_close.restype = None
        self.lib.libusb_get_device.argtypes = [C.c_void_p]
        self.lib.libusb_get_device.restype = C.c_void_p
        self.lib.libusb_get_bus_number.argtypes = [C.c_void_p]
        self.lib.libusb_get_bus_number.restype = C.c_uint8
        self.lib.libusb_get_device_address.argtypes = [C.c_void_p]
        self.lib.libusb_get_device_address.restype = C.c_uint8
        self.lib.libusb_control_transfer.argtypes = [C.c_void_p, C.c_uint8, C.c_uint8,
            C.c_uint16, C.c_uint16, C.POINTER(C.c_ubyte), C.c_uint16, C.c_uint]
        if self.lib.libusb_init(C.byref(self.ctx)) != 0:
            raise RuntimeError('libusb_init failed')
        self.handle = self.lib.libusb_open_device_with_vid_pid(self.ctx, 0x2886, 0x001a)
        if not self.handle:
            self.close()
            raise RuntimeError('USB open failed; no permission or device disappeared')
        actual = self.lib.libusb_get_device(self.handle)
        if self.identity != dict(bus=self.lib.libusb_get_bus_number(actual),
                                 address=self.lib.libusb_get_device_address(actual)):
            self.close()
            raise RuntimeError('USB identity changed during open')

    def close(self):
        if self.handle:
            self.lib.libusb_close(self.handle)
            self.handle = None
        if self.ctx:
            self.lib.libusb_exit(self.ctx)
            self.ctx = C.c_void_p()

    def read(self, name):
        resource, command, fmt = PARAMS[name]
        size = struct.calcsize('<'+fmt) + 1
        deadline = time.monotonic() + .15
        while True:
            remaining = max(1, int((deadline-time.monotonic())*1000))
            buf = (C.c_ubyte * size)()
            n = self.lib.libusb_control_transfer(self.handle, 0xc0, 0,
                command | 0x80, resource, buf, size, min(30, remaining))
            if n != size:
                raise RuntimeError('USB read failed/short '+name+' '+str(n))
            if buf[0] == 0:
                values = list(struct.unpack('<'+fmt, bytes(buf)[1:]))
                if not all(math.isfinite(v) for v in values):
                    raise RuntimeError('Nonfinite USB value '+name)
                return values
            if buf[0] != 64 or time.monotonic() >= deadline:
                raise RuntimeError('USB status/busy '+name+' '+str(buf[0]))
            time.sleep(.002)

    def write(self, name, values):
        if name not in WRITABLE:
            raise ValueError('No writes allowed for '+name)
        if name == 'AEC_FIXEDBEAMSONOFF':
            if values not in ([0], [1]):
                raise ValueError('Invalid fixed mode')
        elif name == 'AUDIO_MGR_OP_R':
            if values not in ([7,0],[7,3]):
                raise ValueError('Only existing ASR auto/fixed beam allowed')
        elif len(values) != 2 or not all(math.isfinite(v) and 0 <= v <= TAU for v in values):
            raise ValueError('Invalid angles')
        resource, command, fmt = PARAMS[name]
        data = struct.pack('<'+fmt, *values)
        buf = (C.c_ubyte * len(data)).from_buffer_copy(data)
        n = self.lib.libusb_control_transfer(self.handle, 0x40, 0, command,
                                             resource, buf, len(data), 60)
        if n != len(data):
            raise RuntimeError('USB write failed/short '+name+' '+str(n))
        got = self.read(name)
        if len(got) != len(values) or any(abs(a-b) > 1e-5 for a,b in zip(got,values)):
            raise RuntimeError('USB write readback mismatch '+name)


def snapshot(bus):
    values = {name:bus.read(name) for name in PARAMS if name != 'AEC_SPENERGY_VALUES'}
    if values['AEC_FIXEDBEAMSONOFF'] != [0]:
        raise RuntimeError('Existing fixed mode; do not override owner settings')
    if values['VERSION'] != [2,0,10] or values['AUDIO_MGR_OP_R'] != [7,3] or values['AEC_ASROUTONOFF'] != [1]:
        raise RuntimeError('Unexpected tested firmware/ASR routing')
    return dict(identity=bus.identity, values=values)


def restore(bus, saved):
    if saved['identity'] != bus.identity or bus.read('VERSION') != saved['values']['VERSION']:
        raise RuntimeError('Device changed; do not write a replacement')
    # Restore original ASR auto selection first, then release fixed beams.
    bus.write('AUDIO_MGR_OP_R', saved['values']['AUDIO_MGR_OP_R'])
    bus.write('AEC_FIXEDBEAMSONOFF', [0])
    bus.write('AEC_FIXEDBEAMSAZIMUTH_VALUES', saved['values']['AEC_FIXEDBEAMSAZIMUTH_VALUES'])
    bus.write('AEC_FIXEDBEAMSONOFF', saved['values']['AEC_FIXEDBEAMSONOFF'])
    for name in ['AEC_FIXEDBEAMSONOFF','AEC_FIXEDBEAMSAZIMUTH_VALUES','AEC_FIXEDBEAMSGATING','AEC_ASROUTONOFF','AUDIO_MGR_OP_R']:
        actual=bus.read(name)
        if len(actual)!=len(saved['values'][name]) or any(abs(x-y)>1e-5 for x,y in zip(actual,saved['values'][name])):
            raise RuntimeError('Restore verification mismatch '+name)
    log('RESTORED', fixed=bus.read('AEC_FIXEDBEAMSONOFF'))


def direction(samples, wake_time):
    recent=[s for s in samples if 0 <= wake_time-s[0] <= .55 and s[2] > 0]
    # KWS accepts a seven-score window (7*32ms), after the final speech energy.
    # Actual owner replay measured last-positive270ms before W. Keep a bounded
    # 400ms lookback, rather than rejecting valid wake tails at250ms.
    if len(recent) < 2 or wake_time-recent[-1][0] > .4:
        return None
    recent=recent[-4:]
    # A short stable recent direction, not an invented speaker identity.
    x=sum(math.cos(s[1]) for s in recent)
    y=sum(math.sin(s[1]) for s in recent)
    if math.hypot(x,y)/len(recent) < .95:
        return None
    return math.atan2(y,x) % TAU


def state_event(row, pid, now, last_event, started):
    """Only fresh wake can OPEN focus. Delayed ordered cleanup must still CLOSE it."""
    if row.get('_PID')!=str(pid):return None
    message=row.get('MESSAGE','')
    if not isinstance(message,str) or not message.startswith('VOICE_STATE '):return None
    words=message.split()
    if len(words)<3 or not words[2].startswith('monotonic_ms='):return None
    try:event_time=int(words[2].split('=',1)[1])/1000
    except ValueError:return None
    if event_time<started-.001 or event_time<last_event or event_time>now:return None
    state=words[1]
    if state in ['WAKE_DETECTED','WAKE_REPEATED'] and now-event_time>.4:return None
    return state,event_time


def socket_event(data, ancillary, flags, pid, uid):
    """Only kernel-authenticated, bounded lifecycle. Never accept target/angle/text."""
    if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) or not 0 < len(data) <= 96:
        return None
    peers=[]
    for level,kind,body in ancillary:
        if level==socket.SOL_SOCKET and kind==socket.SCM_CREDENTIALS and len(body)==12:
            peers.append(struct.unpack('3i',body))
    if len(peers)!=1 or peers[0][0]!=pid or peers[0][1]!=uid:return None
    try:words=data.decode('ascii').split(' ')
    except UnicodeDecodeError:return None
    allowed={'WAKE_DETECTED','WAKE_REPEATED','WAITING_WAKE','PROCESSING','COMMAND_START_TIMEOUT',
             'COMMAND_TIMEOUT','COMMAND_MAX_DURATION','WAKE_UNAVAILABLE','stopping','stopped'}
    if len(words)!=2 or words[0] not in allowed or not words[1].isdigit() or len(words[1])>16:
        return None
    return {'_PID':str(pid),'MESSAGE':'VOICE_STATE '+words[0]+' monotonic_ms='+words[1]}


class Focus:
    def __init__(self, bus, saved):
        self.bus, self.saved, self.until = bus, saved, None
        self.touched = False

    def lock(self, angle, now):
        if self.until is not None:
            return
        self.touched = True  # restore even if the second write fails
        self.bus.write('AEC_FIXEDBEAMSAZIMUTH_VALUES', [angle,angle])
        self.bus.write('AEC_FIXEDBEAMSONOFF', [1])
        # Auto beam3 can still select the free-running beam in fixed mode.
        # Explicitly use processed ASR beam1 while capturing this command.
        self.bus.write('AUDIO_MGR_OP_R', [7,0])
        self.until=now+20
        log('LOCKED', degrees=round(math.degrees(angle),1), maximum_seconds=20)

    def release(self):
        if self.touched:
            restore(self.bus,self.saved)
            self.touched=False
        self.until=None


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('mode',choices=['read','probe','watch','restore'])
    parser.add_argument('--state-dir',type=Path,required=True)
    parser.add_argument('--unit',default='firefly-voice-natural-trial.service')
    parser.add_argument('--pid',type=int)
    parser.add_argument('--seconds',type=int,default=600)
    parser.add_argument('--socket',type=Path,help='Direct application lifecycle; no journal reader')
    args=parser.parse_args()
    if args.seconds < 1 or args.seconds > 600:
        raise ValueError('Finite session1..600seconds required')
    os.umask(0o077)
    args.state_dir.mkdir(mode=0o700,parents=True,exist_ok=True)
    control=Path('/run/firefly-xvf3800-focus')
    control.mkdir(mode=0o700,exist_ok=True)
    with (control/'control.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        bus=USB();focus=None;journal=None;control_socket=None;socket_identity=None
        try:
            path=args.state_dir/'snapshot.json'
            if args.mode == 'restore':
                if path.exists():restore(bus,json.loads(path.read_text()))
                return
            saved=snapshot(bus)
            if args.mode == 'read':
                log('READ_ONLY',**saved)
                return
            if path.exists():
                raise RuntimeError('Consumed snapshot; use a new state directory')
            with path.open('x') as f:json.dump(saved,f);f.flush();os.fsync(f.fileno())
            def interrupted(number,frame):
                raise KeyboardInterrupt
            for sig in [signal.SIGINT,signal.SIGTERM,signal.SIGHUP]:signal.signal(sig,interrupted)
            focus=Focus(bus,saved)
            if args.mode == 'probe':
                # Register-level check only, no mic capture or audio playback.
                angle=bus.read('AEC_AZIMUTH_VALUES')[3] % TAU
                focus.lock(angle,time.monotonic())
                time.sleep(.25)
                log('PROBE_FIXED_READBACK',angles=bus.read('AEC_FIXEDBEAMSAZIMUTH_VALUES'),
                    mode=bus.read('AEC_FIXEDBEAMSONOFF'),asr_output=bus.read('AUDIO_MGR_OP_R'),
                    beams=bus.read('AEC_AZIMUTH_VALUES'))
                return
            if not args.pid or (args.socket is None and args.unit!='firefly-voice-natural-trial.service'):
                raise ValueError('Only the existing owner trial may be observed')
            proc=Path('/proc')/str(args.pid)/'stat'
            if proc.stat().st_uid!=1000:raise ValueError('Unexpected application UID')
            birth=proc.read_text().split(') ',1)[1].split()[19]
            started=time.monotonic();last_event=started-.001
            select=selectors.DefaultSelector()
            if args.socket is not None:
                parent=args.socket.parent.lstat()
                if not args.socket.is_absolute() or len(str(args.socket).encode())>107 or args.socket.exists() or args.socket.is_symlink():
                    raise ValueError('Require a new bounded absolute socket path')
                import stat
                if not stat.S_ISDIR(parent.st_mode) or parent.st_uid!=1000 or parent.st_mode & 0o077:
                    raise ValueError('Require existing private application-owned socket directory')
                control_socket=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM|socket.SOCK_NONBLOCK)
                control_socket.setsockopt(socket.SOL_SOCKET,socket.SO_PASSCRED,1)
                control_socket.bind(str(args.socket))
                os.chmod(args.socket,0o666)  # parent0700; actual sender PID/UID checked below
                meta=args.socket.lstat();socket_identity=(meta.st_dev,meta.st_ino)
                select.register(control_socket,selectors.EVENT_READ)
            else:
                journal=subprocess.Popen(['stdbuf','-oL','journalctl','--follow','--lines=0','--unit='+args.unit,'--output=json','--no-pager'],
                    stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                os.set_blocking(journal.stdout.fileno(),False)
                select.register(journal.stdout,selectors.EVENT_READ)
            samples=collections.deque(maxlen=30);pending=b'';end=time.monotonic()+args.seconds
            current_state='WAITING_WAKE';last_poll=0.;next_check=0.;sample_errors=0
            log('WATCH_READY',unit=args.unit,pid=args.pid,seconds=args.seconds,audio_saved=False,
                source='direct_socket' if control_socket is not None else 'journal')
            while time.monotonic()<end:
                now=time.monotonic()
                if now>=next_check:
                    next_check=now+.5
                    if not proc.exists() or proc.read_text().split(') ',1)[1].split()[19]!=birth:
                        raise RuntimeError('Observed voice process ended/changed')
                    if journal is not None and journal.poll() is not None:raise RuntimeError('Journal reader ended')
                if focus.until is not None and now>=focus.until:
                    log('FOCUS_TIMEOUT');focus.release();samples.clear()
                if focus.until is None and current_state in ['WAITING_WAKE','PROCESSING'] and now-last_poll>=.08:
                    last_poll=now
                    try:
                        azimuth=bus.read('AEC_AZIMUTH_VALUES')[3]
                        energy=bus.read('AEC_SPENERGY_VALUES')[3]
                        if 0<=azimuth<=TAU and energy>=0:
                            samples.append((time.monotonic(),azimuth,energy))
                            sample_errors=0
                    except RuntimeError:
                        sample_errors+=1
                        if sample_errors>=5:raise
                for key,_ in select.select(.015):
                    rows=[]
                    if control_socket is not None:
                        data,ancillary,flags,_=control_socket.recvmsg(128,socket.CMSG_SPACE(12))
                        row=socket_event(data,ancillary,flags,args.pid,1000)
                        if row is not None:rows.append(row)
                    else:
                        data=os.read(key.fd,65536)
                        if not data:raise RuntimeError('Journal EOF')
                        pending+=data
                        if len(pending)>262144:raise RuntimeError('Journal input bound')
                        while b'\n' in pending:
                            line,pending=pending.split(b'\n',1)
                            rows.append(json.loads(line))
                    for row in rows:
                        parsed=state_event(row,args.pid,time.monotonic(),last_event,started)
                        if parsed is None:continue
                        state,event_time=parsed;last_event=event_time
                        if control_socket is not None:
                            log('LIFECYCLE',state=state,delay_ms=round((time.monotonic()-event_time)*1000,2))
                        current_state=state
                        if state in ['WAKE_DETECTED','WAKE_REPEATED']:
                            angle=direction(samples,event_time)
                            if angle is None:
                                positive=[s for s in samples if 0<=event_time-s[0]<=.55 and s[2]>0]
                                log('NO_STABLE_DIRECTION',samples=len(samples),positive_samples=len(positive),
                                    latest_positive_age_ms=round((event_time-positive[-1][0])*1000,1) if positive else None,
                                    event_delay_ms=round((time.monotonic()-event_time)*1000,1))
                            else:focus.lock(angle,time.monotonic())
                        elif state in ['PROCESSING','WAITING_WAKE','COMMAND_START_TIMEOUT','COMMAND_TIMEOUT',
                                       'COMMAND_MAX_DURATION','WAKE_UNAVAILABLE','stopping','stopped']:
                            focus.release();samples.clear()
            log('SESSION_TIMEOUT')
        finally:
            # ExecStopPost repeats restore independently if this process is killed.
            for sig in [signal.SIGINT,signal.SIGTERM,signal.SIGHUP]:signal.signal(sig,signal.SIG_IGN)
            try:
                if focus is not None:focus.release()
            finally:
                if journal is not None:
                    journal.terminate()
                    try:journal.wait(timeout=2)
                    except subprocess.TimeoutExpired:journal.kill();journal.wait(timeout=2)
                if control_socket is not None:
                    control_socket.close()
                    try:
                        meta=args.socket.lstat()
                        if (meta.st_dev,meta.st_ino)==socket_identity:args.socket.unlink()
                    except FileNotFoundError:pass
                bus.close()


if __name__=='__main__':
    main()
