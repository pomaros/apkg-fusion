# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import os
import tempfile
import unittest
from pathlib import Path
from support import *
from test_operations import install,op,run_plan,text
class OperationFailures(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0);lib.apkg_test_fail_alloc(0)
 def test_io_boundaries_have_explicit_recoverable_outcomes(self):
  for point in (1,2,101,102,4,5,6,7):
   with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);install(c,descriptor());(root/'demo/hello').write_text('damage');code,p=op(c,'repair');self.assertEqual(code,0);code,a=approve(c,p);lib.apkg_test_fault(point,1);code,outcome=execute(c,p,a);lib.apkg_test_fault(0,0);self.assertNotEqual(code,0)
    if outcome==3:self.assertEqual(recover(c)[0],0)
    self.assertEqual(recover(c),(0,0));self.assertIn((root/'demo/hello').read_text(),('damage','hello\n'));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_cancellation_boundaries_and_late_commit(self):
  for phase in (0,2,3,4):
   with self.subTest(phase=phase),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);install(c,descriptor());(root/'demo/hello').write_text('damage');code,p=op(c,'repair');self.assertEqual(code,0);code,a=approve(c,p)
    @EVENT_FN
    def event(user,ptr):return int(ptr.contents.phase==phase)
    obs=Observer(event,None,None);result=C.c_void_p();code=lib.apkg_execute(c,p,a,C.byref(obs),C.byref(result),None);view=lib.apkg_result_get(result).contents;self.assertEqual(view.cancellation_requested,1);self.assertEqual(code,0 if phase==4 else 16);self.assertEqual(view.outcome,1 if phase==4 else 0 if phase==0 else 2);lib.apkg_result_free(result);lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_recovery_can_itself_be_interrupted(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());(root/'demo/hello').write_text('damage');code,p=op(c,'repair');code,a=approve(c,p);pid=os.fork()
   if pid==0:lib.apkg_test_fault(102,2);execute(c,p,a);os._exit(98)
   os.waitpid(pid,0);pid=os.fork()
   if pid==0:lib.apkg_test_fault(106,2);recover(c);os._exit(98)
   _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99);self.assertEqual(recover(c),(0,2));self.assertEqual((root/'demo/hello').read_text(),'damage');lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_planner_allocation_failures_do_not_write_or_crash(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());before=(root/'.apkg/state').read_bytes()
   for nth in range(1,181):
    with self.subTest(nth=nth):
     pid=os.fork()
     if pid==0:
      exit_code=120
      try:
       code,s=snapshot(c);p=C.c_void_p();lib.apkg_test_fail_alloc(nth);code=lib.apkg_plan_repair(c,s,text('demo'),C.byref(p),None);lib.apkg_test_fail_alloc(0);lib.apkg_plan_free(p);lib.apkg_snapshot_free(s);exit_code=0 if code in (0,4) else 100+code
      finally:os._exit(exit_code)
     _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),0);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-transaction').exists())
   lib.apkg_close(c)
 def test_executor_allocation_failures_preserve_or_recover(self):
  for nth in range(1,221):
   with self.subTest(nth=nth),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);install(c,descriptor());(root/'demo/hello').write_text('damage');code,p=op(c,'repair');self.assertEqual(code,0);code,a=approve(c,p);read_fd,write_fd=os.pipe();pid=os.fork()
    if pid==0:
     os.close(read_fd);exit_code=120
     try:
      lib.apkg_test_fail_alloc(nth);result=C.c_void_p();code=lib.apkg_execute(c,p,a,None,C.byref(result),None);lib.apkg_test_fail_alloc(0);view=lib.apkg_result_get(result).contents if result else None;os.write(write_fd,bytes([code,view.outcome if view else 255,view.status if view else code]));lib.apkg_result_free(result);exit_code=0
     finally:os._exit(exit_code)
    os.close(write_fd);_,status=os.waitpid(pid,0);report=os.read(read_fd,3);os.close(read_fd);self.assertEqual(os.waitstatus_to_exitcode(status),0);self.assertEqual(len(report),3);code,outcome,status=report;self.assertEqual(code,status)
    if code==0:self.assertEqual(outcome,1)
    elif outcome in (0,2,255):self.assertEqual(code,4)
    else:self.assertEqual(outcome,3);self.assertEqual(code,9)
    if Path(str(root)+'.apkg-transaction').exists():self.assertEqual(recover(c)[0],0)
    self.assertIn((root/'demo/hello').read_text(),('damage','hello\n'));code,s=snapshot(c);self.assertEqual(code,0);lib.apkg_snapshot_free(s);lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_unrepresented_metadata_is_refused_without_loss(self):
  import sys
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());before=(root/'.apkg/state').read_bytes();path=root/'demo/hello'
   if sys.platform=='darwin':
    system=C.CDLL(None);system.setxattr.argtypes=[C.c_char_p,C.c_char_p,C.c_void_p,C.c_size_t,C.c_uint32,C.c_int];self.assertEqual(system.setxattr(str(path).encode(),b'user.apkg-test',b'value',5,0,0),0)
   else:os.setxattr(path,'user.apkg-test',b'value')
   self.assertEqual(op(c,'repair')[0],2);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertEqual(path.read_bytes(),b'hello\n');lib.apkg_close(c)
 def test_commit_callback_cannot_overwrite_foreign_database(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());(root/'demo/hello').write_text('damage');original=(root/'.apkg/state').read_bytes();code,p=op(c,'repair');code,a=approve(c,p)
   @EVENT_FN
   def event(user,ptr):
    if ptr.contents.phase==4:(root/'.apkg/state').write_bytes(b'foreign state')
    return 0
   observer=Observer(event,None,None);self.assertEqual(execute(c,p,a,C.byref(observer)),(9,3));self.assertEqual((root/'.apkg/state').read_bytes(),b'foreign state');self.assertEqual(recover(c),(9,3));(root/'.apkg/state').write_bytes(original);self.assertEqual(recover(c),(0,1));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
