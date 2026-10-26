# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import os
import tempfile
import unittest
from pathlib import Path
from support import *
class AllocationFailures(unittest.TestCase):
 def test_plan_allocation_failures_never_crash_or_write(self):
  for nth in range(1,65):
   with self.subTest(nth=nth),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);r,s=snapshot(c);data=descriptor();r,pkg=package(c,data,sign(data));self.assertEqual(r,0);before=(root/'.apkg/state').read_bytes();pid=os.fork()
    if pid==0:
     lib.apkg_test_fail_alloc(nth);r,p=install_plan(c,s,pkg);lib.apkg_test_fail_alloc(0)
     if p:lib.apkg_plan_free(p)
     os._exit(0 if r in (0,4) else 100+r)
    _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),0);self.assertFalse((root/'demo').exists());self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);lib.apkg_close(c)
 def test_execute_allocation_failures_have_recoverable_outcomes(self):
  for nth in range(1,100):
   with self.subTest(nth=nth),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);p,a=prepared_install(c);read_fd,write_fd=os.pipe();pid=os.fork()
    if pid==0:
     os.close(read_fd);lib.apkg_test_fail_alloc(nth);code,outcome=execute(c,p,a);lib.apkg_test_fail_alloc(0);os.write(write_fd,bytes([code,255 if outcome is None else outcome]));os.close(write_fd);os._exit(0)
    os.close(write_fd);_,status=os.waitpid(pid,0);reported=os.read(read_fd,2);os.close(read_fd);self.assertEqual(os.waitstatus_to_exitcode(status),0);self.assertEqual(len(reported),2);code,outcome=reported
    if code==0:self.assertEqual(outcome,1)
    elif outcome==255:self.assertEqual(code,4)
    elif outcome in (0,2):self.assertEqual(code,4);self.assertFalse((root/'demo').exists())
    else:self.assertEqual(outcome,3);self.assertTrue(Path(str(root)+'.apkg-transaction').exists())
    if Path(str(root)+'.apkg-transaction').exists():self.assertEqual(recover(c)[0],0)
    r,s=snapshot(c);self.assertEqual(r,0)
    if lib.apkg_snapshot_installed_count(s):self.assertEqual((root/'demo/hello').read_bytes(),b'hello\n')
    else:self.assertFalse((root/'demo').exists())
    lib.apkg_snapshot_free(s);lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_signed_read_allocation_failures_are_typed(self):
  for nth in range(1,81):
   with self.subTest(nth=nth),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);data=descriptor();signature=sign(data);pid=os.fork()
    if pid==0:
     lib.apkg_test_fail_alloc(nth);code,pkg=package(c,data,signature);lib.apkg_test_fail_alloc(0)
     if pkg:lib.apkg_package_free(pkg)
     os._exit(0 if code in (0,4) else 100+code)
    _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),0);lib.apkg_close(c)
 def test_presentation_allocation_failure_never_loses_plan(self):
  lib.apkg_plan_format.argtypes=[C.c_void_p,C.POINTER(C.c_void_p)];lib.apkg_plan_format.restype=C.c_int;lib.apkg_client_text_free.argtypes=[C.c_void_p]
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);p,a=prepared_install(c)
   for nth in range(1,51):
    with self.subTest(nth=nth):
     text=C.c_void_p();lib.apkg_test_fail_alloc(nth);code=lib.apkg_plan_format(p,C.byref(text));lib.apkg_test_fail_alloc(0);self.assertIn(code,(0,4));lib.apkg_client_text_free(text);self.assertFalse((root/'demo').exists())
   lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
if __name__=='__main__':unittest.main()
