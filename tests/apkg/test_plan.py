# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import tempfile
import unittest
from pathlib import Path
from support import *
class Plans(unittest.TestCase):
 def test_initialize_preview_has_no_effects(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'absent';r,c=open_ctx(root,1);r,s=snapshot(c);r,p=initialize_plan(c,s);self.assertEqual(r,0);v=lib.apkg_plan_get(p).contents;obj=json.loads(raw(v.canonical));self.assertEqual(obj['body']['before'],'absent');self.assertEqual(obj['body']['new_state']['body']['policy'],json.loads(policy()));self.assertFalse(root.exists());self.assertEqual(list(Path(d).iterdir()),[]);self.assertEqual(hashlib.sha256(raw(v.canonical)).digest(),bytes(v.plan_digest.bytes));r,a=approve(c,p);self.assertEqual(r,0);lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_snapshot_free(s);lib.apkg_close(c)
 def test_install_exactness_and_wrong_session(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=policy());r,c=open_ctx(root,1);r,s=snapshot(c);data=descriptor();r,pkg=package(c,data,sign(data));self.assertEqual(r,0);r,p=install_plan(c,s,pkg);self.assertEqual(r,0);v=lib.apkg_plan_get(p).contents;obj=json.loads(raw(v.canonical));self.assertEqual(obj['body']['new_state']['body']['installed'][0]['descriptor'],json.loads(data));self.assertEqual(v.effect_count,3);self.assertFalse((root/'demo').exists())
   r,other=open_ctx(root,1);self.assertEqual(approve(other,p)[0],8);lib.apkg_close(other)
   state(root,2,policy=policy());self.assertEqual(install_plan(c,s,pkg)[0],8)
   lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);lib.apkg_close(c)
 def test_foreign_destination_refused(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=policy());r,c=open_ctx(root,1);r,s=snapshot(c);data=descriptor();r,pkg=package(c,data,sign(data));(root/'demo').mkdir();self.assertEqual(install_plan(c,s,pkg)[0],14);lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);lib.apkg_close(c)
if __name__=='__main__':unittest.main()
