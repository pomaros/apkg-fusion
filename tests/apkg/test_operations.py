# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import os
import tempfile
import unittest
from pathlib import Path
from support import *
for method in ('remove','repair','rollback'):
 fn=getattr(lib,'apkg_plan_'+method,None)
 if fn:fn.argtypes=[C.c_void_p,C.c_void_p,Text,C.POINTER(C.c_void_p),C.c_void_p];fn.restype=C.c_int
lib.apkg_plan_upgrade.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_plan_upgrade.restype=C.c_int

def text(s):
 b=s.encode();return Text(C.cast(C.c_char_p(b),C.c_void_p),len(b))
def op(c,action,name='demo'):
 code,s=snapshot(c);assert code==0;out=C.c_void_p();code=getattr(lib,'apkg_plan_'+action)(c,s,text(name),C.byref(out),None);lib.apkg_snapshot_free(s);return code,out
def run_plan(c,p):
 code,a=approve(c,p);assert code==0;result=execute(c,p,a);lib.apkg_approval_free(a);return result
def install(c,data):
 code,s=snapshot(c);assert code==0;code,pkg=package(c,data,sign(data));assert code==0,code;code,p=install_plan(c,s,pkg);assert code==0,code
 result=run_plan(c,p);lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);assert result==(0,1),result

def payload(path,data,config=False):
 b=data.encode();r=dict(data=b.hex(),mode='420',path=path,sha256=hashlib.sha256(b).hexdigest(),size=str(len(b)),type='file')
 if config:r['config']='preserve'
 return r
class Operations(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0)
 def test_remove_preserves_edits_unknown_and_empty_dirs(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor(files=[payload('changed','old'),dict(path='empty',type='directory'),payload('pristine','old')]))
   (root/'demo/changed').write_text('mine');(root/'demo/unknown').write_text('foreign');(root/'demo/extra').mkdir()
   code,p=op(c,'remove');self.assertEqual(code,0);self.assertTrue((root/'demo/pristine').exists());self.assertEqual(run_plan(c,p),(0,1));self.assertFalse((root/'demo/pristine').exists());self.assertFalse((root/'demo/empty').exists());self.assertEqual((root/'demo/changed').read_text(),'mine');self.assertEqual((root/'demo/unknown').read_text(),'foreign');self.assertTrue((root/'demo/extra').is_dir());code,s=snapshot(c);self.assertEqual(lib.apkg_snapshot_installed_count(s),0);lib.apkg_snapshot_free(s);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_remove_clean_and_missing_package(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());code,p=op(c,'remove');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertFalse((root/'demo').exists());lib.apkg_plan_free(p);self.assertEqual(op(c,'remove')[0],5);lib.apkg_close(c)
 def test_repair_restores_damage_and_missing_preserves_config_and_foreign(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor(files=[payload('config','defaults',True),payload('damaged','correct'),payload('missing','restore')]))
   install(c,descriptor(name='other',files=[payload('config','other',True),payload('damaged','other')]))
   (root/'demo/config').write_text('personal');(root/'demo/damaged').write_text('bad');(root/'demo/missing').unlink();(root/'demo/foreign').write_text('keep');(root/'other/damaged').write_text('untouched')
   code,p=op(c,'repair');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual((root/'demo/config').read_text(),'personal');self.assertEqual((root/'demo/damaged').read_text(),'correct');self.assertEqual((root/'demo/missing').read_text(),'restore');self.assertEqual((root/'demo/foreign').read_text(),'keep');self.assertEqual((root/'other/damaged').read_text(),'untouched');lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_stale_live_bytes_and_links_refused(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());code,p=op(c,'remove');self.assertEqual(code,0);(root/'demo/hello').write_text('late edit');self.assertEqual(run_plan(c,p),(8,0));self.assertEqual((root/'demo/hello').read_text(),'late edit');lib.apkg_plan_free(p);(root/'demo/link').symlink_to(root/'.apkg/state');self.assertEqual(op(c,'repair')[0],2);lib.apkg_close(c)
 def test_remove_and_repair_process_death_and_repeat_recovery(self):
  for action in ('remove','repair'):
   for point in (2,101,102,4,5):
    with self.subTest(action=action,point=point),tempfile.TemporaryDirectory() as d:
     root=Path(d)/'root';c=init(root);install(c,descriptor());original=(root/'.apkg/state').read_bytes();(root/'demo/hello').write_text('damaged');code,p=op(c,action);self.assertEqual(code,0);code,a=approve(c,p);self.assertEqual(code,0);pid=os.fork()
     if pid==0:lib.apkg_test_fault(point,2);execute(c,p,a);os._exit(98)
     _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99);self.assertEqual(recover(c),(0,1 if point>=4 and point<100 else 2));self.assertEqual(recover(c),(0,0))
     if point in (4,5):self.assertEqual((root/'demo/hello').read_text(),'hello\n' if action=='repair' else 'damaged')
     else:self.assertEqual((root/'.apkg/state').read_bytes(),original);self.assertEqual((root/'demo/hello').read_text(),'damaged')
     lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
