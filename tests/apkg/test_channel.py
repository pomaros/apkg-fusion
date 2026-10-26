# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import copy
import ctypes as C
import os
import subprocess
import tempfile
import unittest
from support import *
from test_client import cli
from test_author import author_fixture,key_fixture,build,members

CHANNEL='cd'*16
CP,CS=keypair(bytes(reversed(range(32))))
class ChannelView(C.Structure):
 _fields_=[('id',ID),('sequence',C.c_uint64),('count',C.c_uint64),('digest',Digest),('signature',C.c_int),('trust',C.c_int),('freshness',C.c_int)]
class ChannelRow(C.Structure):
 _fields_=[('authority',Text),('name',Text),('version',Text),('target',Text),('revision',C.c_uint64),('archive_bytes',C.c_uint64),('descriptor_digest',Digest),('archive_digest',Digest),('signature',C.c_int),('trust',C.c_int),('target_matches',C.c_int),('descriptor',Text),('envelope',Text)]
lib.apkg_channel_read.argtypes=[C.c_void_p,C.POINTER(ID),C.c_uint64,Text,Text,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_channel_read.restype=C.c_int
lib.apkg_channel_get.argtypes=[C.c_void_p];lib.apkg_channel_get.restype=C.POINTER(ChannelView)
lib.apkg_channel_entry_at.argtypes=[C.c_void_p,C.c_uint64,C.POINTER(C.POINTER(ChannelRow)),C.c_void_p];lib.apkg_channel_entry_at.restype=C.c_int
lib.apkg_channel_package.argtypes=[C.c_void_p,C.c_void_p,C.c_uint64,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_channel_package.restype=C.c_int
lib.apkg_channel_free.argtypes=[C.c_void_p]

def grant(identity,key):return dict(id=identity,keys=[dict(algorithm='ed25519',public_key=key.hex())])
def seed(authors=True,channels=True):return wire('policy',dict(authorities=[grant(AUTH,PK)] if authors else [],**({'channels':[grant(CHANNEL,CP)]} if channels else {})))
def entry(data=None,signature=None):
 data=data or descriptor();return dict(archive=dict(sha256=hashlib.sha256(b'archive bytes').hexdigest(),size='13'),descriptor=json.loads(data),signature=json.loads(signature or sign(data)))
def catalog(entries=None,sequence=1,identity=CHANNEL):
 items=entries if entries is not None else [entry()];items=sorted(items,key=lambda item:hashlib.sha256(json.dumps(item['descriptor'],sort_keys=True,separators=(',',':')).encode()).digest());return wire('channel',dict(entries=items,id=identity,sequence=str(sequence)))
def read_channel(c,data,signature=None,identity=CHANNEL,floor=0):
 sig=signature or sign(data,CP,CS,'channel');a=C.create_string_buffer(data);b=C.create_string_buffer(sig);out=C.c_void_p();ident=ID();ident.bytes[:]=bytes.fromhex(identity)
 code=lib.apkg_channel_read(c,C.byref(ident),floor,Text(C.cast(a,C.c_void_p),len(data)),Text(C.cast(b,C.c_void_p),len(sig)),C.byref(out),None);return code,out

def selected(c,ch,index=0):
 out=C.c_void_p();code=lib.apkg_channel_package(c,ch,index,C.byref(out),None);return code,out

class Channels(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fail_alloc(0)
 def test_context_discovery_is_readonly_and_views_own_their_bytes(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';before=state(root,policy=seed());code,c=open_ctx(root);self.assertEqual(code,0);data=catalog();code,ch=read_channel(c,data,floor=1);self.assertEqual(code,0);v=lib.apkg_channel_get(ch).contents;self.assertEqual(v.sequence,1);self.assertEqual(v.count,1);self.assertEqual(v.signature,1);self.assertEqual(v.trust,1);self.assertEqual(v.freshness,0);self.assertEqual(bytes(v.digest.bytes),hashlib.sha256(data).digest());row=C.POINTER(ChannelRow)();self.assertEqual(lib.apkg_channel_entry_at(ch,0,C.byref(row),None),0);self.assertEqual(raw(row.contents.name),b'demo');self.assertEqual(row.contents.trust,1);self.assertEqual(row.contents.target_matches,1);self.assertEqual(raw(row.contents.descriptor),descriptor());code,p=selected(c,ch);self.assertEqual(code,0);lib.apkg_package_free(p);self.assertEqual(selected(c,ch,1)[0],5);lib.apkg_close(c);self.assertEqual(raw(row.contents.name),b'demo');self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertEqual(list(root.parent.iterdir()),[root]);lib.apkg_channel_free(ch)
 def test_channel_key_does_not_grant_author_trust_or_target_compatibility(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=seed(authors=False));code,c=open_ctx(root);code,ch=read_channel(c,catalog());self.assertEqual(code,0);row=C.POINTER(ChannelRow)();lib.apkg_channel_entry_at(ch,0,C.byref(row),None);self.assertEqual(row.contents.trust,2);self.assertEqual(selected(c,ch)[0],12);lib.apkg_channel_free(ch);author=json.loads(descriptor());author['body']['authority']=CHANNEL;author=wire('descriptor',author['body']);code,ch=read_channel(c,catalog([entry(author,sign(author,CP,CS))]));self.assertEqual(code,0);lib.apkg_channel_entry_at(ch,0,C.byref(row),None);self.assertEqual(row.contents.trust,2);self.assertEqual(selected(c,ch)[0],12);lib.apkg_channel_free(ch);lib.apkg_close(c)
   state(root,policy=seed());code,c=open_ctx(root);wrong=descriptor(target='aarch64-aros-v1-hosted');code,ch=read_channel(c,catalog([entry(wrong)]));self.assertEqual(code,0);lib.apkg_channel_entry_at(ch,0,C.byref(row),None);self.assertEqual(row.contents.target_matches,0);self.assertEqual(selected(c,ch)[0],13);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_unknown_channel_author_role_and_signature_domain_refuse(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=seed(channels=False));code,c=open_ctx(root);data=catalog();self.assertEqual(read_channel(c,data)[0],12);same_id=catalog(identity=AUTH);self.assertEqual(read_channel(c,same_id,sign(same_id,PK,SK,'channel'),identity=AUTH)[0],12);lib.apkg_close(c)
   state(root,policy=seed());code,c=open_ctx(root)
   for sig in (sign(data),sign(data,PK,SK,'channel'),sign(data,CP,CS,'attestation')):self.assertIn(read_channel(c,data,sig)[0],(11,12))
   self.assertEqual(read_channel(c,data,identity='ef'*16)[0],12);changed=data.replace(b'"sequence":"1"',b'"sequence":"2"');self.assertEqual(read_channel(c,changed,sign(data,CP,CS,'channel'))[0],10);lib.apkg_close(c)
 def test_sequence_floor_and_policy_root_binding(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=seed());code,c=open_ctx(root);data=catalog(sequence=2);self.assertEqual(read_channel(c,data,floor=3)[0],8);code,ch=read_channel(c,data,floor=2);self.assertEqual(code,0);other=Path(d)/'other';state(other,policy=seed());code,oc=open_ctx(other);self.assertEqual(selected(oc,ch)[0],8);lib.apkg_close(oc);state(root,2,policy=seed(authors=False));self.assertEqual(selected(c,ch)[0],8);lib.apkg_channel_free(ch);lib.apkg_close(c)
 def test_malformed_ambiguous_and_self_awarded_qualifications_refuse(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=seed());code,c=open_ctx(root);base=entry();bad=[];review=copy.deepcopy(base);review['review']='approved';bad.append(catalog([review]));bad.append(catalog([base,base]));changed=json.loads(descriptor());changed['body']['files'][0]['mode']='493';changed=wire('descriptor',changed['body']);bad.append(catalog([base,entry(changed)]));wrong=copy.deepcopy(base);wrong['signature']['signature']='00'*64;bad.append(catalog([wrong]));oversized=copy.deepcopy(base);oversized['archive']['size']=str(2**31);bad.append(catalog([oversized]));bad.append(catalog(sequence=0))
   for data in bad:
    with self.subTest(data=data[:90]):code,ch=read_channel(c,data);self.assertNotEqual(code,0);self.assertFalse(ch)
   lib.apkg_close(c)
 def test_initialization_previews_every_channel_role_without_publishing_trust(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';policy_file=Path(d)/'policy';policy_file.write_bytes(seed());answer=cli('init',root,TARGET,policy_file);self.assertEqual(answer.returncode,16,answer.stderr);self.assertIn('Authorize publisher '+AUTH,answer.stdout);self.assertIn('Authorize channel '+CHANNEL,answer.stdout);self.assertIn(CP.hex(),answer.stdout);self.assertFalse(root.exists());answer=cli('init',root,TARGET,policy_file,'--yes');self.assertEqual(answer.returncode,0,answer.stderr);self.assertEqual(json.loads((root/'.apkg/state').read_bytes())['body']['policy'],json.loads(seed()))
 def test_real_toml_descriptor_native_channel_signing_and_inspection(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);drawer=d/'Drawer';author_fixture(drawer);key=key_fixture(d/'author-key');archive=d/'artifact';built=build(drawer,archive,key);self.assertEqual(built.returncode,0,built.stderr);packed=members(archive);item=dict(archive=dict(sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),size=str(archive.stat().st_size)),descriptor=json.loads(packed['descriptor.json']),signature=json.loads(packed['signature.json']));data=catalog([item]);channel_dir=d/'channel';channel_dir.mkdir();document=channel_dir/'channel.json';document.write_bytes(data);ck=d/'channel-key';ck.write_text('Pkg-Secret-Key: 1\nSeed: '+bytes(reversed(range(32))).hex()+'\nPublic: '+CP.hex()+'\n');signed=cli('sign-object',ck,'channel',document);self.assertEqual(signed.returncode,0,signed.stderr);self.assertEqual(json.loads(signed.stdout),json.loads(sign(data,CP,CS,'channel')));(channel_dir/'channel.signature.json').write_bytes(json.dumps(json.loads(signed.stdout),sort_keys=True,separators=(',',':')).encode());root=d/'root';before=state(root,policy=seed());inspected=cli('channel-inspect',root,channel_dir,CHANNEL,1);self.assertEqual(inspected.returncode,0,inspected.stderr);view=json.loads(inspected.stdout);self.assertEqual(view['entries'][0]['name'],'demo');self.assertEqual(view['entries'][0]['author_trust'],'accepted');self.assertEqual(view['entries'][0]['review'],'not-asserted');self.assertEqual(view['freshness'],'unknown');self.assertEqual((root/'.apkg/state').read_bytes(),before);self.assertFalse(Path(str(root)+'.apkg-objects').exists())
 def test_channel_read_allocation_failures_are_typed_readonly_and_retryable(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';before=state(root,policy=seed());code,c=open_ctx(root);data=catalog()
   for nth in range(1,241):
    with self.subTest(nth=nth):
     lib.apkg_test_fail_alloc(nth)
     try:code,ch=read_channel(c,data)
     finally:lib.apkg_test_fail_alloc(0)
     self.assertIn(code,(0,4));self.assertEqual(bool(ch),code==0);lib.apkg_channel_free(ch);self.assertEqual((root/'.apkg/state').read_bytes(),before)
   code,ch=read_channel(c,data);self.assertEqual(code,0);lib.apkg_channel_free(ch);lib.apkg_close(c)

 def test_native_channel_and_package_ownership_after_inputs_and_context_close(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);root=d/'root';state(root,policy=seed());document=d/'channel.json';data=catalog();document.write_bytes(data);signature=d/'signature.json';signature.write_bytes(sign(data,CP,CS,'channel'));runner=Path(os.environ.get('APKG_CHANNEL_MEMORY',ROOT/'build/test_channel_memory'));result=subprocess.run([str(runner),str(root),str(document),str(signature),CHANNEL],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
 def test_native_inspector_rejects_bad_envelopes_roles_and_qualifications(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);root=d/'root';before=state(root,policy=seed());folder=d/'channel';folder.mkdir();base=entry();review=copy.deepcopy(base);review['runtime_test']='passed';cases=[(catalog([review]),None,2),(catalog([base,base]),None,14),(catalog(),sign(catalog()),11),(catalog(),sign(catalog(),PK,SK,'channel'),12)]
   for data,envelope,expected in cases:
    (folder/'channel.json').write_bytes(data);(folder/'channel.signature.json').write_bytes(envelope or sign(data,CP,CS,'channel'));result=cli('channel-inspect',root,folder,CHANNEL);self.assertEqual(result.returncode,expected,result.stdout+result.stderr);self.assertEqual((root/'.apkg/state').read_bytes(),before)
