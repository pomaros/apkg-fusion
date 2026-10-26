# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import os
import tempfile
import unittest
from pathlib import Path
from support import *
class Context(unittest.TestCase):
 def test_absent_and_options(self):
  invalid=C.c_void_p(1);self.assertEqual(lib.apkg_snapshot_read(None,C.byref(invalid),None),1);self.assertFalse(invalid.value)
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'absent';before=list(Path(d).iterdir());r,c=open_ctx(p);self.assertEqual(r,0)
   r,s=snapshot(c);self.assertEqual(r,0);v=lib.apkg_snapshot_root(s).contents;self.assertEqual((v.state,v.identity_state,v.generation.state),(0,0,0));self.assertEqual(list(Path(d).iterdir()),before)
   lib.apkg_close(c);self.assertEqual(lib.apkg_snapshot_root(s).contents.state,0);lib.apkg_snapshot_free(s)
   self.assertFalse(p.exists());self.assertEqual(open_ctx(p,budget=70*1024*1024)[0],3)
 def test_independent_snapshots_alias_and_close(self):
  with tempfile.TemporaryDirectory() as d:
   a,b=Path(d)/'a',Path(d)/'b';state(a,1);state(b,9);r,ca=open_ctx(a);r,cb=open_ctx(b);ra,sa=snapshot(ca);rb,sb=snapshot(cb);self.assertEqual((ra,rb),(0,0))
   state(a,2);r,new=snapshot(ca);self.assertEqual(r,0);self.assertEqual(lib.apkg_snapshot_root(sa).contents.generation.value,1);self.assertEqual(lib.apkg_snapshot_root(new).contents.generation.value,2);self.assertEqual(lib.apkg_snapshot_root(sb).contents.generation.value,9)
   alias=Path(d)/'alias';alias.symlink_to(a);r,cc=open_ctx(alias);r,sc=snapshot(cc);self.assertEqual(r,0);self.assertEqual(lib.apkg_snapshot_root(sc).contents.generation.value,2)
   for c in (ca,cb,cc):lib.apkg_close(c)
   for s in (sa,sb,new,sc):lib.apkg_snapshot_free(s)
 def test_pending_invalid_legacy_and_links(self):
  with tempfile.TemporaryDirectory() as d:
   a=Path(d)/'a';state(a);txn=Path(str(a)+'.apkg-transaction');txn.mkdir();r,c=open_ctx(a);r,s=snapshot(c);self.assertEqual(r,0);self.assertEqual(lib.apkg_snapshot_root(s).contents.state,3);lib.apkg_snapshot_free(s);txn.rmdir()
   (a/'.apkg/state').write_bytes(wire('root',{},['future-feature']));r,s=snapshot(c);self.assertEqual(r,0);self.assertEqual(lib.apkg_snapshot_root(s).contents.state,4);lib.apkg_snapshot_free(s)
   (a/'.apkg/state').unlink();(a/'.apkg').rmdir();(a/'db').mkdir();r,s=snapshot(c);self.assertEqual(r,0);self.assertEqual(lib.apkg_snapshot_root(s).contents.state,1);lib.apkg_snapshot_free(s);lib.apkg_close(c)
 def test_malformed_and_limits(self):
  with tempfile.TemporaryDirectory() as d:
   a=Path(d)/'a';state(a);r,c=open_ctx(a)
   for bad in (b'{} ',b'{"a":1}',b'{"a":"x","a":"y"}',b'{"a":"\\ud800"}',b'{"\\u0000hidden":"x"}'):
    (a/'.apkg/state').write_bytes(bad);r,s=snapshot(c);self.assertNotEqual(r,0);self.assertFalse(s.value)
   lib.apkg_close(c)
   state(a);r,c=open_ctx(a,budget=20);r,s=snapshot(c);self.assertEqual(r,3);lib.apkg_close(c)
 def test_rebinding_is_detected(self):
  with tempfile.TemporaryDirectory() as d:
   a=Path(d)/'a';state(a);r,c=open_ctx(a);a.rename(Path(d)/'old');state(a);r,s=snapshot(c);self.assertEqual(r,7);lib.apkg_close(c)
   missing=Path(d)/'later';r,c=open_ctx(missing);state(missing);self.assertEqual(snapshot(c)[0],7);lib.apkg_close(c)
   moved=Path(d)/'moved';state(moved);r,c=open_ctx(moved);actual=Path(d)/'actual';moved.rename(actual);moved.symlink_to(actual);self.assertEqual(snapshot(c)[0],7);lib.apkg_close(c)
 def test_inaccessible_root_and_metadata_symlink(self):
  if os.geteuid()==0:self.skipTest('permission semantics require an unprivileged process')
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root);r,c=open_ctx(root);root.chmod(0)
   try:self.assertEqual(snapshot(c)[0],6)
   finally:root.chmod(0o700);lib.apkg_close(c)
   outside=Path(d)/'outside';state(outside);(root/'.apkg/state').unlink();(root/'.apkg').rmdir();(root/'.apkg').symlink_to(outside/'.apkg');r,c=open_ctx(root);self.assertNotEqual(snapshot(c)[0],0);lib.apkg_close(c)
 def test_legacy_pending_and_mixed_database_fail_closed(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'legacy';(root/'db/transactions/pending').mkdir(parents=True);r,c=open_ctx(root,1);r,s=snapshot(c);self.assertEqual(r,0);v=lib.apkg_snapshot_root(s).contents;self.assertEqual((v.state,v.pending_count.value),(3,1));lib.apkg_snapshot_free(s);self.assertEqual(recover(c),(2,3));lib.apkg_close(c)
   common=Path(d)/'common';state(common,policy=policy());(common/'db').mkdir();r,c=open_ctx(common,1);r,s=snapshot(c);self.assertEqual(r,0);self.assertEqual(lib.apkg_snapshot_root(s).contents.state,4);lib.apkg_snapshot_free(s);data=descriptor();self.assertEqual(package(c,data,sign(data))[0],17);lib.apkg_close(c)
if __name__=='__main__': unittest.main()