if __name__=='__main__':unittest.main()

class Upgrade(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0)
 def plan(self,c,data):
  code,s=snapshot(c);self.assertEqual(code,0);code,pkg=package(c,data,sign(data));self.assertEqual(code,0);p=C.c_void_p();code=lib.apkg_plan_upgrade(c,s,pkg,C.byref(p),None);lib.apkg_snapshot_free(s);lib.apkg_package_free(pkg);return code,p
 def newer(self,files=None,version='2.0',revision='1'):
  obj=json.loads(descriptor(files=files));obj['body']['version']=version;obj['body']['revision']=revision;return json.dumps(obj,sort_keys=True,separators=(',',':')).encode()
 def test_upgrade_preserves_personal_files_and_removes_pristine_obsolete(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor(files=[payload('config','old',True),payload('edited','old'),payload('obsolete','old'),payload('program','old')]))
   (root/'demo/config').write_text('my settings');(root/'demo/edited').write_text('personal');(root/'demo/unknown').write_text('foreign')
   code,p=self.plan(c,self.newer([payload('config','new',True),payload('edited','new'),payload('program','new')]));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual((root/'demo/config').read_text(),'my settings');self.assertEqual((root/'demo/edited').read_text(),'personal');self.assertEqual((root/'demo/program').read_text(),'new');self.assertEqual((root/'demo/unknown').read_text(),'foreign');self.assertFalse((root/'demo/obsolete').exists());lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_version_revision_and_prerelease_refusals(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor())
   for version,revision in [('0.9','1'),('1.0','1'),('1.0-rc1','1'),('unorderable','1')]:self.assertEqual(self.plan(c,self.newer(version=version,revision=revision))[0],17)
   code,p=self.plan(c,self.newer(version='1.0',revision='2'));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_unknown_path_takeover_and_type_conflicts_refused(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());(root/'demo/foreign').write_text('mine');self.assertEqual(self.plan(c,self.newer([payload('foreign','theirs')]))[0],14);lib.apkg_close(c)
 def test_upgrade_crash_and_changed_live_recovery(self):
  for point in (101,102,4,5):
   with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);install(c,descriptor());code,p=self.plan(c,self.newer());self.assertEqual(code,0);code,a=approve(c,p);pid=os.fork()
    if pid==0:lib.apkg_test_fault(point,2);execute(c,p,a);os._exit(98)
    _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99)
    if point==102:
     (root/'demo/hello').write_text('edit during recovery');self.assertEqual(recover(c),(9,3));self.assertEqual((root/'demo/hello').read_text(),'edit during recovery');(root/'demo/hello').write_text('hello\n')
    self.assertEqual(recover(c),(0,1 if point in (4,5) else 2));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)

class Rollback(unittest.TestCase):
 plan=Upgrade.plan
 newer=Upgrade.newer
 def last_archive(self,root):
  state=(root/'.apkg/state').read_bytes()
  return next(p for p in root.parent.glob(root.name+'.apkg-history.*') if (p/'decision').exists() and (p/'new-state').read_bytes()==state)
 def test_rollback_restores_retained_bytes_offline_and_preserves_later_edits(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor(files=[payload('config','default',True),payload('program','v1')]))
   (root/'demo/config').write_text('settings before upgrade');code,p=self.plan(c,self.newer([payload('config','new defaults',True),payload('program','v2')]));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);history=self.last_archive(root);(root/'demo/extra').write_text('new personal file');code,p=op(c,'rollback',str(history));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual((root/'demo/program').read_text(),'v1');self.assertEqual((root/'demo/config').read_text(),'settings before upgrade');self.assertEqual((root/'demo/extra').read_text(),'new personal file');self.assertEqual(op(c,'rollback',str(history))[0],8);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_rollback_repair_restores_real_before_image(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());(root/'demo/hello').write_text('before repair');code,p=op(c,'repair');self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);history=self.last_archive(root);code,p=op(c,'rollback',str(history));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual((root/'demo/hello').read_text(),'before repair');lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_rollback_rejects_modified_or_missing_history(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());code,p=self.plan(c,self.newer());self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);history=self.last_archive(root);(history/'old-0/hello').write_text('corrupt backup');self.assertEqual(op(c,'rollback',str(history))[0],10);lib.apkg_close(c)

class MergeRefusals(unittest.TestCase):
 def test_foreign_case_collision_refused_during_planning(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());(root/'demo/Foreign').write_text('mine');data=json.loads(descriptor(files=[payload('foreign','new')]));data['body']['version']='2.0';wiredata=json.dumps(data,sort_keys=True,separators=(',',':')).encode();code,s=snapshot(c);code,pkg=package(c,wiredata,sign(wiredata));p=C.c_void_p();self.assertEqual(lib.apkg_plan_upgrade(c,s,pkg,C.byref(p),None),14);self.assertEqual((root/'demo/Foreign').read_text(),'mine');lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);lib.apkg_close(c)
