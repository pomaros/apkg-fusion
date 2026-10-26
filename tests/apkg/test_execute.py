# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import os
import tempfile
import unittest
from pathlib import Path
from support import *
class Execute(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0)
 def test_initialize_absent_existing_and_install(self):
  for existing in (False,True):
   with self.subTest(existing=existing),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root,existing);p,a=prepared_install(c);self.assertEqual(execute(c,p,a),(0,1));self.assertEqual((root/'demo/hello').read_bytes(),b'hello\n');self.assertEqual(execute(c,p,a),(8,0));r,s=snapshot(c);self.assertEqual(r,0);self.assertEqual(lib.apkg_snapshot_installed_count(s),1);self.assertEqual(lib.apkg_snapshot_root(s).contents.generation.value,2);lib.apkg_snapshot_free(s);lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_stale_policy_destination_and_read_mode(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);p,a=prepared_install(c);(root/'demo').mkdir();self.assertEqual(execute(c,p,a),(8,0));self.assertFalse((root/'demo/hello').exists());lib.apkg_approval_free(a);lib.apkg_plan_free(p);(root/'demo').rmdir()
   p,a=prepared_install(c);original=(root/'.apkg/state').read_bytes();obj=json.loads(original);obj['body']['generation']='8';(root/'.apkg/state').write_bytes(json.dumps(obj,sort_keys=True,separators=(',',':')).encode());self.assertEqual(execute(c,p,a),(8,0));self.assertFalse((root/'demo').exists());lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_io_before_after_decision_and_recovery(self):
  for point in (1,2,3,4,5):
   with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);p,a=prepared_install(c);lib.apkg_test_fault(point,1);code,outcome=execute(c,p,a);self.assertNotEqual(code,0);self.assertEqual(outcome,3 if point>=4 else (0 if point==1 else 2));lib.apkg_test_fault(0,0)
    if point>=4:self.assertEqual(recover(c),(0,1));self.assertEqual((root/'demo/hello').read_bytes(),b'hello\n')
    else:self.assertFalse((root/'demo').exists())
    self.assertEqual(recover(c),(0,0));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_process_death_each_boundary(self):
  for point in (2,3,4,5):
   with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);p,a=prepared_install(c);pid=os.fork()
    if pid==0:lib.apkg_test_fault(point,2);execute(c,p,a);os._exit(98)
    _,exit_status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(exit_status),99);self.assertEqual(recover(c),(0,1 if point>=4 else 2));self.assertEqual((root/'demo').exists(),point>=4);self.assertEqual(recover(c),(0,0));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_changed_published_files_and_interrupted_recovery(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);p,a=prepared_install(c);pid=os.fork()
   if pid==0:lib.apkg_test_fault(3,2);execute(c,p,a);os._exit(98)
   os.waitpid(pid,0);(root/'demo/hello').write_bytes(b'user edit');self.assertEqual(recover(c),(9,3));self.assertEqual((root/'demo/hello').read_bytes(),b'user edit');(root/'demo/hello').write_bytes(b'hello\n')
   pid=os.fork()
   if pid==0:lib.apkg_test_fault(6,2);recover(c);os._exit(98)
   os.waitpid(pid,0);self.assertEqual(recover(c),(0,2));self.assertEqual(recover(c),(0,0));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_read_context_cannot_approve_or_execute(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';write=init(root);p,a=prepared_install(write);r,read=open_ctx(root,0);self.assertEqual(approve(read,p)[0],17);self.assertEqual(execute(read,p,a),(17,0));self.assertFalse((root/'demo').exists());lib.apkg_close(read);lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(write)
if __name__=='__main__':unittest.main()
