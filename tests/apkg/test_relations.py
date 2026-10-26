# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import os
import tempfile
import unittest
from pathlib import Path
from support import *
from test_operations import install,op,run_plan,payload
lib.apkg_plan_resolve.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.c_size_t,C.POINTER(C.c_void_p),C.c_size_t,C.c_int,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_plan_resolve.restype=C.c_int

def requirement(name,version='1.0',revision='1',target=TARGET,authority=AUTH):return dict(authority=authority,min_revision=revision,min_version=version,name=name,target=target)
def related(name,version='1.0',depends=(),conflicts=(),replaces=(),recommends=()):
 obj=json.loads(descriptor(name=name));obj['body']['version']=version;obj['body']['relations']=dict(conflicts=list(conflicts),depends=list(depends),recommends=list(recommends),replaces=list(replaces));return json.dumps(obj,sort_keys=True,separators=(',',':')).encode()
def resolve(c,requests,candidates=(),recommends=False):
 code,s=snapshot(c);assert code==0;owned=[]
 for data in [*requests,*candidates]:
  code,pkg=package(c,data,sign(data));assert code==0,(code,data);owned.append(pkg)
 req=(C.c_void_p*len(requests))(*owned[:len(requests)]);pool=(C.c_void_p*len(candidates))(*owned[len(requests):]);out=C.c_void_p();code=lib.apkg_plan_resolve(c,s,req,len(req),pool,len(pool),recommends,C.byref(out),None)
 for pkg in owned:lib.apkg_package_free(pkg)
 lib.apkg_snapshot_free(s);return code,out
class Relations(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0)
 def test_installed_provider_preferred_and_reason_preserved(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,related('library'));code,p=resolve(c,[related('application',depends=[requirement('library')])],[related('library','2.0')]);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));entries=json.loads((root/'.apkg/state').read_bytes())['body']['installed'];self.assertEqual(entries[0]['descriptor']['body']['version'],'1.0');self.assertEqual(entries[1]['reason'],'explicit');self.assertEqual(op(c,'remove','library')[0],15);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_closure_cycles_and_shared_commit(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);a=related('a',depends=[requirement('b')]);b=related('b',depends=[requirement('a')]);code,p=resolve(c,[a],[b]);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertTrue((root/'a/hello').exists());self.assertTrue((root/'b/hello').exists());entries=json.loads((root/'.apkg/state').read_bytes())['body'];self.assertEqual(entries['generation'],'2');self.assertEqual(entries['installed'][1]['reason'],'dependency');lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_missing_wrong_target_and_conflicts_have_no_writes(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);before=(root/'.apkg/state').read_bytes()
   self.assertEqual(resolve(c,[related('a',depends=[requirement('missing')])])[0],15)
   self.assertEqual(resolve(c,[related('a',depends=[requirement('b',target='aarch64-aros-v1-native')])],[related('b')])[0],15)
   self.assertEqual(resolve(c,[related('a',conflicts=[requirement('b')]),related('b')])[0],14)
   self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_close(c)
 def test_replacements_and_optional_recommendations(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,related('old'));code,p=resolve(c,[related('new',replaces=[requirement('old')],recommends=[requirement('extra')])],[related('extra')]);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertFalse((root/'old').exists());self.assertFalse((root/'extra').exists());lib.apkg_plan_free(p)
   code,p=resolve(c,[related('next',recommends=[requirement('extra')])],[related('extra')],True);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));entries=json.loads((root/'.apkg/state').read_bytes())['body']['installed'];self.assertEqual(next(x['reason'] for x in entries if x['descriptor']['body']['name']=='extra'),'recommendation');lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_backtracking_avoids_candidate_conflicts(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);code,p=resolve(c,[related('app',depends=[requirement('provider')])],[related('provider','2.0',conflicts=[requirement('app')]),related('provider','1.0')]);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));entries=json.loads((root/'.apkg/state').read_bytes())['body']['installed'];self.assertEqual(entries[1]['descriptor']['body']['version'],'1.0');lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_multi_package_death_rolls_back_or_finishes_together(self):
  for point in (101,102,4,5):
   with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);code,p=resolve(c,[related('app',depends=[requirement('provider')])],[related('provider')]);self.assertEqual(code,0);code,a=approve(c,p);pid=os.fork()
    if pid==0:lib.apkg_test_fault(point,2);execute(c,p,a);os._exit(98)
    _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99);self.assertEqual(recover(c),(0,1 if point in (4,5) else 2));self.assertEqual((root/'app').exists(),point in (4,5));self.assertEqual((root/'provider').exists(),point in (4,5));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_direct_install_cannot_bypass_dependency_checks(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);data=related('app',depends=[requirement('missing')]);code,pkg=package(c,data,sign(data));self.assertEqual(code,0);code,s=snapshot(c);code,p=install_plan(c,s,pkg);self.assertEqual(code,15);self.assertFalse((root/'app').exists());lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);lib.apkg_close(c)
