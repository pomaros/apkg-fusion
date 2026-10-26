# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from support import *
from test_author import author_fixture,key_fixture,build
import test_client as clients
from test_channel import CHANNEL,CP,seed,ChannelRow,selected

PUBLISH=ROOT/'tools/apkg-channel'
lib.apkg_channel_open_file.argtypes=[C.c_void_p,Text,C.POINTER(ID),C.c_uint64,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_channel_open_file.restype=C.c_int

def channel_key(path):
 path.write_text('Pkg-Secret-Key: 1\nSeed: '+bytes(reversed(range(32))).hex()+'\nPublic: '+CP.hex()+'\n');path.chmod(0o600);return path

def artifact(d,name='demo'):
 source=d/(name+'-drawer');author_fixture(source,name=name);key=key_fixture(d/(name+'.author-key'));path=d/(name+'.pkg');result=build(source,path,key);assert result.returncode==0,result.stderr;return path

def command(archive,endpoint,key,identity=CHANNEL):
 return [sys.executable,str(PUBLISH),str(archive),'--directory',str(endpoint),'--id',identity,'--sign',str(key),'--engine',str(clients.CLI)]
def publish(archive,endpoint,key,identity=CHANNEL):return subprocess.run(command(archive,endpoint,key,identity),capture_output=True,text=True)
def head(endpoint):return (endpoint/'current').read_text().strip()
def snapshot_document(endpoint):return json.loads((endpoint/'snapshots'/head(endpoint)/'channel.json').read_bytes())
def open_file(c,endpoint,floor=0):
 path=os.fsencode(endpoint);buf=C.create_string_buffer(path);identity=ID();identity.bytes[:]=bytes.fromhex(CHANNEL);out=C.c_void_p();code=lib.apkg_channel_open_file(c,Text(C.cast(buf,C.c_void_p),len(path)),C.byref(identity),floor,C.byref(out),None);return code,out

class FileChannels(unittest.TestCase):
 def test_real_publication_context_discovery_and_explicit_archive_install(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package=artifact(d);key=channel_key(d/'channel-key');endpoint=d/'channel';first=publish(package,endpoint,key);self.assertEqual(first.returncode,0,first.stderr);answer=json.loads(first.stdout);self.assertTrue(answer['changed']);self.assertEqual(answer['sequence'],'1');self.assertEqual(head(endpoint),answer['snapshot']);self.assertEqual((endpoint/'archives'/(answer['archive_sha256']+'.pkg')).read_bytes(),package.read_bytes());root=d/'root';before=state(root,policy=seed());inspected=clients.cli('channel-open',root,endpoint,CHANNEL,1);self.assertEqual(inspected.returncode,0,inspected.stderr);view=json.loads(inspected.stdout);self.assertEqual(view['entries'][0]['name'],'demo');self.assertEqual(view['entries'][0]['archive_sha256'],answer['archive_sha256']);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-objects').exists());code,c=open_ctx(root);code,ch=open_file(c,endpoint,1);self.assertEqual(code,0);code,p=selected(c,ch);self.assertEqual(code,0);lib.apkg_package_free(p);lib.apkg_channel_free(ch);lib.apkg_close(c);runner=Path(os.environ.get('APKG_CHANNEL_MEMORY',ROOT/'build/test_channel_memory'));owned=subprocess.run([str(runner),str(root),str(endpoint),CHANNEL],capture_output=True,text=True);self.assertEqual(owned.returncode,0,owned.stdout+owned.stderr);helper=clients.OperationClients();helper.commit('install',root,endpoint/'archives'/(answer['archive_sha256']+'.pkg'));self.assertEqual((root/'demo/demo-drawer/program').read_bytes(),b'body')
 def test_idempotent_retry_conflicts_identity_and_corrupt_retention_refuse(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package=artifact(d);key=channel_key(d/'channel-key');endpoint=d/'channel';first=publish(package,endpoint,key);self.assertEqual(first.returncode,0,first.stderr);reference=(endpoint/'current').read_bytes();retry=publish(package,endpoint,key);self.assertEqual(retry.returncode,0,retry.stderr);self.assertFalse(json.loads(retry.stdout)['changed']);self.assertEqual((endpoint/'current').read_bytes(),reference);self.assertNotEqual(publish(package,endpoint,key,'ef'*16).returncode,0);self.assertEqual((endpoint/'current').read_bytes(),reference)
   source=d/'demo-drawer';(source/'program').write_bytes(b'changed without version/revision');conflict=d/'conflict.pkg';result=build(source,conflict,d/'demo.author-key');self.assertEqual(result.returncode,0,result.stderr);self.assertNotEqual(publish(conflict,endpoint,key).returncode,0);self.assertEqual((endpoint/'current').read_bytes(),reference);stored=next((endpoint/'archives').glob('*.pkg'));stored.write_bytes(b'corrupt');self.assertNotEqual(publish(package,endpoint,key).returncode,0);self.assertEqual(stored.read_bytes(),b'corrupt');self.assertEqual((endpoint/'current').read_bytes(),reference)
 def test_invalid_payload_and_key_location_do_not_create_channel(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package=artifact(d);raw=package.read_bytes();package.write_bytes(raw[:-1]+bytes([raw[-1]^1]));key=channel_key(d/'channel-key');endpoint=d/'channel';result=publish(package,endpoint,key);self.assertNotEqual(result.returncode,0);self.assertFalse(endpoint.exists());package.write_bytes(raw);endpoint.mkdir();inside=channel_key(endpoint/'secret-key');result=publish(package,endpoint,inside);self.assertNotEqual(result.returncode,0);self.assertEqual(list(endpoint.iterdir()),[inside])
 def test_two_publishers_serialize_without_losing_entries(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);a=artifact(d,'alpha');b=artifact(d,'beta');key=channel_key(d/'channel-key');endpoint=d/'channel';processes=[subprocess.Popen(command(pkg,endpoint,key),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) for pkg in (a,b)]
   for process in processes:
    stdout,stderr=process.communicate(timeout=90);self.assertEqual(process.returncode,0,stdout+stderr)
   catalog=snapshot_document(endpoint);self.assertEqual(catalog['body']['sequence'],'2');self.assertEqual({e['descriptor']['body']['name'] for e in catalog['body']['entries']},{'alpha','beta'});self.assertEqual(len(list((endpoint/'archives').glob('*.pkg'))),2)
 def test_process_death_at_publication_boundaries_preserves_a_valid_head_and_retry(self):
  loaded=runpy.run_path(str(PUBLISH),run_name='publication_fault_fixture');implementation=loaded['publish'];g=implementation.__globals__
  for boundary in ('archive','snapshot','before-head','after-head'):
   with self.subTest(boundary=boundary),tempfile.TemporaryDirectory() as d:
    d=Path(d);a=artifact(d,'alpha');b=artifact(d,'beta');key=channel_key(d/'channel-key');endpoint=d/'channel';self.assertEqual(publish(a,endpoint,key).returncode,0);old=head(endpoint);args=SimpleNamespace(archive=b,directory=endpoint,sign=key,id=CHANNEL,engine=clients.CLI);pid=os.fork()
    if pid==0:
     try:
      if boundary=='archive':
       original=g['retain_archive']
       def retain(*args):original(*args);os._exit(99)
       g['retain_archive']=retain
      elif boundary=='snapshot':
       original=g['immutable_bytes']
       def retain(*args):original(*args);os._exit(99)
       g['immutable_bytes']=retain
      elif boundary=='before-head':g['replace_current']=lambda *args:os._exit(99)
      else:
       original=g['replace_current']
       def replace(*args):original(*args);os._exit(99)
       g['replace_current']=replace
      with patch.object(tempfile,'tempdir',str(d)):implementation(args)
     finally:os._exit(98)
    _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99);root=d/'root';state(root,policy=seed());opened=clients.cli('channel-open',root,endpoint,CHANNEL);self.assertEqual(opened.returncode,0,opened.stdout+opened.stderr);self.assertEqual(json.loads(opened.stdout)['sequence'],'2' if boundary=='after-head' else '1')
    if boundary!='after-head':self.assertEqual(head(endpoint),old)
    retried=publish(b,endpoint,key);self.assertEqual(retried.returncode,0,retried.stderr);self.assertEqual(snapshot_document(endpoint)['body']['sequence'],'2');self.assertEqual(len(snapshot_document(endpoint)['body']['entries']),2)
 def test_changed_reference_stales_an_open_handle_and_floor_refuses_replay(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);a=artifact(d,'alpha');b=artifact(d,'beta');key=channel_key(d/'channel-key');endpoint=d/'channel';self.assertEqual(publish(a,endpoint,key).returncode,0);old=(endpoint/'current').read_bytes();root=d/'root';state(root,policy=seed());code,c=open_ctx(root);code,ch=open_file(c,endpoint);self.assertEqual(code,0);self.assertEqual(publish(b,endpoint,key).returncode,0);self.assertEqual(selected(c,ch)[0],8);lib.apkg_channel_free(ch);(endpoint/'current').write_bytes(old);self.assertEqual(open_file(c,endpoint,2)[0],8);lib.apkg_close(c)
 def test_readonly_open_allocation_failures_preserve_endpoint_and_root(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package=artifact(d);key=channel_key(d/'channel-key');endpoint=d/'channel';self.assertEqual(publish(package,endpoint,key).returncode,0);reference=(endpoint/'current').read_bytes();root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root)
   for nth in range(1,281):
    with self.subTest(nth=nth):
     lib.apkg_test_fail_alloc(nth)
     try:code,ch=open_file(c,endpoint)
     finally:lib.apkg_test_fail_alloc(0)
     self.assertIn(code,(0,4));self.assertEqual(bool(ch),code==0);lib.apkg_channel_free(ch);self.assertEqual((endpoint/'current').read_bytes(),reference);self.assertEqual((root/'.apkg/state').read_bytes(),before)
   lib.apkg_close(c)

 def test_one_toml_command_publishes_openly_through_common_helpers(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'Drawer';manifest=author_fixture(source);before=manifest.read_bytes();author=key_fixture(d/'author-key');key=channel_key(d/'channel-key');endpoint=d/'channel';result=subprocess.run([sys.executable,str(ROOT/'tools/apkg-publish'),str(source),'--common','--channel',str(endpoint),'--sign',str(author),'--authority',AUTH,'--channel-id',CHANNEL,'--channel-sign',str(key),'--runtime','native','--engine',str(clients.CLI)],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stderr);answer=json.loads(result.stdout);self.assertEqual(answer['publication_mode'],'open');self.assertEqual(answer['review'],'not-asserted');self.assertEqual(answer['author_public_key'],PK.hex());retried=subprocess.run(result.args,capture_output=True,text=True);self.assertEqual(retried.returncode,0,retried.stderr);self.assertFalse(json.loads(retried.stdout)['changed']);self.assertEqual(json.loads(retried.stdout)['snapshot'],answer['snapshot']);self.assertEqual(manifest.read_bytes(),before);self.assertFalse((source/'application.json').exists());root=d/'root';state(root,policy=seed());opened=clients.cli('channel-open',root,endpoint,CHANNEL);self.assertEqual(opened.returncode,0,opened.stderr);self.assertEqual(json.loads(opened.stdout)['entries'][0]['name'],'demo')
 def test_common_pipeline_refuses_overlapping_source_channel_and_both_secret_keys(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'Drawer';manifest=author_fixture(source);before=manifest.read_bytes();author=key_fixture(d/'author-key');key=channel_key(d/'channel-key');endpoint=d/'channel';inside=channel_key(source/'channel-secret')
   for directory,author_key,channel_sign in ((source/'public',author,key),(d,author,key),(endpoint,author,inside),(endpoint,inside,key)):
    result=subprocess.run([sys.executable,str(ROOT/'tools/apkg-publish'),str(source),'--common','--channel',str(directory),'--sign',str(author_key),'--authority',AUTH,'--channel-id',CHANNEL,'--channel-sign',str(channel_sign),'--runtime','native','--engine',str(clients.CLI)],capture_output=True,text=True);self.assertNotEqual(result.returncode,0);self.assertEqual(manifest.read_bytes(),before);self.assertFalse((source/'public').exists());self.assertFalse(endpoint.exists())
 def test_io_failures_before_commit_keep_old_head_and_post_commit_failure_is_retryable(self):
  loaded=runpy.run_path(str(PUBLISH),run_name='publication_io_fixture');implementation=loaded['publish'];g=implementation.__globals__
  for boundary in ('archive','metadata','final-flush'):
   with self.subTest(boundary=boundary),tempfile.TemporaryDirectory() as d:
    d=Path(d);a=artifact(d,'alpha');b=artifact(d,'beta');key=channel_key(d/'channel-key');endpoint=d/'channel';self.assertEqual(publish(a,endpoint,key).returncode,0);old=head(endpoint);args=SimpleNamespace(archive=b,directory=endpoint,sign=key,id=CHANNEL,engine=clients.CLI)
    if boundary in ('archive','metadata'):
     name='retain_archive' if boundary=='archive' else 'immutable_bytes'
     with patch.dict(g,{name:lambda *args:(_ for _ in ()).throw(OSError('injected write failure'))}):
      with self.assertRaises(OSError):implementation(args)
     self.assertEqual(head(endpoint),old)
    else:
     flush=g['flush_dir']
     def fail_final(path):
      if Path(path).resolve()==endpoint.resolve() and head(endpoint)!=old:raise OSError('injected final flush failure')
      return flush(path)
     with patch.dict(g,{'flush_dir':fail_final}):
      with self.assertRaises(loaded['Refusal']):implementation(args)
     self.assertNotEqual(head(endpoint),old)
    root=d/'root';state(root,policy=seed());self.assertEqual(clients.cli('channel-open',root,endpoint,CHANNEL).returncode,0);retried=publish(b,endpoint,key);self.assertEqual(retried.returncode,0,retried.stderr);self.assertEqual(snapshot_document(endpoint)['body']['sequence'],'2')

 def test_nonregular_archive_and_current_reference_refuse_without_blocking(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package=d/'pipe.pkg';os.mkfifo(package);key=channel_key(d/'channel-key');endpoint=d/'channel';result=subprocess.run(command(package,endpoint,key),capture_output=True,text=True,timeout=5);self.assertNotEqual(result.returncode,0);self.assertFalse(endpoint.exists());endpoint.mkdir();os.mkfifo(endpoint/'current');root=d/'root';state(root,policy=seed());result=subprocess.run([str(clients.CLI),'channel-open',str(root),str(endpoint),CHANNEL],capture_output=True,text=True,timeout=5);self.assertEqual(result.returncode,6,result.stdout+result.stderr)
 def test_large_archive_publication_keeps_catalog_and_managed_metadata_small(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'Drawer';author_fixture(source);author=key_fixture(d/'author-key');program=source/'program'
   with program.open('ab') as stream:
    for _ in range(513):stream.write(b'P'*65536)
   package=d/'large.pkg';built=build(source,package,author);self.assertEqual(built.returncode,0,built.stderr);key=channel_key(d/'channel-key');endpoint=d/'channel';published=publish(package,endpoint,key);self.assertEqual(published.returncode,0,published.stderr);answer=json.loads(published.stdout);stored=endpoint/'archives'/(answer['archive_sha256']+'.pkg');self.assertGreater(stored.stat().st_size,32*1024*1024)
   with stored.open('rb') as stream:self.assertEqual(hashlib.file_digest(stream,'sha256').hexdigest(),answer['archive_sha256'])
   self.assertLess((endpoint/'snapshots'/head(endpoint)/'channel.json').stat().st_size,10000);root=d/'root';state(root,policy=seed());opened=clients.cli('channel-open',root,endpoint,CHANNEL);self.assertEqual(opened.returncode,0,opened.stderr);helper=clients.OperationClients();helper.commit('install',root,stored);self.assertLess((root/'.apkg/state').stat().st_size,10000)
   with program.open('rb') as before,(root/'demo/Drawer/program').open('rb') as after:self.assertEqual(hashlib.file_digest(before,'sha256').digest(),hashlib.file_digest(after,'sha256').digest())

 def test_publisher_does_not_overwrite_a_changed_reference(self):
  loaded=runpy.run_path(str(PUBLISH),run_name='publication_stale_fixture');implementation=loaded['publish'];g=implementation.__globals__
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);a=artifact(d,'alpha');b=artifact(d,'beta');c=artifact(d,'gamma');key=channel_key(d/'channel-key');endpoint=d/'channel';self.assertEqual(publish(a,endpoint,key).returncode,0);other=(endpoint/'current').read_bytes();self.assertEqual(publish(c,endpoint,key).returncode,0);before=(endpoint/'current').read_bytes();args=SimpleNamespace(archive=b,directory=endpoint,sign=key,id=CHANNEL,engine=clients.CLI);replace=g['replace_current']
   def changed(directory,expected,digest):
    (directory/'current').write_bytes(other)
    return replace(directory,expected,digest)
   with patch.dict(g,{'replace_current':changed}):
    with self.assertRaises(loaded['Refusal']):implementation(args)
   self.assertNotEqual(before,other);self.assertEqual((endpoint/'current').read_bytes(),other);self.assertEqual(len(list((endpoint/'archives').glob('*.pkg'))),3)
