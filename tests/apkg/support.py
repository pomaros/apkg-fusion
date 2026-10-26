# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import hashlib
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
lib = C.CDLL(str(ROOT / 'build/libapkg-test.dylib'))
class Text(C.Structure): _fields_=[('data',C.c_void_p),('size',C.c_size_t)]
class ID(C.Structure): _fields_=[('bytes',C.c_ubyte*16)]
class Digest(C.Structure): _fields_=[('bytes',C.c_ubyte*32)]
class U64(C.Structure): _fields_=[('state',C.c_int),('value',C.c_uint64),('evidence',Text)]
class Fact(C.Structure): _fields_=[('state',C.c_int),('value',Text),('evidence',Text)]
class RootView(C.Structure): _fields_=[('state',C.c_int),('identity_state',C.c_int),('root_id',ID),('generation',U64),('pending_count',U64),('snapshot_digest',Digest),('target',Fact),('diagnostic',Text),('source_record',Text)]
class Options(C.Structure): _fields_=[('version',C.c_uint32),('size',C.c_size_t),('root',Text),('mode',C.c_int),('object_max',C.c_uint64),('memory_max',C.c_uint64)]
for name,args,restype in [('apkg_open',[C.POINTER(Options),C.POINTER(C.c_void_p),C.c_void_p],C.c_int),('apkg_snapshot_read',[C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p],C.c_int),('apkg_snapshot_root',[C.c_void_p],C.POINTER(RootView)),('apkg_snapshot_installed_count',[C.c_void_p],C.c_uint64),('apkg_close',[C.c_void_p],None),('apkg_snapshot_free',[C.c_void_p],None)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=restype

def wire(kind,body,requires=None): return json.dumps(dict(body=body,format='apkg-json-1',requires=requires or [],schema='1',type=kind),ensure_ascii=False,sort_keys=True,separators=(',',':')).encode()
def open_ctx(path,mode=0,budget=0):
 b=str(path).encode();buf=C.create_string_buffer(b);o=Options(1,C.sizeof(Options),Text(C.cast(buf,C.c_void_p),len(b)),mode,budget,0);ctx=C.c_void_p();r=lib.apkg_open(C.byref(o),C.byref(ctx),None);return r,ctx

def snapshot(ctx):
 s=C.c_void_p();r=lib.apkg_snapshot_read(ctx,C.byref(s),None);return r,s

def raw(v): return C.string_at(v.data,v.size) if v.data else b''
def state(root,generation=1,target='x86_64-aros-v11-native',installed=None,policy=None):
 root.mkdir(parents=True,exist_ok=True);(root/'.apkg').mkdir(exist_ok=True)
 data=wire('root',dict(generation=str(generation),installed=installed or [],policy=json.loads(policy or wire('policy',dict(authorities=[]))),root_id='11'*16,target=target))
 (root/'.apkg/state').write_bytes(data);return data
AUTH='ab'*16
TARGET='x86_64-aros-v11-native'
lib.pkg_ed25519_keypair.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p]
lib.pkg_ed25519_sign.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p]
lib.apkg_package_read.argtypes=[C.c_void_p,Text,Text,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_package_read.restype=C.c_int
lib.apkg_package_free.argtypes=[C.c_void_p]
def keypair(seed=bytes(range(32))):
 pk=C.create_string_buffer(32);sk=C.create_string_buffer(64);lib.pkg_ed25519_keypair(pk,sk,seed);return pk.raw,sk.raw
PK,SK=keypair()
def policy(pk=PK):return wire('policy',dict(authorities=[dict(id=AUTH,keys=[dict(algorithm='ed25519',public_key=pk.hex())])]))
def descriptor(name='demo',target=TARGET,files=None):
 if files is None:files=[dict(data=b'hello\n'.hex(),mode='420',path='hello',sha256=hashlib.sha256(b'hello\n').hexdigest(),size='6',type='file')]
 return wire('descriptor',dict(authority=AUTH,files=files,kind='data',name=name,revision='1',target=target,version='1.0'))
def sign(data,pk=PK,sk=SK,domain='descriptor'):
 digest=hashlib.sha256(data).digest();msg=b'APKG-SIG\0\1'+bytes([dict(descriptor=1,channel=2,attestation=3)[domain]])+len(data).to_bytes(8,'big')+digest+pk
 signature=C.create_string_buffer(64);lib.pkg_ed25519_sign(signature,msg,len(msg),sk)
 return json.dumps(dict(bytes=str(len(data)),domain=domain,format='apkg-signature-1',public_key=pk.hex(),sha256=digest.hex(),signature=signature.raw.hex()),sort_keys=True,separators=(',',':')).encode()
def package(ctx,data,signature):
 a=C.create_string_buffer(data);b=C.create_string_buffer(signature);p=C.c_void_p();r=lib.apkg_package_read(ctx,Text(C.cast(a,C.c_void_p),len(data)),Text(C.cast(b,C.c_void_p),len(signature)),C.byref(p),None);return r,p
class PlanView(C.Structure): _fields_=[('kind',C.c_int),('plan_digest',Digest),('root_id',ID),('generation',C.c_uint64),('effect_count',C.c_uint64),('canonical',Text),('executable',C.c_int)]
lib.apkg_plan_initialize.argtypes=[C.c_void_p,C.c_void_p,Text,Text,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_plan_initialize.restype=C.c_int
lib.apkg_plan_install.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_plan_install.restype=C.c_int
lib.apkg_plan_get.argtypes=[C.c_void_p];lib.apkg_plan_get.restype=C.POINTER(PlanView)
lib.apkg_plan_free.argtypes=[C.c_void_p]
lib.apkg_plan_approve.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(Digest),C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_plan_approve.restype=C.c_int
lib.apkg_approval_free.argtypes=[C.c_void_p]
def initialize_plan(ctx,s,target=TARGET,seed=None):
 seed=seed or policy();t=target.encode();tb=C.create_string_buffer(t);pb=C.create_string_buffer(seed);p=C.c_void_p();r=lib.apkg_plan_initialize(ctx,s,Text(C.cast(tb,C.c_void_p),len(t)),Text(C.cast(pb,C.c_void_p),len(seed)),C.byref(p),None);return r,p
def install_plan(ctx,s,pkg):
 p=C.c_void_p();r=lib.apkg_plan_install(ctx,s,pkg,None,C.byref(p),None);return r,p
def approve(ctx,p):
 a=C.c_void_p();v=lib.apkg_plan_get(p).contents;r=lib.apkg_plan_approve(ctx,p,C.byref(v.plan_digest),C.byref(a),None);return r,a
class ResultView(C.Structure):_fields_=[('operation_id',ID),('status',C.c_int),('outcome',C.c_int),('cancellation_requested',C.c_int),('committed_generation',U64),('record',Text)]
lib.apkg_execute.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_execute.restype=C.c_int
lib.apkg_result_get.argtypes=[C.c_void_p];lib.apkg_result_get.restype=C.POINTER(ResultView)
lib.apkg_result_free.argtypes=[C.c_void_p]
lib.apkg_recover.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p];lib.apkg_recover.restype=C.c_int
lib.apkg_test_fault.argtypes=[C.c_int,C.c_int]
def execute(ctx,p,a,observer=None):
 r=C.c_void_p();code=lib.apkg_execute(ctx,p,a,observer,C.byref(r),None);view=lib.apkg_result_get(r).contents if r else None;outcome=view.outcome if view else None;lib.apkg_result_free(r);return code,outcome

def recover(ctx):
 r=C.c_void_p();code=lib.apkg_recover(ctx,None,C.byref(r),None);outcome=lib.apkg_result_get(r).contents.outcome if r else None;lib.apkg_result_free(r);return code,outcome

def init(root,existing=False):
 if existing:root.mkdir()
 code,c=open_ctx(root,1);assert code==0;code,s=snapshot(c);assert code==0;code,p=initialize_plan(c,s);assert code==0;code,a=approve(c,p);assert code==0
 result=execute(c,p,a);assert result==(0,1),result
 lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_snapshot_free(s);return c

def prepared_install(ctx,name='demo',files=None):
 r,s=snapshot(ctx);assert r==0;data=descriptor(name=name,files=files);r,pkg=package(ctx,data,sign(data));assert r==0,r;r,p=install_plan(ctx,s,pkg);assert r==0,r;r,a=approve(ctx,p);assert r==0;lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);return p,a
class Event(C.Structure):_fields_=[('operation_id',ID),('sequence',C.c_uint64),('kind',C.c_int),('phase',C.c_int),('subject',Text),('message',Text),('completed',C.c_uint64),('total',U64),('unit',Text),('cancellable',C.c_int)]
EVENT_FN=C.CFUNCTYPE(C.c_int,C.c_void_p,C.POINTER(Event))
class Observer(C.Structure):_fields_=[('event',EVENT_FN),('user',C.c_void_p),('cancel',C.c_void_p)]
lib.apkg_test_fail_alloc.argtypes=[C.c_uint]
