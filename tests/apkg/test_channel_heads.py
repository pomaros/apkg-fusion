# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import unittest
from support import *
from test_channel import CHANNEL, CP, CS, ChannelView, catalog, entry, read_channel, seed, selected
from test_file_channel import head, open_file
from test_http_channel import fixture, serve, http_open
from test_operations import run_plan, install, op, payload
import test_client as clients

lib.apkg_plan_channel_accept.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_plan_channel_accept.restype=C.c_int
class HeadView(C.Structure):_fields_=ChannelView._fields_+[('accepted',U64)]
def view(ch):return C.cast(lib.apkg_channel_get(ch),C.POINTER(HeadView)).contents

def accepting(c,ch):
 code,s=snapshot(c);assert code==0;out=C.c_void_p();code=lib.apkg_plan_channel_accept(c,s,ch,C.byref(out),None);lib.apkg_snapshot_free(s);return code,out

def accept(c,data):
 code,ch=read_channel(c,data);assert code==0,code;code,p=accepting(c,ch);lib.apkg_channel_free(ch);assert code==0,code;result=run_plan(c,p);lib.apkg_plan_free(p);return result

def heads(root):return json.loads((root/'.apkg/state').read_bytes())['body'].get('channel_heads',[])

def rewrite(endpoint,data):
 digest=hashlib.sha256(data).hexdigest();folder=endpoint/'snapshots'/digest;folder.mkdir(exist_ok=True);(folder/'channel.json').write_bytes(data);(folder/'channel.signature.json').write_bytes(sign(data,CP,CS,'channel'));(endpoint/'current').write_text(digest+'\n');return digest

