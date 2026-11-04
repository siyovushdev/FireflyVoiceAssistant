import contextlib
import copy
import io
import math
import socket
import os
import unittest
import importlib.util
import sys
from pathlib import Path
spec=importlib.util.spec_from_file_location('xvf_focus',Path(sys.argv.pop(1))/'experiments/xvf3800-focus.py')
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)


class Bus:
    def __init__(self):
        self.identity={'bus':4,'address':4}
        self.values={'VERSION':[2,0,10],'AEC_AZIMUTH_VALUES':[1,1,1,1],
          'AEC_SPENERGY_VALUES':[1,1,1,1], 'AEC_FIXEDBEAMSAZIMUTH_VALUES':[0.,0.],
          'AEC_FIXEDBEAMSONOFF':[0],'AEC_FIXEDBEAMSGATING':[0],
          'AEC_ASROUTONOFF':[1],'AUDIO_MGR_OP_R':[7,3]}
        self.writes=[]
        self.fail_once=None
    def read(self,name):return list(self.values[name])
    def write(self,name,values):
        self.writes.append((name,list(values)))
        if self.fail_once==name:
            self.fail_once=None
            self.values[name]=list(values)  # SET arrived, subsequent readback failed
            raise RuntimeError('Injected failure')
        self.values[name]=list(values)


