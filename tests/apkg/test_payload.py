# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from support import *
from test_operations import op,run_plan,payload,text
from test_client import cli,WORKER
import test_client as clients
lib.apkg_package_import.argtypes=[C.c_void_p,Text,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_package_import.restype=C.c_int
lib.apkg_test_max_alloc.argtypes=[C.c_int];lib.apkg_test_max_alloc.restype=C.c_size_t

def file_hash(path):
 h=hashlib.sha256()
 with Path(path).open('rb') as f:
  for chunk in iter(lambda:f.read(65536),b''):h.update(chunk)
 return h.hexdigest()
def blob(path,source,mode='420',config=False):
 source=Path(source);sha=file_hash(source);r=dict(blob=sha,mode=mode,path=path,sha256=sha,size=str(source.stat().st_size),type='file')
 if config:r['config']='preserve'
 return r

def desc(files,version='1.0',name='demo'):
 obj=json.loads(descriptor(name=name));obj['body']['version']=version;obj['body']['files']=sorted(files,key=lambda x:x['path']);return json.dumps(obj,sort_keys=True,separators=(',',':')).encode()
def archive(path,data,objects=(),signature=None,extra=()):
 members=[('descriptor.json',data),('signature.json',signature or sign(data)),*objects,*extra]
 def length(value):return value.stat().st_size if isinstance(value,Path) else len(value)
 total=8+sum(9+len(name.encode())+length(value) for name,value in members)
 with Path(path).open('wb') as out:
  out.write(b'PKG\1'+struct.pack('>I',total))
  for name,value in members:
   b=name.encode();out.write(struct.pack('>I',len(b))+b+b'\0'+struct.pack('>I',length(value)))
   if isinstance(value,Path):
    with value.open('rb') as source:
     for chunk in iter(lambda:source.read(65536),b''):out.write(chunk)
   else:out.write(value)
 return Path(path)
def import_package(c,path,observer=None):
 out=C.c_void_p();code=lib.apkg_package_import(c,text(str(path)),observer,C.byref(out),None);return code,out
def patterned(path,size,byte=b'A'):
 with Path(path).open('wb') as f:
  chunk=byte*65536
  while size:
   n=min(size,len(chunk));f.write(chunk[:n]);size-=n
 return Path(path)
def cached(root,sha):return Path(str(root)+'.apkg-objects')/sha

class Payload(unittest.TestCase):
 def test_large_payload_stays_out_of_database_plan_and_allocations(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);source=patterned(Path(d)/'large',32*1024*1024+7);entry=blob('large.bin',source);data=desc([dict(path='Empty',type='directory'),entry]);pkg=archive(Path(d)/'large.pkg',data,[('objects/'+entry['blob'],source)]);before=(root/'.apkg/state').read_bytes();lib.apkg_test_max_alloc(1)
   code,package_ptr=import_package(c,pkg);self.assertEqual(code,0);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'demo').exists());code,s=snapshot(c);code,p=install_plan(c,s,package_ptr);self.assertEqual(code,0);self.assertLess(lib.apkg_plan_get(p).contents.canonical.size,10000);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(file_hash(root/'demo/large.bin'),entry['sha256']);self.assertTrue((root/'demo/Empty').is_dir());self.assertLess((root/'.apkg/state').stat().st_size,10000);self.assertLess(lib.apkg_test_max_alloc(0),1024*1024);lib.apkg_plan_free(p);lib.apkg_package_free(package_ptr);lib.apkg_snapshot_free(s);lib.apkg_close(c)
 def test_archive_tampering_missing_extra_duplicate_and_truncation(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);source=Path(d)/'source';source.write_bytes(b'contents');entry=blob('file',source);data=desc([entry]);normal=('objects/'+entry['blob'],source)
   vectors=[([normal],(),0),([],(),10),([normal],[(normal[0],source)],1),([normal],[('../outside',b'evil')],1),([('objects/'+entry['blob'],b'Contents')],(),10)]
   for number,(objects,extra,expected) in enumerate(vectors):
    with self.subTest(number=number):
     pkg=archive(Path(d)/f'{number}.pkg',data,objects,extra=extra);code,p=import_package(c,pkg);self.assertEqual(code,expected);lib.apkg_package_free(p);self.assertFalse((root/'demo').exists())
   pkg=archive(Path(d)/'truncated.pkg',data,[normal]);pkg.write_bytes(pkg.read_bytes()[:-1]);self.assertEqual(import_package(c,pkg)[0],10);self.assertFalse((Path(d)/'outside').exists());lib.apkg_close(c)
 def test_signature_target_and_corrupt_existing_objects(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);source=Path(d)/'source';source.write_bytes(b'content');entry=blob('file',source);data=desc([entry]);pkg=archive(Path(d)/'pkg',data,[('objects/'+entry['blob'],source)]);code,p=import_package(c,pkg);self.assertEqual(code,0);lib.apkg_package_free(p);cached(root,entry['blob']).write_bytes(b'corrupt');self.assertEqual(import_package(c,pkg)[0],10);self.assertEqual(cached(root,entry['blob']).read_bytes(),b'corrupt');code,p=package(c,data,sign(data));self.assertEqual(code,0);code,s=snapshot(c);self.assertEqual(install_plan(c,s,p)[0],10);lib.apkg_package_free(p);lib.apkg_snapshot_free(s)
   other_pk,other_sk=keypair(bytes(range(1,33)));bad=archive(Path(d)/'bad',data,[('objects/'+entry['blob'],source)],signature=sign(data,other_pk,other_sk));self.assertEqual(import_package(c,bad)[0],12);lib.apkg_close(c)
 def test_full_operation_cycle_retains_large_before_images_offline(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);old=patterned(Path(d)/'old',8*1024*1024+3,b'A');new=patterned(Path(d)/'new',8*1024*1024+5,b'B');conf=Path(d)/'config';conf.write_text('defaults');first=[blob('program',old,'493'),blob('settings',conf,config=True)];second=[blob('program',new,'493'),blob('settings',conf,config=True)]
   p1=archive(Path(d)/'v1.pkg',desc(first),[('objects/'+r['blob'],old if r['path']=='program' else conf) for r in first]);p2=archive(Path(d)/'v2.pkg',desc(second,'2.0'),[('objects/'+r['blob'],new if r['path']=='program' else conf) for r in second]);code,pkg=import_package(c,p1);code,s=snapshot(c);code,p=install_plan(c,s,pkg);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);(root/'demo/settings').write_text('personal')
   code,pkg=import_package(c,p2);self.assertEqual(code,0);code,s=snapshot(c);p=C.c_void_p();self.assertEqual(lib.apkg_plan_upgrade(c,s,pkg,C.byref(p),None),0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(file_hash(root/'demo/program'),second[0]['blob']);self.assertEqual((root/'demo/settings').read_text(),'personal');lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);p1.unlink();p2.unlink();old.unlink();new.unlink()
   current=(root/'.apkg/state').read_bytes();history=next(p for p in root.parent.glob(root.name+'.apkg-history.*') if (p/'decision').exists() and (p/'new-state').read_bytes()==current);code,p=op(c,'rollback',str(history));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(file_hash(root/'demo/program'),first[0]['blob']);lib.apkg_plan_free(p)
   (root/'demo/program').write_bytes(b'damaged');code,p=op(c,'repair');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(file_hash(root/'demo/program'),first[0]['blob']);lib.apkg_plan_free(p)
   for h in root.parent.glob(root.name+'.apkg-history.*'):self.assertLess((h/'plan').stat().st_size,50000)
   code,p=op(c,'remove');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertFalse((root/'demo/program').exists());self.assertEqual((root/'demo/settings').read_text(),'personal');lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_cli_and_actual_worker_consume_the_same_archive(self):
  with tempfile.TemporaryDirectory() as d:
   helper=clients.OperationClients();a,unused=helper.setup_root_package(d,'cli');b,unused=helper.setup_root_package(d,'worker');source=Path(d)/'payload';source.write_bytes(b'package');entry=blob('.hidden',source);data=desc([dict(path='Empty',type='directory'),entry]);pkg=archive(Path(d)/'signed package.pkg',data,[('objects/'+entry['blob'],source)]);helper.commit('install',a,pkg);helper.worker(b,pkg,0);helper.compare(a,b);self.assertEqual((a/'demo/.hidden').read_bytes(),(b/'demo/.hidden').read_bytes())
 def test_cancel_progress_is_owned_monotonic_and_cleans_private_temp(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);source=patterned(Path(d)/'payload',5*1024*1024);entry=blob('file',source);pkg=archive(Path(d)/'package',desc([entry]),[('objects/'+entry['blob'],source)]);before=(root/'.apkg/state').read_bytes();events=[]
   @EVENT_FN
   def event(user,ptr):
    v=ptr.contents;events.append((v.sequence,v.completed,v.total.value,bytes(v.operation_id.bytes)));return 1 if v.completed>=1024*1024 else 0
   observer=Observer(event,None,None);code,p=import_package(c,pkg,C.byref(observer));self.assertEqual(code,16);self.assertFalse(p);self.assertTrue(events);self.assertEqual([v[0] for v in events],list(range(1,len(events)+1)));self.assertEqual(len({v[3] for v in events}),1);self.assertEqual([v[1] for v in events],sorted(v[1] for v in events));self.assertTrue(all(v[2]==pkg.stat().st_size for v in events));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(cached(root,entry['blob']).exists());store=Path(str(root)+'.apkg-objects');self.assertFalse(list(store.glob('.incoming.*')));lib.apkg_close(c)
 def test_interrupted_import_retry_and_io_refusal_preserve_root(self):
  for point in (1001,1002,1003):
   for action in (1,2):
    with self.subTest(point=point,action=action),tempfile.TemporaryDirectory() as d:
     root=Path(d)/'root';c=init(root);source=Path(d)/'source';source.write_bytes(b'content');entry=blob('file',source);pkg=archive(Path(d)/'package',desc([entry]),[('objects/'+entry['blob'],source)]);before=(root/'.apkg/state').read_bytes();pid=os.fork()
     if pid==0:
      exit_code=120
      try:
       lib.apkg_test_fault(point,action);code,p=import_package(c,pkg);lib.apkg_package_free(p);exit_code=0 if code==6 else 121
      finally:os._exit(exit_code)
     _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99 if action==2 else 0);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'demo').exists());code,p=import_package(c,pkg);self.assertEqual(code,0);lib.apkg_package_free(p);lib.apkg_close(c)
 def test_blob_import_allocation_failures_are_typed_and_retryable(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);source=Path(d)/'source';source.write_bytes(b'content');entry=blob('file',source);pkg=archive(Path(d)/'package',desc([entry]),[('objects/'+entry['blob'],source)]);before=(root/'.apkg/state').read_bytes()
   for nth in range(1,101):
    with self.subTest(nth=nth):
     pid=os.fork()
     if pid==0:
      exit_code=120
      try:
       lib.apkg_test_fail_alloc(nth);code,p=import_package(c,pkg);lib.apkg_test_fail_alloc(0);lib.apkg_package_free(p);exit_code=0 if code in (0,4) else 100+code
      finally:os._exit(exit_code)
     _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),0);self.assertEqual((root/'.apkg/state').read_bytes(),before)
   code,p=import_package(c,pkg);self.assertEqual(code,0);lib.apkg_package_free(p);lib.apkg_close(c)
 def test_metadata_bounds_zero_length_shared_blobs_and_cpu_refusal(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);source=Path(d)/'empty';source.write_bytes(b'');a=blob('a',source);b=blob('b',source);pkg=archive(Path(d)/'empty.pkg',desc([a,b]),[('objects/'+a['blob'],source)]);code,p=import_package(c,pkg);self.assertEqual(code,0);lib.apkg_package_free(p)
   binary=bytearray(64);binary[:7]=b'\x7fELF\x02\x01\x01';binary[18:20]=(183).to_bytes(2,'little');source.write_bytes(binary);a=blob('wrong-cpu',source);pkg=archive(Path(d)/'wrong.pkg',desc([a]),[('objects/'+a['blob'],source)]);self.assertEqual(import_package(c,pkg)[0],13)
   pkg=Path(d)/'oversized';path=b'descriptor.json';pkg.write_bytes(b'PKG\1'+struct.pack('>I',8+9+len(path)+3*1024*1024)+struct.pack('>I',len(path))+path+b'\0'+struct.pack('>I',3*1024*1024)+b' '*(3*1024*1024));self.assertEqual(import_package(c,pkg)[0],3);lib.apkg_close(c)
 def test_cached_readonly_and_approved_source_independence(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);source=Path(d)/'source';source.write_bytes(b'contents');a=blob('file',source);data=desc([a]);pkg=archive(Path(d)/'archive',data,[('objects/'+a['blob'],source)]);code,read=open_ctx(root,0);self.assertEqual(import_package(read,pkg)[0],17);lib.apkg_close(read);self.assertFalse(Path(str(root)+'.apkg-objects').exists());code,p=package(c,data,sign(data));self.assertEqual(code,0);code,s=snapshot(c);self.assertEqual(install_plan(c,s,p)[0],5);lib.apkg_package_free(p);lib.apkg_snapshot_free(s)
   code,pkgobj=import_package(c,pkg);self.assertEqual(code,0);code,s=snapshot(c);code,p=install_plan(c,s,pkgobj);self.assertEqual(code,0);pkg.unlink();source.unlink();self.assertEqual(run_plan(c,p),(0,1));self.assertEqual((root/'demo/file').read_bytes(),b'contents');lib.apkg_plan_free(p);lib.apkg_package_free(pkgobj);lib.apkg_snapshot_free(s);lib.apkg_close(c)
 def test_pending_inline_journal_remains_recoverable_without_object_cache(self):
  from test_operations import install
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);install(c,descriptor());(root/'demo/hello').write_text('personal damage');code,p=op(c,'repair');code,a=approve(c,p);pid=os.fork()
   if pid==0:
    try:lib.apkg_test_fault(102,2);execute(c,p,a)
    finally:os._exit(120)
   _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99);txn=Path(str(root)+'.apkg-transaction');plan=json.loads((txn/'plan').read_bytes())
   for change in plan['body']['changes']:
    for kind in ('before','next'):
     if change[kind]:
      for entry in change[kind]['files']:
       if 'blob' in entry:entry['data']=cached(root,entry.pop('blob')).read_bytes().hex()
   raw=json.dumps(plan,sort_keys=True,separators=(',',':')).encode();(txn/'plan').write_bytes(raw);(txn/'plan-sha').write_text(hashlib.sha256(raw).hexdigest())
   for path in Path(str(root)+'.apkg-objects').iterdir():path.unlink()
   self.assertEqual(recover(c),(0,2));self.assertEqual((root/'demo/hello').read_text(),'personal damage');lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_corruption_after_approval_refuses_before_managed_publication(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);source=Path(d)/'source';source.write_bytes(b'content');entry=blob('file',source);pkg=archive(Path(d)/'archive',desc([entry]),[('objects/'+entry['blob'],source)]);code,package_ptr=import_package(c,pkg);self.assertEqual(code,0);code,s=snapshot(c);code,p=install_plan(c,s,package_ptr);self.assertEqual(code,0);code,a=approve(c,p);cached(root,entry['blob']).write_bytes(b'corrupt');self.assertEqual(execute(c,p,a),(10,0));self.assertFalse((root/'demo').exists());self.assertFalse(Path(str(root)+'.apkg-transaction').exists());lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_package_free(package_ptr);lib.apkg_snapshot_free(s);lib.apkg_close(c)
