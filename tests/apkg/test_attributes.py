# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Semantic Amiga facts survive host transport without changing host principals."""
import sys
import os
import tempfile
import unittest
from support import *
from test_operations import install, payload, op, run_plan
from test_payload import desc, blob, archive, import_package

ATTR='user.apkg.amiga'
def attrs(comment='café', protection='2', uid='42', gid='123', extra=None):
 return dict(comment=comment.encode().hex(),extra=extra or [],gid=gid,protection=protection,uid=uid)
def encoded(value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()
# Python exposes xattrs on Linux but not every macOS build. Exercise the OS
# interface directly on both hosts, independently of the engine reader.
fs=C.CDLL(None,use_errno=True)
fs.setxattr.argtypes=[C.c_char_p,C.c_char_p,C.c_void_p,C.c_size_t,C.c_uint32]+([C.c_int] if sys.platform=='darwin' else [])
fs.getxattr.argtypes=[C.c_char_p,C.c_char_p,C.c_void_p,C.c_size_t]+([C.c_uint32,C.c_int] if sys.platform=='darwin' else [])
fs.getxattr.restype=C.c_ssize_t
fs.removexattr.argtypes=[C.c_char_p,C.c_char_p]+([C.c_int] if sys.platform=='darwin' else [])
def putattr(path,key,value):
 args=[os.fsencode(path),key.encode(),value,len(value),0]
 if sys.platform=='darwin':args.append(0)
 if fs.setxattr(*args):raise OSError(C.get_errno(),'setxattr')
def dropattr(path,key):
 args=[os.fsencode(path),key.encode()]
 if sys.platform=='darwin':args.append(0)
 if fs.removexattr(*args):raise OSError(C.get_errno(),'removexattr')
def observed(path):
 buf=C.create_string_buffer(16384);args=[os.fsencode(path),ATTR.encode(),buf,len(buf)]
 if sys.platform=='darwin':args.extend([0,0])
 n=fs.getxattr(*args)
 if n<0:raise OSError(C.get_errno(),'getxattr')
 return json.loads(buf.raw[:n])
def setattrs(path,value):putattr(path,ATTR,encoded(value))
def entry(data='first',value=None):
 r=payload('program',data);r['amiga']=value or attrs();return r

def upgrade(c,data):
 code,s=snapshot(c);assert code==0
 code,pkg=package(c,data,sign(data));assert code==0,code
 p=C.c_void_p();code=lib.apkg_plan_upgrade(c,s,pkg,C.byref(p),None)
 lib.apkg_snapshot_free(s);lib.apkg_package_free(pkg);return code,p

class Attributes(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0);lib.apkg_test_fail_alloc(0)
 def test_archive_roundtrip_directories_ids_fullword_and_opaque_fields(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);source=Path(d)/'source';source.write_bytes(b'body')
   value=attrs(protection=str(2**63+2),uid=str(2**32-1),extra=[dict(key=b'foreign-tag'.hex(),value='à conserver'.encode().hex())]);f=blob('program',source,mode='384');f['amiga']=value
   directory=dict(path='Empty',type='directory',mode='488',amiga=attrs(comment='directory',protection='0'))
   data=desc([directory,f]);pkg=archive(Path(d)/'package',data,[('objects/'+f['blob'],source)])
   code,obj=import_package(c,pkg);self.assertEqual(code,0);code,s=snapshot(c);code,p=install_plan(c,s,obj);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(observed(root/'demo/program'),value);self.assertEqual(observed(root/'demo/Empty'),directory['amiga']);self.assertEqual((root/'demo/program').stat().st_mode&0o777,0o600);self.assertEqual((root/'demo/Empty').stat().st_mode&0o777,0o750);self.assertEqual((root/'demo/program').stat().st_uid,os.geteuid());self.assertEqual((root/'demo/program').stat().st_gid,os.getegid())
   lib.apkg_plan_free(p);lib.apkg_package_free(obj);lib.apkg_snapshot_free(s);lib.apkg_close(c)
 def test_upgrade_merges_metadata_independently_from_bytes_and_per_bit(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,desc([entry()]));file=root/'demo/program';personal=attrs(comment='personal',protection='6');setattrs(file,personal)
   nextattrs=attrs(comment='publisher change',protection='10',uid='43',gid='124');code,p=upgrade(c,desc([entry('second',nextattrs)],'2.0'));self.assertEqual(code,0);self.assertIn(b'706572736f6e616c',raw(lib.apkg_plan_get(p).contents.canonical));self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(file.read_text(),'second');actual=observed(file);self.assertEqual(actual['comment'],personal['comment']);self.assertEqual(actual['protection'],'14');self.assertEqual(actual['uid'],'43');self.assertEqual(actual['gid'],'124');lib.apkg_plan_free(p)
   file.write_text('damage');code,p=op(c,'repair');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(file.read_text(),'second');self.assertEqual(observed(file),actual);lib.apkg_plan_free(p)
   code,p=op(c,'remove');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(file.read_text(),'second');self.assertEqual(observed(file),actual);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_clean_remove_and_explicit_default_metadata(self):
  for value in (attrs(),attrs(comment='',uid=None,gid=None)):
   with self.subTest(value=value),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);install(c,desc([entry(value=value)]));code,p=op(c,'remove');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertFalse((root/'demo').exists());lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_late_metadata_edit_invalidates_approved_plan(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,desc([entry()]));code,p=op(c,'remove');self.assertEqual(code,0);setattrs(root/'demo/program',attrs(comment='late edit'));self.assertEqual(run_plan(c,p),(8,0));self.assertEqual(observed(root/'demo/program')['comment'],b'late edit'.hex());lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_malformed_metadata_and_unrepresented_xattrs_refuse_without_writes(self):
  vectors=[dict(comment=b'\xff'.hex()),dict(comment=('x'*80).encode().hex()),dict(comment=b'\n'.hex()),dict(comment='☃'.encode().hex()),dict(uid=str(2**32)),dict(protection='0'),dict(extra=[dict(key=b'uid'.hex(),value=b'x'.hex())]),dict(comment='GG'),dict(unknown='fact')]
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);before=(root/'.apkg/state').read_bytes()
   for changes in vectors:
    value=attrs();value.update(changes);data=desc([entry(value=value)]);code,pkg=package(c,data,sign(data));self.assertEqual(code,2,changes);self.assertFalse(pkg)
   self.assertEqual((root/'.apkg/state').read_bytes(),before);install(c,desc([entry()]));putattr(root/'demo/program','user.unrepresented',b'fact');self.assertEqual(op(c,'repair')[0],2);dropattr(root/'demo/program','user.unrepresented');putattr(root/'demo/program',ATTR,b'broken');self.assertEqual(op(c,'repair')[0],1);lib.apkg_close(c)
 def test_crash_recovery_and_offline_rollback_preserve_metadata(self):
  for point in (101,102,4,5):
   with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);install(c,desc([entry()]));code,p=upgrade(c,desc([entry('second',attrs(comment='new'))],'2.0'));self.assertEqual(code,0);code,a=approve(c,p);self.assertEqual(code,0);pid=os.fork()
    if pid==0:lib.apkg_test_fault(point,2);execute(c,p,a);os._exit(98)
    _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99);self.assertEqual(recover(c),(0,1 if point in (4,5) else 2));self.assertEqual(recover(c),(0,0));self.assertEqual((root/'demo/program').read_text(),'second' if point in (4,5) else 'first');self.assertEqual(observed(root/'demo/program')['comment'],('new' if point in (4,5) else 'café').encode().hex());lib.apkg_approval_free(a);lib.apkg_plan_free(p)
    if point in (4,5):
     state=(root/'.apkg/state').read_bytes();history=next(p for p in root.parent.glob(root.name+'.apkg-history.*') if (p/'decision').exists() and (p/'new-state').read_bytes()==state);code,p=op(c,'rollback',str(history));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual((root/'demo/program').read_text(),'first');self.assertEqual(observed(root/'demo/program'),attrs());lib.apkg_plan_free(p)
    lib.apkg_close(c)

 def test_metadata_planner_allocation_failures_are_typed_and_readonly(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,desc([entry()]));file=root/'demo/program';setattrs(file,attrs(comment='personal'));before=(root/'.apkg/state').read_bytes()
   for nth in range(1,401):
    with self.subTest(nth=nth):
     pid=os.fork()
     if pid==0:
      exitcode=120
      try:
       code,s=snapshot(c);p=C.c_void_p();lib.apkg_test_fail_alloc(nth);code=lib.apkg_plan_repair(c,s,Text(C.cast(C.c_char_p(b'demo'),C.c_void_p),4),C.byref(p),None);lib.apkg_test_fail_alloc(0);lib.apkg_plan_free(p);lib.apkg_snapshot_free(s);exitcode=0 if code in (0,4) else 100+code
      finally:os._exit(exitcode)
     _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),0);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertEqual(observed(file),attrs(comment='personal'));self.assertFalse(Path(str(root)+'.apkg-transaction').exists())
   lib.apkg_close(c)
 def test_metadata_executor_allocation_failures_preserve_or_recover(self):
  for nth in range(1,351):
   with self.subTest(nth=nth),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';c=init(root);install(c,desc([entry()]));file=root/'demo/program';setattrs(file,attrs(comment='personal'));file.write_text('damage');code,p=op(c,'repair');self.assertEqual(code,0);code,a=approve(c,p);self.assertEqual(code,0);pid=os.fork()
    if pid==0:
     exitcode=120
     try:
      lib.apkg_test_fail_alloc(nth);code,outcome=execute(c,p,a);lib.apkg_test_fail_alloc(0);exitcode=0 if code in (0,4,9) else 100+code
     finally:os._exit(exitcode)
    _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),0);self.assertEqual(recover(c)[0],0);self.assertEqual(recover(c),(0,0));self.assertIn(file.read_text(),('damage','first'));self.assertEqual(observed(file),attrs(comment='personal'));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_actual_cli_and_worker_install_identical_metadata(self):
  import test_client as clients
  with tempfile.TemporaryDirectory() as d:
   helper=clients.OperationClients();a,_=helper.setup_root_package(d,'cli');b,_=helper.setup_root_package(d,'worker');data=desc([entry()]);pkg=archive(Path(d)/'package.pkg',data)
   preview=clients.cli('install',a,pkg);self.assertEqual(preview.returncode,16);self.assertIn('comment [café]',preview.stdout);self.assertIn('Amiga protection',preview.stdout);self.assertIn('uid \"42\"',preview.stdout)
   helper.commit('install',a,pkg);helper.worker(b,pkg,0);helper.compare(a,b);self.assertEqual(observed(a/'demo/program'),attrs());self.assertEqual(observed(b/'demo/program'),attrs())