class FocusTests(unittest.TestCase):
    def test_silence_does_not_select_someones_last_direction(self):
        self.assertIsNone(f.direction([(.8,1,0),(.9,1,0)],1))
    def test_stale_or_future_direction_not_used(self):
        self.assertIsNone(f.direction([(0,1,1),(.1,1,1)],1))
        self.assertIsNone(f.direction([(1.1,1,1),(1.2,1,1)],1))
    def test_competing_directions_do_not_lock(self):
        self.assertIsNone(f.direction([(.8,0,1),(.9,math.pi,1)],1))
    def test_circular_wrap_does_not_point_at_opposite_side(self):
        x=f.direction([(.8,.01,1),(.9,2*math.pi-.01,1)],1)
        self.assertLess(min(abs(x),abs(x-2*math.pi)),.001)
    def test_recent_stable_direction(self):
        self.assertAlmostEqual(f.direction([(.8,1,1),(.9,1.01,1)],1),1.005)
    def test_measured_debounced_wake_tail_still_has_valid_direction(self):
        self.assertAlmostEqual(f.direction([(.65,1,1),(.73,1,1)],1),1.)
        self.assertIsNone(f.direction([(.5,1,1),(.59,1,1)],1))
    def test_delayed_cleanup_not_ignored_and_next_wake_can_collect(self):
        row={'_PID':'123','MESSAGE':'VOICE_STATE WAITING_WAKE monotonic_ms=1050'}
        self.assertEqual(f.state_event(row,123,1.8,1.,.5),('WAITING_WAKE',1.05))
        row['MESSAGE']='VOICE_STATE PROCESSING monotonic_ms=1050'
        self.assertEqual(f.state_event(row,123,1.8,1.,.5),('PROCESSING',1.05))
    def test_delayed_wake_never_opens_focus(self):
        row={'_PID':'123','MESSAGE':'VOICE_STATE WAKE_DETECTED monotonic_ms=1050'}
        self.assertIsNone(f.state_event(row,123,1.8,1.,.5))
        self.assertEqual(f.state_event(row,123,1.1,1.,.5),('WAKE_DETECTED',1.05))
    def test_wrong_pid_future_old_or_unordered_event_ignored(self):
        row={'_PID':'124','MESSAGE':'VOICE_STATE WAKE_DETECTED monotonic_ms=1050'}
        self.assertIsNone(f.state_event(row,123,1.1,1.,.5))
        row['_PID']='123'
        self.assertIsNone(f.state_event(row,123,1.,.9,.5))
        self.assertIsNone(f.state_event(row,123,1.1,1.06,.5))
        self.assertIsNone(f.state_event(row,123,1.1,1.,1.08))
    def test_real_kernel_credentials_and_lifecycle_only(self):
        left,right=socket.socketpair(socket.AF_UNIX,socket.SOCK_DGRAM)
        try:
            right.setsockopt(socket.SOL_SOCKET,socket.SO_PASSCRED,1)
            left.send(b'WAKE_DETECTED 1050')
            data,ancillary,flags,_=right.recvmsg(128,socket.CMSG_SPACE(12))
            row=f.socket_event(data,ancillary,flags,os.getpid(),os.getuid())
            self.assertEqual(f.state_event(row,os.getpid(),1.1,1.,.5),('WAKE_DETECTED',1.05))
            self.assertIsNone(f.socket_event(data,ancillary,flags,os.getpid()+1,os.getuid()))
            self.assertIsNone(f.socket_event(data,ancillary,flags,os.getpid(),os.getuid()+1))
            self.assertIsNone(f.socket_event(data,[],flags,os.getpid(),os.getuid()))
            self.assertIsNone(f.socket_event(data,ancillary,socket.MSG_CTRUNC,os.getpid(),os.getuid()))
            for invalid in [b'ANGLE 1.5',b'WAKE_DETECTED 1 pid=1',b'WAKE_DETECTED nan',b'X'*200,b'\xff 1000']:
                self.assertIsNone(f.socket_event(invalid,ancillary,0,os.getpid(),os.getuid()))
        finally:left.close();right.close()
    def test_partial_apply_still_restores_whole_changed_state(self):
        for name in ['AEC_FIXEDBEAMSONOFF','AUDIO_MGR_OP_R']:
            with self.subTest(name=name):
                b=Bus();s=f.snapshot(b);before=copy.deepcopy(b.values);c=f.Focus(b,s)
                b.fail_once=name
                with self.assertRaises(RuntimeError):c.lock(2,10)
                self.assertTrue(c.touched)
                with contextlib.redirect_stdout(io.StringIO()):c.release()
                self.assertEqual(b.values,before)
    def test_fixed_owner_mode_rejected_without_writes(self):
        b=Bus();b.values['AEC_FIXEDBEAMSONOFF']=[1]
        with self.assertRaises(RuntimeError):f.snapshot(b)
        self.assertEqual(b.writes,[])
    def test_different_asr_route_rejected_without_writes(self):
        b=Bus();b.values['AUDIO_MGR_OP_R']=[0,0]
        with self.assertRaises(RuntimeError):f.snapshot(b)
        self.assertEqual(b.writes,[])
    def test_replacement_usb_not_modified_by_restore(self):
        b=Bus();s=f.snapshot(b);b.identity={'bus':4,'address':5}
        with self.assertRaises(RuntimeError):f.restore(b,s)
        self.assertEqual(b.writes,[])
    def test_repeated_wake_cannot_extend_focus_forever(self):
        b=Bus();c=f.Focus(b,f.snapshot(b))
        with contextlib.redirect_stdout(io.StringIO()):c.lock(1,10);c.lock(2,29)
        self.assertEqual(c.until,30)
        self.assertEqual(b.values['AEC_FIXEDBEAMSAZIMUTH_VALUES'],[1,1])
    def test_restoration_is_idempotent(self):
        b=Bus();s=f.snapshot(b);c=f.Focus(b,s)
        with contextlib.redirect_stdout(io.StringIO()):
            c.lock(2,10);c.release();f.restore(b,s)
        self.assertEqual(b.values['AEC_FIXEDBEAMSONOFF'],[0])
        self.assertEqual(b.values['AEC_FIXEDBEAMSAZIMUTH_VALUES'],[0,0])
    def test_no_flash_gain_unrelated_mux_or_reboot_write_entrypoint(self):
        bus=object.__new__(f.USB)
        for name in ['SAVE_CONFIGURATION','REBOOT','AUDIO_MGR_OP_R','AEC_ASROUTONOFF','AEC_ASROUTGAIN']:
            with self.subTest(name=name),self.assertRaises(ValueError):bus.write(name,[1])
        for name,values in [('AUDIO_MGR_OP_R',[0,0]),('AEC_FIXEDBEAMSONOFF',[2]),('AEC_FIXEDBEAMSAZIMUTH_VALUES',[float('nan'),1]),
                            ('AEC_FIXEDBEAMSAZIMUTH_VALUES',[-1,0]),('AEC_FIXEDBEAMSAZIMUTH_VALUES',[0])]:
            with self.subTest(values=values),self.assertRaises(ValueError):bus.write(name,values)


if __name__=='__main__':unittest.main()
