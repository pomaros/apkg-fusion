# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import hashlib
import json
from pathlib import Path
import shutil
import os
import subprocess
import tempfile
import unittest
from support import *
from test_channel import CHANNEL, seed, selected, ChannelRow
from test_file_channel import artifact, channel_key, publish, open_file, head
from test_http_channel import fixture, http_open, fetch, serve
from test_operations import run_plan, op
from test_author import author_fixture, key_fixture, build
import test_relations


def choose(c, channel, name, version=None):
 for index in range(lib.apkg_channel_get(channel).contents.count):
  row=C.POINTER(ChannelRow)();assert lib.apkg_channel_entry_at(channel,index,C.byref(row),None)==0
  if raw(row.contents.name).decode()==name and (version is None or raw(row.contents.version).decode()==version):
   code,pkg=fetch(c,channel,index);assert code==0,code;return pkg
 raise AssertionError('package not found')


def plan(c,pkg):
 code,s=snapshot(c);assert code==0;code,p=install_plan(c,s,pkg);lib.apkg_snapshot_free(s);return code,p


class ChannelPlans(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0);lib.apkg_test_fail_alloc(0)
 def test_plan_owns_origin_and_canonical_digest_after_inputs_freed(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);_,endpoint,_=fixture(d);root=d/'root';state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);self.assertEqual(code,0);pkg=choose(c,ch,'demo');code,p=plan(c,pkg);self.assertEqual(code,0);v=lib.apkg_plan_get(p).contents;data=raw(v.canonical);obj=json.loads(data);origin=obj['body']['origins'][0];self.assertEqual(bytes.fromhex(origin['endpoint']).decode(),str(endpoint.resolve()));self.assertEqual(origin['snapshot_sha256'],head(endpoint));self.assertEqual(len(obj['body']['origins']),1);self.assertEqual(hashlib.sha256(data).digest(),bytes(v.plan_digest.bytes));lib.apkg_package_free(pkg);lib.apkg_channel_free(ch);self.assertEqual(run_plan(c,p),(0,1));self.assertNotIn(b'origins',(root/'.apkg/state').read_bytes());lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_changed_head_before_planning_or_after_approval_refuses_without_mutation(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);_,endpoint,_=fixture(d);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);pkg=choose(c,ch,'demo');reference=(endpoint/'current').read_bytes();(endpoint/'current').write_text('00'*32+'\n');code,p=plan(c,pkg);self.assertEqual(code,8);self.assertFalse(p);(endpoint/'current').write_bytes(reference);code,p=plan(c,pkg);self.assertEqual(code,0);code,a=approve(c,p);self.assertEqual(code,0);(endpoint/'current').write_text('00'*32+'\n');self.assertEqual(execute(c,p,a),(8,0));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'demo').exists());self.assertFalse(Path(str(root)+'.apkg-transaction').exists());lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_changed_source_during_stage_or_apply_restores_and_retry_is_explicit(self):
  for phase in (2,3):
   with self.subTest(phase=phase),tempfile.TemporaryDirectory() as d:
    d=Path(d);_,endpoint,_=fixture(d);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);pkg=choose(c,ch,'demo');code,p=plan(c,pkg);self.assertEqual(code,0);reference=(endpoint/'current').read_bytes()
    @EVENT_FN
    def change(user,event):
     if event.contents.phase==phase:(endpoint/'current').write_text('00'*32+'\n')
     return 0
    observer=Observer(change,None,None);code,a=approve(c,p);self.assertEqual(execute(c,p,a,C.byref(observer)),(8,2));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'demo').exists());self.assertFalse(Path(str(root)+'.apkg-transaction').exists());lib.apkg_approval_free(a);(endpoint/'current').write_bytes(reference);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_http_immutable_snapshot_tampering_refuses_before_mutation(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);_,endpoint,_=fixture(d);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1)
   with serve(endpoint) as (url,server):
    code,ch=http_open(c,url);self.assertEqual(code,0);pkg=choose(c,ch,'demo');code,p=plan(c,pkg);self.assertEqual(code,0);code,a=approve(c,p);document=endpoint/'snapshots'/head(endpoint)/'channel.json';data=json.loads(document.read_bytes());data['body']['sequence']='2';document.write_bytes(json.dumps(data,sort_keys=True,separators=(',',':')).encode());self.assertEqual(execute(c,p,a),(10,0));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-transaction').exists());lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_decided_recovery_and_subsequent_rollback_need_no_channel(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);_,endpoint,_=fixture(d);root=d/'root';state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);pkg=choose(c,ch,'demo');code,p=plan(c,pkg);self.assertEqual(code,0)
   @EVENT_FN
   def disconnect(user,event):
    if event.contents.phase==4:shutil.rmtree(endpoint)
    return 0
   observer=Observer(disconnect,None,None);code,a=approve(c,p);lib.apkg_test_fault(4,1);self.assertEqual(execute(c,p,a,C.byref(observer)),(9,3));lib.apkg_test_fault(0,0);self.assertEqual(recover(c),(0,1));self.assertEqual((root/'demo/demo-drawer/program').read_bytes(),b'body');lib.apkg_approval_free(a);lib.apkg_plan_free(p);history=next(path for path in root.parent.glob(root.name+'.apkg-history.*') if (path/'decision').exists() and (path/'new-state').read_bytes()==(root/'.apkg/state').read_bytes());code,p=op(c,'rollback',str(history));self.assertEqual(code,0);self.assertNotIn('origins',json.loads(raw(lib.apkg_plan_get(p).contents.canonical))['body']);self.assertEqual(run_plan(c,p),(0,1));self.assertFalse((root/'demo').exists());lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_selected_dependency_origin_survives_resolver_but_unused_candidate_is_ignored(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);key=key_fixture(d/'author-key');ckey=channel_key(d/'channel-key');objects={};endpoints={}
   for name in ('app','provider','unused'):
    folder=d/(name+'-drawer');author_fixture(folder,name=name,depends=['provider >= 1.1'] if name=='app' else []);archive=d/(name+'.pkg');result=build(folder,archive,key);self.assertEqual(result.returncode,0,result.stderr);endpoint=d/(name+'-channel');result=publish(archive,endpoint,ckey);self.assertEqual(result.returncode,0,result.stderr);endpoints[name]=endpoint
   root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1)
   for name in endpoints:
    code,ch=open_file(c,endpoints[name]);self.assertEqual(code,0);objects[name]=choose(c,ch,name);lib.apkg_channel_free(ch)
   code,s=snapshot(c);req=(C.c_void_p*1)(objects['app']);pool=(C.c_void_p*2)(objects['provider'],objects['unused']);p=C.c_void_p();self.assertEqual(lib.apkg_plan_resolve(c,s,req,1,pool,2,0,C.byref(p),None),0);lib.apkg_snapshot_free(s);origins=json.loads(raw(lib.apkg_plan_get(p).contents.canonical))['body']['origins'];self.assertEqual(len(origins),2);self.assertEqual({bytes.fromhex(o['endpoint']).decode() for o in origins},{str(endpoints['app'].resolve()),str(endpoints['provider'].resolve())});reference=(endpoints['provider']/'current').read_bytes();(endpoints['provider']/'current').write_text('00'*32+'\n');self.assertEqual(run_plan(c,p),(8,0));self.assertEqual((root/'.apkg/state').read_bytes(),before);(endpoints['provider']/'current').write_bytes(reference);shutil.rmtree(endpoints['unused']);self.assertEqual(run_plan(c,p),(0,1));self.assertTrue((root/'provider/provider-drawer/program').exists());self.assertFalse((root/'unused').exists());lib.apkg_plan_free(p)
   for pkg in objects.values():lib.apkg_package_free(pkg)
   lib.apkg_close(c)
 def test_origin_plan_allocation_failures_leave_state_and_are_retryable(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);_,endpoint,_=fixture(d);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);pkg=choose(c,ch,'demo');code,s=snapshot(c)
   failures=0;successes=0
   for point in range(1,401):
    lib.apkg_test_fail_alloc(point);code,p=install_plan(c,s,pkg);lib.apkg_test_fail_alloc(0);self.assertIn(code,(0,4),point)
    if code:failures+=1;self.assertFalse(p)
    else:successes+=1;lib.apkg_plan_free(p)
    self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-transaction').exists())
   self.assertGreater(failures,0);self.assertGreater(successes,0);lib.apkg_snapshot_free(s);code,p=plan(c,pkg);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_initial_single_package_path_also_binds_origin_and_restores(self):
  from test_payload import archive
  for phase in (None,2,3):
   with self.subTest(phase=phase),tempfile.TemporaryDirectory() as d:
    d=Path(d);archive(d/'simple.pkg',descriptor());key=channel_key(d/'channel-key');endpoint=d/'channel';self.assertEqual(publish(d/'simple.pkg',endpoint,key).returncode,0);root=d/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);pkg=choose(c,ch,'demo');code,p=plan(c,pkg);self.assertEqual(code,0);body=json.loads(raw(lib.apkg_plan_get(p).contents.canonical))['body'];self.assertEqual(body['action'],'install');self.assertIn('origins',body)
    @EVENT_FN
    def change(user,event):
     if event.contents.phase==phase:(endpoint/'current').write_text('00'*32+'\n')
     return 0
    observer=Observer(change,None,None);code,a=approve(c,p);self.assertEqual(execute(c,p,a,C.byref(observer)),(0,1) if phase is None else (8,2));self.assertEqual((root/'demo').exists(),phase is None)
    if phase is not None:self.assertEqual((root/'.apkg/state').read_bytes(),before)
    lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_upgrade_origin_is_bound_while_offline_repair_and_rollback_are_local(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);_,endpoint,_=fixture(d);root=d/'root';state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);pkg=choose(c,ch,'demo');code,p=plan(c,pkg);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_channel_free(ch)
   source=d/'demo-drawer';manifest=source.with_name(source.name+'.arospkg.toml');manifest.write_text(manifest.read_text().replace('version = "1.2"','version = "2.0"'));(source/'program').write_bytes(b'new body');new=d/'new.pkg';result=build(source,new,d/'demo.author-key');self.assertEqual(result.returncode,0,result.stderr);result=publish(new,endpoint,d/'channel-key');self.assertEqual(result.returncode,0,result.stderr);code,ch=open_file(c,endpoint);pkg=choose(c,ch,'demo','2.0');code,s=snapshot(c);p=C.c_void_p();self.assertEqual(lib.apkg_plan_upgrade(c,s,pkg,C.byref(p),None),0);lib.apkg_snapshot_free(s);self.assertIn('origins',json.loads(raw(lib.apkg_plan_get(p).contents.canonical))['body']);before=(root/'.apkg/state').read_bytes();reference=(endpoint/'current').read_bytes();(endpoint/'current').write_text('00'*32+'\n');self.assertEqual(run_plan(c,p),(8,0));self.assertEqual((root/'.apkg/state').read_bytes(),before);(endpoint/'current').write_bytes(reference);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_channel_free(ch);history=next(path for path in root.parent.glob(root.name+'.apkg-history.*') if (path/'decision').exists() and (path/'new-state').read_bytes()==(root/'.apkg/state').read_bytes());shutil.rmtree(endpoint);code,p=op(c,'rollback',str(history));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);self.assertEqual((root/'demo/demo-drawer/program').read_bytes(),b'body');(root/'demo/demo-drawer/program').write_bytes(b'damage');code,p=op(c,'repair');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual((root/'demo/demo-drawer/program').read_bytes(),b'body');lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_native_owned_plan_workflow_after_freeing_inputs(self):
  runner=Path(os.environ.get('APKG_CHANNEL_MEMORY',ROOT/'build/test_channel_memory'))
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);_,endpoint,_=fixture(d)
   with serve(endpoint) as (url,server):
    for mode,source in (('file',str(endpoint)),('http',url)):
     root=d/mode;state(root,policy=seed());result=subprocess.run([str(runner),str(root),source,CHANNEL,'--plan'],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertEqual((root/'demo/demo-drawer/program').read_bytes(),b'body')
 def test_even_another_granted_channel_signer_invalidates_approved_origin(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);_,endpoint,_=fixture(d);root=d/'root';public,secret=keypair(bytes([9])*32);policy_data=json.loads(seed());keys=policy_data['body']['channels'][0]['keys'];keys.append(dict(algorithm='ed25519',public_key=public.hex()));keys.sort(key=lambda key:key['public_key']);before=state(root,policy=json.dumps(policy_data,sort_keys=True,separators=(',',':')).encode());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);self.assertEqual(code,0);pkg=choose(c,ch,'demo');code,p=plan(c,pkg);self.assertEqual(code,0);code,a=approve(c,p);folder=endpoint/'snapshots'/head(endpoint);data=(folder/'channel.json').read_bytes();(folder/'channel.signature.json').write_bytes(sign(data,public,secret,'channel'));self.assertEqual(execute(c,p,a),(8,0));self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse((root/'demo').exists());lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_channel_free(ch);lib.apkg_close(c)