class ChannelHeads(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0);lib.apkg_test_fail_alloc(0)
 def test_readonly_discovery_unknown_freshness_and_explicit_acceptance(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';before=state(root,policy=seed());data=catalog(sequence=4);code,c=open_ctx(root);code,ch=read_channel(c,data);self.assertEqual(code,0);self.assertEqual(view(ch).accepted.state,0);self.assertEqual(view(ch).freshness,0);code,p=accepting(c,ch);self.assertEqual(code,0);self.assertEqual(approve(c,p)[0],17);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertEqual(list(root.parent.iterdir()),[root]);lib.apkg_plan_free(p);lib.apkg_channel_free(ch);lib.apkg_close(c)
   code,c=open_ctx(root,1);self.assertEqual(accept(c,data),(0,1));body=json.loads((root/'.apkg/state').read_bytes())['body'];self.assertEqual(body['installed'],[]);self.assertEqual(body['policy'],json.loads(seed()));self.assertEqual(body['generation'],'2');self.assertEqual(heads(root),[dict(id=CHANNEL,sequence='4',snapshot_sha256=hashlib.sha256(data).hexdigest())]);lib.apkg_close(c);code,c=open_ctx(root);before=(root/'.apkg/state').read_bytes();code,ch=read_channel(c,data);self.assertEqual(code,0);self.assertEqual(view(ch).accepted.state,1);self.assertEqual(view(ch).accepted.value,4);self.assertEqual(view(ch).freshness,0);lib.apkg_channel_free(ch);code,ch=read_channel(c,catalog(sequence=5));self.assertEqual(code,0);self.assertEqual(view(ch).accepted.value,4);lib.apkg_channel_free(ch);self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_close(c)
 def test_replay_and_same_sequence_equivocation_are_refused_across_contexts(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=seed());code,c=open_ctx(root,1);data=catalog(sequence=7);self.assertEqual(accept(c,data),(0,1));lib.apkg_close(c);code,c=open_ctx(root);before=(root/'.apkg/state').read_bytes();self.assertEqual(read_channel(c,catalog(sequence=6))[0],8);fork=catalog(entries=[],sequence=7);self.assertEqual(read_channel(c,fork)[0],14);self.assertEqual(read_channel(c,data,floor=8)[0],8);self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_close(c)
 def test_old_observation_is_rechecked_and_racing_approvals_do_not_lower_floor(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=seed());code,c=open_ctx(root,1);code,old=read_channel(c,catalog(sequence=2));code,p=accepting(c,old);self.assertEqual(code,0);code,a=approve(c,p);self.assertEqual(code,0);self.assertEqual(accept(c,catalog(sequence=3)),(0,1));self.assertEqual(execute(c,p,a),(8,0));self.assertEqual(selected(c,old)[0],8);self.assertEqual(accepting(c,old)[0],8);self.assertEqual(heads(root)[0]['sequence'],'3');lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_channel_free(old);lib.apkg_close(c)
 def test_mirrors_http_and_file_share_floor_and_same_sequence_digest(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);_,endpoint,_=fixture(d);old_data=(endpoint/'snapshots'/head(endpoint)/'channel.json').read_bytes();root=d/'root';state(root,policy=seed());code,c=open_ctx(root,1);code,ch=open_file(c,endpoint);self.assertEqual(code,0);code,p=accepting(c,ch);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_channel_free(ch);self.assertEqual(accept(c,catalog(sequence=2)),(0,1));mirror=d/'mirror';shutil.copytree(endpoint,mirror);before=(root/'.apkg/state').read_bytes();self.assertEqual(open_file(c,mirror)[0],8)
   with serve(mirror) as (url,server):self.assertEqual(http_open(c,url)[0],8)
   rewrite(mirror,catalog(entries=[],sequence=2));self.assertEqual(open_file(c,mirror)[0],14);self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_close(c)
 def test_package_operations_preserve_floor_and_acceptance_is_not_package_rollback(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=seed());code,c=open_ctx(root,1);self.assertEqual(accept(c,catalog(sequence=2)),(0,1));saved=heads(root);install(c,descriptor());self.assertEqual(heads(root),saved);self.assertEqual(accept(c,catalog(sequence=5)),(0,1));saved=heads(root);history=next(p for p in root.parent.glob(root.name+'.apkg-history.*') if (p/'decision').exists() and (p/'new-state').read_bytes()==(root/'.apkg/state').read_bytes());before=(root/'.apkg/state').read_bytes();code,p=op(c,'rollback',str(history));self.assertEqual(code,17);self.assertFalse(p);self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertEqual(heads(root),saved);self.assertTrue((root/'demo').exists());code,p=op(c,'remove');self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);self.assertEqual(heads(root),saved);self.assertEqual(read_channel(c,catalog(sequence=4))[0],8);lib.apkg_close(c)
 def test_malformed_stored_heads_refuse_readonly_without_repair(self):
  good=dict(id=CHANNEL,sequence='1',snapshot_sha256='00'*32)
  for invalid in (None,{},[dict(good,sequence='0')],[dict(good,sequence='18446744073709551616')],[dict(good,snapshot_sha256='z'*64)],[good,good],[dict(good,extra=True)]):
   with self.subTest(invalid=invalid),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';data=json.loads(state(root,policy=seed()));data['body']['channel_heads']=invalid;before=json.dumps(data,sort_keys=True,separators=(',',':')).encode();(root/'.apkg/state').write_bytes(before);code,c=open_ctx(root);self.assertEqual(code,0);code,s=snapshot(c);self.assertEqual(code,1);self.assertFalse(s);self.assertEqual(read_channel(c,catalog())[0],1);self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_snapshot_free(s);lib.apkg_close(c)
 def test_acceptance_cancellation_and_io_failures_preserve_or_commit_explicitly(self):
  for point in (1,2,4,5,6):
   with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=read_channel(c,catalog(sequence=2));code,p=accepting(c,ch);self.assertEqual(code,0);code,a=approve(c,p);lib.apkg_test_fault(point,1);code,outcome=execute(c,p,a);lib.apkg_test_fault(0,0);self.assertNotEqual(code,0)
    if outcome==3:self.assertEqual(recover(c)[0],0)
    if point<4:self.assertEqual((root/'.apkg/state').read_bytes(),before)
    else:self.assertEqual(heads(root)[0]['sequence'],'2')
    self.assertEqual(recover(c),(0,0));lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_channel_free(ch);lib.apkg_close(c)
  for phase in (0,2,4):
   with self.subTest(phase=phase),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=read_channel(c,catalog());code,p=accepting(c,ch);code,a=approve(c,p)
    @EVENT_FN
    def cancel(user,event):return int(event.contents.phase==phase)
    observer=Observer(cancel,None,None);self.assertEqual(execute(c,p,a,C.byref(observer)),(0,1) if phase==4 else (16,0 if phase==0 else 2))
    if phase<4:self.assertEqual((root/'.apkg/state').read_bytes(),before)
    lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_process_death_and_repeated_recovery_keep_monotonic_acceptance(self):
  for point in (2,4,5):
   with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
    root=Path(d)/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=read_channel(c,catalog(sequence=2));code,p=accepting(c,ch);code,a=approve(c,p);pid=os.fork()
    if pid==0:lib.apkg_test_fault(point,2);execute(c,p,a);os._exit(98)
    _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99)
    if point>=4:
     pid=os.fork()
     if pid==0:lib.apkg_test_fault(6,2);recover(c);os._exit(98)
     _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99)
    self.assertEqual(recover(c),(0,2 if point==2 else 1));self.assertEqual(recover(c),(0,0))
    if point==2:self.assertEqual((root/'.apkg/state').read_bytes(),before)
    else:self.assertEqual(read_channel(c,catalog(sequence=1))[0],8)
    lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_allocation_failures_remain_readonly_and_retryable(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';before=state(root,policy=seed());code,c=open_ctx(root,1);code,ch=read_channel(c,catalog(sequence=9));code,s=snapshot(c);failures=successes=0
   for nth in range(1,281):
    out=C.c_void_p();lib.apkg_test_fail_alloc(nth);code=lib.apkg_plan_channel_accept(c,s,ch,C.byref(out),None);lib.apkg_test_fail_alloc(0);self.assertIn(code,(0,4),nth)
    if code:failures+=1;self.assertFalse(out)
    else:successes+=1;lib.apkg_plan_free(out)
    self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-transaction').exists())
   self.assertGreater(failures,0);self.assertGreater(successes,0);lib.apkg_snapshot_free(s);code,p=accepting(c,ch);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_actual_cli_previews_and_confirms_file_and_http_acceptance(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);_,endpoint,_=fixture(d)
   with serve(endpoint) as (url,server):
    for name,source in (('file',endpoint),('http',url)):
     root=d/name;before=state(root,policy=seed());preview=clients.cli('channel-accept',root,source,CHANNEL);self.assertEqual(preview.returncode,16,preview.stdout+preview.stderr);self.assertIn('Accepted sequence: 1',preview.stdout);self.assertEqual((root/'.apkg/state').read_bytes(),before);digest=re.search(r'Approved plan: ([a-f0-9]{64})',preview.stdout).group(1);done=clients.cli('channel-accept',root,source,CHANNEL,'--confirm',digest);self.assertEqual(done.returncode,0,done.stdout+done.stderr);self.assertEqual(heads(root)[0]['sequence'],'1');opened=clients.cli('channel-open',root,source,CHANNEL);self.assertEqual(opened.returncode,0,opened.stdout+opened.stderr);result=json.loads(opened.stdout);self.assertEqual(result['accepted_sequence'],'1');self.assertEqual(result['freshness'],'unknown');self.assertFalse((root/'demo').exists())
 def test_independent_channel_ids_sorted_marks_and_uint64_boundary(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';other='ef'*16;policy=json.loads(seed());policy['body']['channels'].append(dict(id=other,keys=[dict(algorithm='ed25519',public_key=CP.hex())]));state(root,policy=json.dumps(policy,sort_keys=True,separators=(',',':')).encode());code,c=open_ctx(root,1);data=catalog(entries=[],identity=other,sequence=2**64-1);code,ch=read_channel(c,data,identity=other);self.assertEqual(code,0);code,p=accepting(c,ch);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_channel_free(ch);self.assertEqual(accept(c,catalog(sequence=1)),(0,1));self.assertEqual([h['id'] for h in heads(root)],[CHANNEL,other]);self.assertEqual(read_channel(c,catalog(entries=[],identity=other,sequence=2**64-2),identity=other)[0],8);self.assertEqual(read_channel(c,catalog(entries=[],identity=other,sequence=2**64),identity=other)[0],2);lib.apkg_close(c)
 def test_floor_registry_limit_refuses_addition_without_losing_existing_marks(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';data=json.loads(state(root,policy=seed()));data['body']['channel_heads']=[dict(id=f'{i:032x}',sequence='1',snapshot_sha256='00'*32) for i in range(1,1025)];before=json.dumps(data,sort_keys=True,separators=(',',':')).encode();(root/'.apkg/state').write_bytes(before);code,c=open_ctx(root,1);code,ch=read_channel(c,catalog());self.assertEqual(code,0);self.assertEqual(accepting(c,ch)[0],3);self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_real_package_rollback_survives_later_catalog_acceptance_without_replay(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=seed());code,c=open_ctx(root,1);self.assertEqual(accept(c,catalog(sequence=2)),(0,1));install(c,descriptor());data=json.loads(descriptor(files=[payload('hello','upgraded')]));data['body']['version']='2.0';data=json.dumps(data,sort_keys=True,separators=(',',':')).encode();code,pkg=package(c,data,sign(data));self.assertEqual(code,0);code,s=snapshot(c);p=C.c_void_p();self.assertEqual(lib.apkg_plan_upgrade(c,s,pkg,C.byref(p),None),0);lib.apkg_snapshot_free(s);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_package_free(pkg);after_upgrade=(root/'.apkg/state').read_bytes();history=next(p for p in root.parent.glob(root.name+'.apkg-history.*') if (p/'decision').exists() and (p/'new-state').read_bytes()==after_upgrade);revision=json.loads(after_upgrade)['body']['package_generation'];self.assertEqual(accept(c,catalog(sequence=8)),(0,1));self.assertEqual(json.loads((root/'.apkg/state').read_bytes())['body']['package_generation'],revision);code,p=op(c,'rollback',str(history));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);self.assertEqual((root/'demo/hello').read_text(),'hello\n');self.assertEqual(heads(root)[0]['sequence'],'8');self.assertEqual(read_channel(c,catalog(sequence=7))[0],8);self.assertEqual(op(c,'rollback',str(history))[0],8);lib.apkg_close(c)
