# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
from contextlib import contextmanager
import hashlib
import http.server
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest
from support import *
import test_client as clients
from test_channel import CHANNEL,CP,CS,seed,selected
from test_file_channel import artifact,channel_key,publish,open_file,head
from test_operations import run_plan

lib.apkg_channel_open_http.argtypes=[C.c_void_p,Text,C.POINTER(ID),C.c_uint64,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_channel_open_http.restype=C.c_int
lib.apkg_channel_fetch.argtypes=[C.c_void_p,C.c_void_p,C.c_uint64,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_channel_fetch.restype=C.c_int
lib.apkg_cancel_create.argtypes=[C.POINTER(C.c_void_p)];lib.apkg_cancel_create.restype=C.c_int
lib.apkg_cancel_request.argtypes=[C.c_void_p];lib.apkg_cancel_free.argtypes=[C.c_void_p]

def http_open(c,url,floor=0,observer=None):
 rawurl=url.encode();buffer=C.create_string_buffer(rawurl);ident=ID();ident.bytes[:]=bytes.fromhex(CHANNEL);out=C.c_void_p();code=lib.apkg_channel_open_http(c,Text(C.cast(buffer,C.c_void_p),len(rawurl)),C.byref(ident),floor,observer,C.byref(out),None);return code,out

def fetch(c,ch,index=0,observer=None):
 out=C.c_void_p();code=lib.apkg_channel_fetch(c,ch,index,observer,C.byref(out),None);return code,out

@contextmanager
def serve(directory):
 class Handler(http.server.SimpleHTTPRequestHandler):
  protocol_version='HTTP/1.1'
  def __init__(self,*args,**kwargs):super().__init__(*args,directory=str(directory),**kwargs)
  def log_message(self,*args):pass
  def handle(self):
   try:super().handle()
   except (ConnectionResetError,BrokenPipeError):pass
  def do_GET(self):
   server=self.server;server.requests.append(self.path)
   mode=server.mode
   if mode=='oversized-current' and self.path.endswith('/current'):
    self.send_response(200);self.send_header('Content-Length','4096');self.end_headers()
    try:self.wfile.write(b'X'*4096)
    except OSError:pass
    return
   if mode=='redirect' and not self.path.startswith('/real/'):
    self.send_response(302);self.send_header('Location','/real'+self.path);self.send_header('Content-Length','0');self.end_headers();return
   if self.path.startswith('/real/'):self.path=self.path[5:]
   if self.path.startswith('/archives/') and mode in ('tamper','short','slow','missing'):
    if mode=='missing':self.send_error(404);return
    data=(directory/self.path.lstrip('/')).read_bytes()
    self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers()
    if mode=='tamper':data=data[:-1]+bytes([data[-1]^1])
    if mode=='short':data=data[:len(data)//2];self.close_connection=True
    try:
     if mode=='slow':
      for at in range(0,len(data),4096):self.wfile.write(data[at:at+4096]);self.wfile.flush();time.sleep(0.005)
     else:self.wfile.write(data)
    except OSError:pass
    return
   super().do_GET()
 server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);server.mode='normal';server.requests=[];thread=threading.Thread(target=server.serve_forever,kwargs=dict(poll_interval=0.02),daemon=True);thread.start()
 try:yield f'http://127.0.0.1:{server.server_port}',server
 finally:server.shutdown();server.server_close();thread.join(timeout=3)

def fixture(d,large=False):
 package=artifact(d)
 if large:
  from test_author import build
  source=d/'demo-drawer'
  with (source/'program').open('ab') as f:
   for _ in range(129):f.write(b'P'*65536)
  package=d/'large.pkg';result=build(source,package,d/'demo.author-key');assert result.returncode==0,result.stderr
 key=channel_key(d/'channel-key');endpoint=d/'channel';result=publish(package,endpoint,key);assert result.returncode==0,result.stderr;return package,endpoint,json.loads(result.stdout)

class HttpChannels(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0);lib.apkg_test_fail_alloc(0)
 def test_http_readonly_discovery_fetch_exact_artifact_and_install(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package,endpoint,published=fixture(d);root=d/'root';before=state(root,policy=seed());code,reader=open_ctx(root)
   with serve(endpoint) as (url,server):
    code,ch=http_open(reader,url,1);self.assertEqual(code,0);self.assertEqual(fetch(reader,ch)[0],17);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-archives').exists());lib.apkg_channel_free(ch);lib.apkg_close(reader);code,c=open_ctx(root,1);code,ch=http_open(c,url);self.assertEqual(code,0);code,obj=fetch(c,ch);self.assertEqual(code,0);retained=Path(str(root)+'.apkg-archives')/(published['archive_sha256']+'.pkg');self.assertEqual(retained.read_bytes(),package.read_bytes());self.assertEqual(retained.stat().st_mode&0o777,0o600);self.assertEqual((root/'.apkg/state').read_bytes(),before);code,s=snapshot(c);code,p=install_plan(c,s,obj);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual((root/'demo/demo-drawer/program').read_bytes(),b'body');lib.apkg_plan_free(p);lib.apkg_package_free(obj);lib.apkg_snapshot_free(s);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_transfer_tampering_truncation_and_missing_resource_refuse_without_commit(self):
  for mode,expected in (('tamper',10),('short',18),('missing',5)):
   with self.subTest(mode=mode),tempfile.TemporaryDirectory() as d:
    d=Path(d);package,endpoint,published=fixture(d);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1)
    with serve(endpoint) as (url,server):
     code,ch=http_open(c,url);self.assertEqual(code,0);server.mode=mode;code,obj=fetch(c,ch);self.assertEqual(code,expected);self.assertFalse(obj);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'demo').exists());self.assertFalse(list(Path(str(root)+'.apkg-archives').glob('.incoming.*')));self.assertFalse(list(Path(str(root)+'.apkg-archives').glob('*.pkg')));lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_bounded_metadata_redirects_and_invalid_urls(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package,endpoint,published=fixture(d);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root)
   for url in ('file:///tmp/channel','http://user:secret@host/channel','https://host/channel?secret=x','http://host/fragment#x'):
    self.assertEqual(http_open(c,url)[0],1)
   with serve(endpoint) as (url,server):
    server.mode='oversized-current';self.assertEqual(http_open(c,url)[0],3);server.mode='redirect';code,ch=http_open(c,url);self.assertEqual(code,0);lib.apkg_channel_free(ch);self.assertTrue(any(path.startswith('/real/') for path in server.requests));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-archives').exists());lib.apkg_close(c)
 def test_actual_cli_opens_and_prepares_http_without_managed_commit(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package,endpoint,published=fixture(d);root=d/'root';before=state(root,policy=seed())
   with serve(endpoint) as (url,server):
    opened=clients.cli('channel-open',root,url,CHANNEL,1);self.assertEqual(opened.returncode,0,opened.stdout+opened.stderr);self.assertEqual(json.loads(opened.stdout)['entries'][0]['name'],'demo');prepared=clients.cli('channel-fetch',root,url,CHANNEL,0,1);self.assertEqual(prepared.returncode,0,prepared.stdout+prepared.stderr);self.assertEqual(json.loads(prepared.stdout)['archive_sha256'],published['archive_sha256']);self.assertFalse(json.loads(prepared.stdout)['managed_commit']);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'demo').exists())
 def test_file_fetch_cache_reuse_and_corruption_refusal(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package,endpoint,published=fixture(d);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);self.assertEqual(code,0)
   code,obj=fetch(c,ch);self.assertEqual(code,0);lib.apkg_package_free(obj)
   cache=Path(str(root)+'.apkg-archives')/(published['archive_sha256']+'.pkg');self.assertEqual(cache.read_bytes(),package.read_bytes())
   (endpoint/'archives'/(published['archive_sha256']+'.pkg')).unlink()
   code,obj=fetch(c,ch);self.assertEqual(code,0);lib.apkg_package_free(obj)
   corrupt=cache.read_bytes()[:-1]+b'!';cache.write_bytes(corrupt);self.assertEqual(fetch(c,ch)[0],10);self.assertEqual(cache.read_bytes(),corrupt);self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_transfer_flush_and_publish_failures_are_retryable_preparation(self):
  for fault in (1101,1102,1103):
   with self.subTest(fault=fault),tempfile.TemporaryDirectory() as d:
    d=Path(d);package,endpoint,published=fixture(d);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);self.assertEqual(code,0)
    lib.apkg_test_fault(fault,1);self.assertEqual(fetch(c,ch)[0],6);lib.apkg_test_fault(0,0)
    cache=Path(str(root)+'.apkg-archives');self.assertFalse(list(cache.glob('.incoming.*')));self.assertEqual(bool(list(cache.glob('*.pkg'))),fault==1103);self.assertEqual((root/'.apkg/state').read_bytes(),before)
    code,obj=fetch(c,ch);self.assertEqual(code,0);self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_package_free(obj);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_progress_and_cancel_during_large_http_transfer(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package,endpoint,published=fixture(d,large=True);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);events=[]
   @EVENT_FN
   def metadata(user,event):
    e=event.contents;events.append((bytes(e.operation_id.bytes),e.sequence,e.completed,e.total.state));return 0
   observer=Observer(metadata,None,None)
   with serve(endpoint) as (url,server):
    code,ch=http_open(c,url,observer=C.byref(observer));self.assertEqual(code,0);self.assertGreater(len(events),0);self.assertEqual(len({e[0] for e in events}),1);self.assertEqual([e[2] for e in events],sorted(e[2] for e in events));self.assertEqual(len({e[3] for e in events}),1)
    events.clear()
    @EVENT_FN
    def cancel(user,event):
     e=event.contents;events.append((bytes(e.operation_id.bytes),e.sequence,e.completed,e.phase));return int(e.phase==1 and e.completed>=65536)
    observer=Observer(cancel,None,None);server.mode='slow';code,obj=fetch(c,ch,observer=C.byref(observer));self.assertEqual(code,16);self.assertFalse(obj);self.assertEqual(len({e[0] for e in events}),1);self.assertIn(0,[e[3] for e in events]);self.assertIn(1,[e[3] for e in events]);self.assertEqual([e[2] for e in events],sorted(e[2] for e in events));self.assertEqual((root/'.apkg/state').read_bytes(),before);cache=Path(str(root)+'.apkg-archives');self.assertFalse(list(cache.glob('.incoming.*')));self.assertFalse(list(cache.glob('*.pkg')))
    cancel_token=C.c_void_p();self.assertEqual(lib.apkg_cancel_create(C.byref(cancel_token)),0);lib.apkg_cancel_request(cancel_token);observer=Observer(EVENT_FN(),None,cancel_token);self.assertEqual(fetch(c,ch,observer=C.byref(observer))[0],16);lib.apkg_cancel_free(cancel_token);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_signed_reference_to_different_valid_archive_is_refused(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);package,endpoint,published=fixture(d);other=artifact(d,'other');other_data=other.read_bytes();other_sha=hashlib.sha256(other_data).hexdigest();(endpoint/'archives'/(other_sha+'.pkg')).write_bytes(other_data)
   original=json.loads((endpoint/'snapshots'/head(endpoint)/'channel.json').read_bytes());original['body']['entries'][0]['archive']=dict(sha256=other_sha,size=str(len(other_data)));data=json.dumps(original,sort_keys=True,separators=(',',':')).encode();digest=hashlib.sha256(data).hexdigest();folder=endpoint/'snapshots'/digest;folder.mkdir();(folder/'channel.json').write_bytes(data);(folder/'channel.signature.json').write_bytes(sign(data,CP,CS,'channel'));(endpoint/'current').write_text(digest+'\n')
   root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);self.assertEqual(code,0);self.assertEqual(fetch(c,ch)[0],10);self.assertFalse(list(Path(str(root)+'.apkg-archives').glob('*.pkg')));self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_channel_free(ch);lib.apkg_close(c)
