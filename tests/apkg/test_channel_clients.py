# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from support import *
import test_client as clients
from test_author import author_fixture, key_fixture, build
from test_channel import CHANNEL, seed
from test_file_channel import channel_key, publish, open_file, head, snapshot_document
from test_http_channel import serve, http_open
from test_operations import run_plan, op
from test_payload import import_package

lib.apkg_plan_channel_install.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.POINTER(Digest),C.c_int,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_plan_channel_install.restype=C.c_int
CONFIRM=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_char_p)
lib.apkg_client_channel_apply.argtypes=[C.c_char_p,C.c_char_p,C.c_char_p,C.c_int,CONFIRM,C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_client_channel_apply.restype=C.c_int

def published(d,conflict=False,depends=True):
 key=key_fixture(d/'author-key');ckey=channel_key(d/'channel-key');endpoint=d/'channel';archives={}
 for name,version in (('provider','1.2'),('provider','2.0'),('app','1.2'),('unused','1.2')):
  source=d/(name+'-'+version+'-drawer');manifest=author_fixture(source,name=name,version=version,depends=['provider >= 1.1'] if name=='app' and depends else []);(source/'program').write_bytes((name+version).encode())
  if conflict and name=='provider' and version=='2.0':manifest.write_text(manifest.read_text()+'conflicts = ["app"]\n')
  archive=d/(name+'-'+version+'.pkg');result=build(source,archive,key);assert result.returncode==0,result.stderr;result=publish(archive,endpoint,ckey);assert result.returncode==0,result.stderr;archives[name,version]=archive
 return endpoint,archives

def row(endpoint,name,version='1.2'):
 for item in snapshot_document(endpoint)['body']['entries']:
  body=item['descriptor']['body']
  if body['name']==name and body['version']==version:return item
 raise AssertionError('missing row')

def digest(endpoint,name='app',version='1.2'):
 return hashlib.sha256(json.dumps(row(endpoint,name,version)['descriptor'],sort_keys=True,separators=(',',':')).encode()).hexdigest()

def selection(endpoint,name='app',version='1.2'):return CHANNEL+'/'+head(endpoint)+'/'+digest(endpoint,name,version)

def plan(c,ch,sha,recommendations=0,observer=None):
 code,s=snapshot(c);assert code==0;wanted=Digest();wanted.bytes[:]=bytes.fromhex(sha);out=C.c_void_p();code=lib.apkg_plan_channel_install(c,s,ch,C.byref(wanted),recommendations,observer,C.byref(out),None);lib.apkg_snapshot_free(s);return code,out

def apply(root,source,selected,confirm=None,observer=None):
 confirm=confirm or CONFIRM(lambda user,p,text:1);out=C.c_void_p();code=lib.apkg_client_channel_apply(str(root).encode(),str(source).encode(),selected.encode(),0,confirm,None,observer,C.byref(out),None);outcome=lib.apkg_result_get(out).contents.outcome if out else None;lib.apkg_result_free(out);return code,outcome

def install_archive(c,path):
 code,obj=import_package(c,path);assert code==0,code;code,s=snapshot(c);code,p=install_plan(c,s,obj);assert code==0,code;result=run_plan(c,p);lib.apkg_plan_free(p);lib.apkg_snapshot_free(s);lib.apkg_package_free(obj);assert result==(0,1),result

def committed(root):return json.loads((root/'.apkg/state').read_bytes())['body']

class ChannelClients(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0);lib.apkg_test_fail_alloc(0)
 def test_metadata_resolution_fetches_only_selected_versions_and_commits_floor_together(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d,conflict=True);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1)
   for name,version in (('provider','2.0'),('unused','1.2')):(endpoint/'archives'/(row(endpoint,name,version)['archive']['sha256']+'.pkg')).unlink()
   with serve(endpoint) as (url,server):
    code,ch=http_open(c,url);self.assertEqual(code,0);code,p=plan(c,ch,digest(endpoint));self.assertEqual(code,0);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'app').exists());selected_archives={path.split('/')[-1] for path in server.requests if path.startswith('/archives/')};self.assertEqual(selected_archives,{row(endpoint,'app')['archive']['sha256']+'.pkg',row(endpoint,'provider')['archive']['sha256']+'.pkg'});body=json.loads(raw(lib.apkg_plan_get(p).contents.canonical))['body'];self.assertEqual(body['new_state']['body']['channel_heads'][0]['sequence'],'4');self.assertEqual(len(body['origins']),1);self.assertEqual(run_plan(c,p),(0,1));actual=committed(root);self.assertEqual(actual['generation'],'2');self.assertEqual(actual['package_generation'],'2');self.assertEqual([(x['descriptor']['body']['name'],x['descriptor']['body']['version'],x['reason']) for x in actual['installed']],[('app','1.2','explicit'),('provider','1.2','dependency')]);lib.apkg_plan_free(p);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_incomparable_dependency_candidates_refuse_instead_of_string_order(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);key=key_fixture(d/'author-key');ckey=channel_key(d/'channel-key');endpoint=d/'channel'
   for name,version,depends in (('provider','2.0-test1',[]),('provider','2.0-preview1',[]),('app','1.2',['provider >= 1.1'])):
    source=d/(name+'-'+version+'-drawer');author_fixture(source,name=name,version=version,depends=depends);(source/'program').write_bytes((name+version).encode())
    archive=d/(name+'-'+version+'.pkg');result=build(source,archive,key);self.assertEqual(result.returncode,0,result.stderr);result=publish(archive,endpoint,ckey);self.assertEqual(result.returncode,0,result.stderr)
   root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);self.assertEqual(code,0)
   code,p=plan(c,ch,digest(endpoint));self.assertEqual(code,14);self.assertFalse(p);self.assertEqual((root/'.apkg/state').read_bytes(),before)
   code,p=plan(c,ch,digest(endpoint,'provider','2.0-test1'));self.assertEqual(code,0);lib.apkg_plan_free(p)
   lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_installed_compatible_provider_is_reused_without_archive_fetch(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);root=d/'root';state(root,policy=seed());code,c=open_ctx(root,1);install_archive(c,archives['provider','1.2'])
   for version in ('1.2','2.0'):(endpoint/'archives'/(row(endpoint,'provider',version)['archive']['sha256']+'.pkg')).unlink()
   with serve(endpoint) as (url,server):
    code,ch=http_open(c,url);code,p=plan(c,ch,digest(endpoint));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual([p for p in server.requests if p.startswith('/archives/')],['/archives/'+row(endpoint,'app')['archive']['sha256']+'.pkg']);provider=next(x for x in committed(root)['installed'] if x['descriptor']['body']['name']=='provider');self.assertEqual(provider['descriptor']['body']['version'],'1.2');self.assertEqual(provider['reason'],'explicit');lib.apkg_plan_free(p);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_cli_and_actual_worker_discover_identical_facts_and_install_same_packages(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);left=d/'cli';right=d/'worker';before_left=state(left,policy=seed());before_right=state(right,policy=seed());selected=selection(endpoint)
   with serve(endpoint) as (url,server):
    query=clients.cli('channel-open',left,url,CHANNEL);self.assertEqual(query.returncode,0,query.stderr);worker=subprocess.run([str(clients.WORKER),str(right),url,'8','7',CHANNEL],capture_output=True,text=True);self.assertEqual(worker.returncode,0,worker.stdout+worker.stderr);self.assertEqual(json.loads(query.stdout),json.loads(worker.stdout));self.assertEqual((left/'.apkg/state').read_bytes(),before_left);self.assertEqual((right/'.apkg/state').read_bytes(),before_right);preview=clients.cli('channel-install',left,url,*selected.split('/'));self.assertEqual(preview.returncode,16,preview.stdout+preview.stderr);self.assertIn('Record replay floor at commit: 4',preview.stdout);self.assertIn('Channel signing key:',preview.stdout);approved=re.search(r'Approved plan: ([a-f0-9]{64})',preview.stdout).group(1);done=clients.cli('channel-install',left,url,*selected.split('/'),'--confirm',approved);self.assertEqual(done.returncode,0,done.stdout+done.stderr);worker=subprocess.run([str(clients.WORKER),str(right),url,'1','7',selected],capture_output=True,text=True);self.assertEqual(worker.returncode,0,worker.stdout+worker.stderr);self.assertEqual(committed(left),committed(right));self.assertEqual(next(x['descriptor']['body']['version'] for x in committed(left)['installed'] if x['descriptor']['body']['name']=='provider'),'2.0');self.assertEqual((left/'app/app-1.2-drawer/program').read_bytes(),b'app1.2');self.assertEqual((right/'app/app-1.2-drawer/program').read_bytes(),b'app1.2')
 def test_decline_preserves_state_and_floor_but_can_leave_verified_preparation(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);root=d/'root';before=state(root,policy=seed());seen=[]
   @CONFIRM
   def decline(user,p,text):seen.append(C.string_at(text));return 0
   self.assertEqual(apply(root,endpoint,selection(endpoint),decline),(16,None));self.assertEqual(len(seen),1);self.assertIn(b'qualification: no independent',seen[0]);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'app').exists());self.assertEqual(len(list(Path(str(root)+'.apkg-archives').glob('*.pkg'))),2)
 def test_changed_discovery_snapshot_refuses_without_preparing_archives(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);root=d/'root';before=state(root,policy=seed());old=selection(endpoint);extra=d/'extra';extra.mkdir();source=extra/'drawer';author_fixture(source,name='extra');pkg=extra/'extra.pkg';result=build(source,pkg,d/'author-key');self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(publish(pkg,endpoint,d/'channel-key').returncode,0);self.assertEqual(apply(root,endpoint,old),(8,None));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-archives').exists())
 def test_source_change_after_confirmation_and_cancellation_never_commit_floor(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);root=d/'root';before=state(root,policy=seed());reference=(endpoint/'current').read_bytes()
   @CONFIRM
   def change(user,p,text):(endpoint/'current').write_text('00'*32+'\n');return 1
   self.assertEqual(apply(root,endpoint,selection(endpoint),change),(8,0));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'app').exists());(endpoint/'current').write_bytes(reference)
   @EVENT_FN
   def cancel(user,event):return int(event.contents.phase==1)
   observer=Observer(cancel,None,None);self.assertEqual(apply(root,endpoint,selection(endpoint),observer=C.byref(observer)),(16,None));self.assertEqual((root/'.apkg/state').read_bytes(),before)
 def test_selected_corrupt_archive_refuses_without_install_or_floor_change(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);root=d/'root';before=state(root,policy=seed());pkg=endpoint/'archives'/(row(endpoint,'provider','2.0')['archive']['sha256']+'.pkg');data=pkg.read_bytes();pkg.write_bytes(data[:-1]+bytes([data[-1]^1]));self.assertEqual(apply(root,endpoint,selection(endpoint)),(10,None));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'app').exists())
 def test_shared_worker_decline_cancel_and_allocation_failures_keep_root(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);selected=selection(endpoint)
   with serve(endpoint) as (url,server):
    for choice in (0,2,3,4,6):
     root=d/('root'+str(choice));before=state(root,policy=seed());result=subprocess.run([str(clients.WORKER),str(root),url,str(choice),'7',selected],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'app').exists())
 def test_write_requirement_invalid_selection_and_unknown_digest_are_explicit(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root);code,ch=open_file(c,endpoint);self.assertEqual(plan(c,ch,digest(endpoint))[0],17);lib.apkg_channel_free(ch);lib.apkg_close(c);self.assertFalse(Path(str(root)+'.apkg-archives').exists());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);self.assertEqual(plan(c,ch,'00'*32)[0],5);lib.apkg_channel_free(ch);lib.apkg_close(c)
   for selected in ('',CHANNEL+'/'+digest(endpoint),selection(endpoint).upper(),selection(endpoint)+'extra'):self.assertEqual(apply(root,endpoint,selected),(1,None))
   self.assertEqual((root/'.apkg/state').read_bytes(),before)
 def test_offline_repair_remove_and_stale_history_after_channel_install(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);root=d/'root';state(root,policy=seed());self.assertEqual(apply(root,endpoint,selection(endpoint)),(0,1));history=next(p for p in root.parent.glob(root.name+'.apkg-history.*') if (p/'decision').exists() and (p/'new-state').read_bytes()==(root/'.apkg/state').read_bytes());floor=committed(root)['channel_heads'];shutil.rmtree(endpoint);code,c=open_ctx(root,1);(root/'app/app-1.2-drawer/program').write_bytes(b'damaged');code,p=op(c,'repair','app');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);self.assertEqual((root/'app/app-1.2-drawer/program').read_bytes(),b'app1.2');self.assertEqual(committed(root)['channel_heads'],floor);self.assertEqual(op(c,'rollback',str(history))[0],8);code,p=op(c,'remove','app');self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);self.assertEqual(committed(root)['channel_heads'],floor);lib.apkg_close(c)
 def test_metadata_dependency_failure_performs_no_archive_transfer(self):
  from test_channel_heads import rewrite
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);document=snapshot_document(endpoint);document['body']['entries']=[entry for entry in document['body']['entries'] if entry['descriptor']['body']['name']!='provider'];document['body']['sequence']='5';rewrite(endpoint,json.dumps(document,sort_keys=True,separators=(',',':')).encode());root=d/'root';before=state(root,policy=seed())
   with serve(endpoint) as (url,server):self.assertEqual(apply(root,url,selection(endpoint)),(15,None));self.assertFalse(any(path.startswith('/archives/') for path in server.requests));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-archives').exists())
 def test_unknown_author_and_wrong_target_refuse_without_archive_preparation(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d)
   for name,policy,target,expected in (('untrusted',seed(authors=False),TARGET,12),('target',seed(),'i386-aros-v0-native',13)):
    root=d/name;before=state(root,policy=policy,target=target);self.assertEqual(apply(root,endpoint,selection(endpoint)),(expected,None));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-archives').exists())
 def test_update_preserves_configuration_and_rolls_back_offline_with_floor_intact(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);root=d/'root';state(root,policy=seed());self.assertEqual(apply(root,endpoint,selection(endpoint)),(0,1));config=root/'app/app-1.2-drawer/.hidden';config.write_bytes(b'personal config');source=d/'app-1.2-drawer';manifest=source.with_name(source.name+'.arospkg.toml');manifest.write_text(manifest.read_text().replace('version = "1.2"','version = "3.0"'));(source/'program').write_bytes(b'app3.0');(source/'.hidden').write_bytes(b'new default');new=d/'app-new.pkg';result=build(source,new,d/'author-key');self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(publish(new,endpoint,d/'channel-key').returncode,0);self.assertEqual(apply(root,endpoint,selection(endpoint,'app','3.0')),(0,1));self.assertEqual(config.read_bytes(),b'personal config');self.assertEqual((root/'app/app-1.2-drawer/program').read_bytes(),b'app3.0');after=(root/'.apkg/state').read_bytes();history=next(p for p in root.parent.glob(root.name+'.apkg-history.*') if (p/'decision').exists() and (p/'new-state').read_bytes()==after);floor=committed(root)['channel_heads'];shutil.rmtree(endpoint);code,c=open_ctx(root,1);code,p=op(c,'rollback',str(history));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);self.assertEqual(config.read_bytes(),b'personal config');self.assertEqual((root/'app/app-1.2-drawer/program').read_bytes(),b'app1.2');self.assertEqual(committed(root)['channel_heads'],floor);lib.apkg_close(c)
 def test_planner_allocation_failures_are_typed_and_never_commit(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);endpoint,archives=published(d);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);self.assertEqual(code,0);code,s=snapshot(c);wanted=Digest();wanted.bytes[:]=bytes.fromhex(digest(endpoint));failures=0
   for point in range(1,501):
    out=C.c_void_p();lib.apkg_test_fail_alloc(point);code=lib.apkg_plan_channel_install(c,s,ch,C.byref(wanted),0,None,C.byref(out),None);lib.apkg_test_fail_alloc(0);self.assertIn(code,(0,4),point)
    if code:failures+=1;self.assertFalse(out)
    else:lib.apkg_plan_free(out)
    self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'app').exists())
   self.assertGreater(failures,0);lib.apkg_snapshot_free(s);code,p=plan(c,ch,digest(endpoint));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_channel_free(ch);lib.apkg_close(c)
