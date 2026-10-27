# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import os
import tempfile
import unittest
from pathlib import Path
from support import *
class FailureCoverage(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0)
 def test_initialization_process_death_and_recovery_interrupted_again(self):
  for existing in (False,True):
   for point in (2,4,5):
    with self.subTest(existing=existing,point=point),tempfile.TemporaryDirectory() as d:
     root=Path(d)/'root'
     if existing:root.mkdir()
     r,c=open_ctx(root,1);r,s=snapshot(c);r,p=initialize_plan(c,s);r,a=approve(c,p);pid=os.fork()
     if pid==0:lib.apkg_test_fault(point,2);execute(c,p,a);os._exit(98)
     _,exit_status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(exit_status),99);self.assertEqual(recover(c),(0,1 if point>=4 else 2));self.assertEqual((root/'.apkg/state').exists(),point>=4);self.assertEqual(recover(c),(0,0));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_snapshot_free(s);lib.apkg_close(c)
 def test_cancellation_and_late_cancellation(self):
  for phase in (0,2,3,4):
   with self.subTest(phase=phase),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);p,a=prepared_install(c);events=[]
    @EVENT_FN
    def event(user,ptr):
     ev=ptr.contents;events.append((ev.sequence,bytes(ev.operation_id.bytes)));return 1 if ev.phase==phase else 0
    obs=Observer(event,None,None);r=C.c_void_p();code=lib.apkg_execute(c,p,a,C.byref(obs),C.byref(r),None);v=lib.apkg_result_get(r).contents
    self.assertEqual(code,0 if phase==4 else 16);self.assertEqual(v.outcome,1 if phase==4 else 0 if phase==0 else 2);self.assertEqual(v.cancellation_requested,1);self.assertEqual((root/'demo').exists(),phase==4);self.assertEqual([x[0] for x in events],list(range(1,len(events)+1)));self.assertEqual(len({x[1] for x in events}),1);lib.apkg_result_free(r);lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_hidden_empty_directory_and_case_collision(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);payload=b'private\n';files=[dict(data=payload.hex(),mode='420',path='.hidden',sha256=hashlib.sha256(payload).hexdigest(),size=str(len(payload)),type='file'),dict(path='Empty',type='directory')];p,a=prepared_install(c,files=files);self.assertEqual(execute(c,p,a),(0,1));self.assertEqual((root/'demo/.hidden').read_bytes(),payload);self.assertEqual(list((root/'demo/Empty').iterdir()),[]);lib.apkg_approval_free(a);lib.apkg_plan_free(p)
   for paths in (('A','a'),('parent/child',),('A','a/child')):
    entries=[dict(path=x,type='directory') for x in paths];data=descriptor(name='other',files=entries);self.assertNotEqual(package(c,data,sign(data))[0],0)
   lib.apkg_close(c)
 def test_missing_or_corrupt_journal_preserved(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);p,a=prepared_install(c);pid=os.fork()
   if pid==0:lib.apkg_test_fault(3,2);execute(c,p,a);os._exit(98)
   os.waitpid(pid,0);journal=Path(str(root)+'.apkg-transaction');original=(journal/'plan').read_bytes();(journal/'plan').write_bytes(b'broken');self.assertEqual(recover(c),(9,3));self.assertTrue(journal.exists());self.assertTrue((root/'demo/hello').exists());(journal/'plan').write_bytes(original);self.assertEqual(recover(c),(0,2));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_parent_lock_refuses_competing_process(self):
  import fcntl
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);p,a=prepared_install(c);fd=os.open(d,os.O_RDONLY);fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
   self.assertEqual(execute(c,p,a),(7,0));self.assertFalse((root/'demo').exists());fcntl.flock(fd,fcntl.LOCK_UN);os.close(fd);lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_live_change_after_check_event_refuses_before_writes(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);p,a=prepared_install(c);original=(root/'.apkg/state').read_bytes()
   @EVENT_FN
   def event(user,ptr):
    if ptr.contents.phase==0:
     changed=json.loads(original);changed['body']['generation']='99';(root/'.apkg/state').write_bytes(json.dumps(changed,sort_keys=True,separators=(',',':')).encode())
    return 0
   obs=Observer(event,None,None);self.assertEqual(execute(c,p,a,C.byref(obs)),(8,0));self.assertFalse((root/'demo').exists());self.assertFalse(Path(str(root)+'.apkg-transaction').exists());lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_reapproved_retry_has_distinct_operation_identity(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);p,a=prepared_install(c);ids=[]
   @EVENT_FN
   def event(user,ptr):
    ids.append(bytes(ptr.contents.operation_id.bytes));return 1
   obs=Observer(event,None,None);self.assertEqual(execute(c,p,a,C.byref(obs)),(16,0));lib.apkg_approval_free(a);r,a=approve(c,p);self.assertEqual(r,0);self.assertEqual(execute(c,p,a,C.byref(obs)),(16,0));self.assertEqual(len(set(ids)),2);lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
if __name__=='__main__':unittest.main()

class RecoveryFollowsRecordedIntent(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fault(0,0)
    def crash(self,c,p,a):
        pid=os.fork()
        if pid==0:
            lib.apkg_test_fault(4,2);execute(c,p,a);os._exit(98)
        os.waitpid(pid,0)
    def test_initialization_recovery_never_takes_over_a_root_created_after_the_decision(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';code,c=open_ctx(root,1);self.assertEqual(code,0);code,s=snapshot(c);code,p=initialize_plan(c,s);code,a=approve(c,p)
            self.crash(c,p,a);self.assertTrue(Path(str(root)+'.apkg-transaction').exists())
            root.mkdir();(root/'foreign.txt').write_text('user data')
            code,c2=open_ctx(root,1);self.assertEqual(recover(c2),(9,3))
            self.assertEqual(sorted(os.listdir(root)),['foreign.txt']);self.assertTrue(Path(str(root)+'.apkg-transaction').exists())
            lib.apkg_close(c2);lib.apkg_close(c)
    def test_recovery_refuses_a_linked_metadata_directory(self):
        import shutil
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c=init(root);p,a=prepared_install(c);self.crash(c,p,a)
            outside=Path(d)/'outside';shutil.move(str(root/'.apkg'),str(outside));os.symlink(str(outside),str(root/'.apkg'));before=(outside/'state').read_bytes()
            self.assertEqual(recover(c),(9,3));self.assertEqual((outside/'state').read_bytes(),before);self.assertTrue(Path(str(root)+'.apkg-transaction').exists())
            lib.apkg_close(c)
