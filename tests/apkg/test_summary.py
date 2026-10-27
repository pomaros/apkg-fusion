# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import subprocess
import tempfile
import unittest
from support import *
from test_client import CLI, WORKER
from test_channel import CHANNEL, CP, CS, catalog, entry, seed
from test_updates import desc, refresh
from test_operations import install, op, run_plan
from test_channel_heads import accept
from test_http_channel import serve

lib.apkg_channel_summarize.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_channel_summarize.restype = C.c_int
lib.apkg_update_summary_get.argtypes = [C.c_void_p]
lib.apkg_update_summary_get.restype = C.c_void_p
lib.apkg_update_summary_free.argtypes = [C.c_void_p]
lib.apkg_update_summary_format.argtypes = [C.c_void_p, C.c_char_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_update_summary_format.restype = C.c_int
lib.apkg_client_outdated.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_char_p,
    C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_outdated.restype = C.c_int


def emit(endpoint, descriptors):
    entries = [entry(data, signature) for data, signature in descriptors]
    data = catalog(entries); digest = hashlib.sha256(data).hexdigest()
    folder = endpoint / 'snapshots' / digest; folder.mkdir(parents=True)
    (folder / 'channel.json').write_bytes(data)
    (folder / 'channel.signature.json').write_bytes(sign(data, CP, CS, 'channel'))
    (endpoint / 'current').write_text(digest + '\n')


def summarize(s, ch):
    out = C.c_void_p(); code = lib.apkg_channel_summarize(s, ch, C.byref(out), None)
    return code, out


def formatted(summary, field=None):
    out = C.c_void_p()
    code = lib.apkg_update_summary_format(lib.apkg_update_summary_get(summary),
        field.encode() if field is not None else None, C.byref(out), None)
    text = C.string_at(out).decode() if out else None
    lib.apkg_client_text_free(out)
    return code, text


def query(root, endpoint, field=None):
    out = C.c_void_p()
    code = lib.apkg_client_outdated(str(root).encode(), str(endpoint).encode(), CHANNEL.encode(),
        field.encode() if field is not None else None, None, C.byref(out), None)
    text = C.string_at(out).decode() if out else None
    lib.apkg_client_text_free(out)
    return code, text


def fixture(d):
    root, endpoint = d / 'root', d / 'channel'; state(root, policy=seed())
    code, ctx = open_ctx(root, 1); assert code == 0
    try:
        for name in ('demo', 'multi', 'blocked', 'opaque', 'fork', 'missing', 'owned'):
            install(ctx, desc(name, 'opaque' if name == 'opaque' else '1.0'))
    finally: lib.apkg_close(ctx)
    pk, sk = keypair(b'\x80'*32)
    items = []
    for name, version, untrusted in [('demo','1.0',False),('demo','2.0',False),('demo','3.0',False),
            ('multi','2.0',False),('multi','3.0',True),('blocked','2.0',True)]:
        data = desc(name, version); items.append((data, sign(data, pk, sk) if untrusted else sign(data)))
    for data in [desc('opaque','opaque',2), desc('fork',content=b'fork'),
            desc('owned','2.0',authority='ef'*16), desc('owned','2.0',target='arm-aros-v11-native')]:
        items.append((data, sign(data)))
    emit(endpoint, items)
    return root, endpoint


class Summary(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fail_alloc(0)

    def test_package_counts_readiness_and_actual_native_owned_copy_match_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, endpoint = fixture(Path(tmp)); before = (root/'.apkg/state').read_bytes()
            code, text = query(root, endpoint); self.assertEqual(code, 0)
            values = dict(line.split(' ',1) for line in text.splitlines())
            expected = dict(installed='7', newer='3', metadataeligible='2', blocked='1',
                newercandidates='5', blockedcandidates='2', indeterminate='1', conflicts='3', notlisted='2',
                ready='unknown', updated='unknown', stale='unknown', freshness='unknown', planreadiness='not-checked',
                review='not-asserted', runtime='not-asserted', packages='blocked demo multi')
            for key, value in expected.items(): self.assertEqual(values[key], value, key)
            self.assertEqual(values['snapshot'], hashlib.sha256(before).hexdigest())
            result = subprocess.run([str(CLI),'outdated',str(root),str(endpoint),CHANNEL],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr); self.assertEqual(result.stdout,text+'\n')
            for mode in ('17','19'):
                result = subprocess.run([str(WORKER),str(root),str(endpoint),mode,'7',CHANNEL],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr); self.assertEqual(result.stdout,'status 0\n'+text+'\n')
            self.assertEqual(query(root,endpoint,'READY'),(0,'unknown'))
            self.assertEqual(query(root,endpoint,'wrong-field'),(1,None))
            self.assertEqual((root/'.apkg/state').read_bytes(),before)
            self.assertFalse((endpoint/'archives').exists())

    def test_empty_source_and_source_without_coverage_do_not_assert_withdrawal(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root=d/'root'; endpoint=d/'channel'; state(root,policy=seed()); emit(endpoint,[])
            self.assertEqual(query(root,endpoint,'packages'),(0,''))
            self.assertEqual(query(root,endpoint,'newer'),(0,'0'))
            code,ctx=open_ctx(root,1); install(ctx,desc()); lib.apkg_close(ctx)
            self.assertEqual(query(root,endpoint,'notlisted'),(0,'1'))
            self.assertEqual(query(root,endpoint,'ready'),(0,'unknown'))
            self.assertEqual(query(root,endpoint,'freshness'),(0,'unknown'))

    def test_qualified_identities_count_separately_but_unqualified_names_refuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root=d/'root'; endpoint=d/'channel'
            policy_body=json.loads(seed())['body']; policy_body['authorities'].append(dict(id='ef'*16,keys=[dict(algorithm='ed25519',public_key=PK.hex())]))
            records=[]
            for authority in (AUTH,'ef'*16):
                data=desc(authority=authority); records.append(dict(descriptor=json.loads(data),signature=json.loads(sign(data))))
            state(root,policy=wire('policy',policy_body),installed=records)
            data=desc(version='2.0'); emit(endpoint,[(data,sign(data))])
            self.assertEqual(query(root,endpoint,'installed'),(0,'2'))
            self.assertEqual(query(root,endpoint,'newer'),(0,'1'))
            self.assertEqual(query(root,endpoint,'qualifiedpackages'),(0,AUTH+'/demo/'+TARGET))
            self.assertEqual(query(root,endpoint,'packages'),(14,None))
            self.assertEqual(query(root,endpoint),(14,None))
            result=subprocess.run([str(WORKER),str(root),str(endpoint),'17','7',CHANNEL,'qualifiedpackages'],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(result.stdout,'status 0\n'+AUTH+'/demo/'+TARGET+'\n')
            body=json.loads((root/'.apkg/state').read_bytes())['body']; body['installed'].append(records[0])
            (root/'.apkg/state').write_bytes(wire('root',body))
            code,s,ch=refresh(root,endpoint); self.assertEqual(code,0)
            code,summary=summarize(s,ch); self.assertEqual(code,14); self.assertFalse(summary)
            lib.apkg_snapshot_free(s); lib.apkg_channel_free(ch)

    def test_cached_native_query_reevaluates_operations_policy_target_and_replay_without_network(self):
        cases = [('generation','newer','status 0\n3\n'),
                 ('remove','newer','status 0\n2\n'),
                 ('author','metadataeligible','status 0\n0\n'),
                 ('channel','newer','status 12\n'),
                 ('target','metadataeligible','status 0\n0\n'),
                 ('floor','newer','status 8\n'),
                 ('fork','newer','status 14\n'),
                 ('pending','newer','status 9\n')]
        for change,field,expected in cases:
            with self.subTest(change=change),tempfile.TemporaryDirectory() as tmp:
                root,endpoint=fixture(Path(tmp))
                with serve(endpoint) as (url,server):
                    process=subprocess.Popen([str(WORKER),str(root),url,'18','7',CHANNEL,field],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                    try:
                        self.assertEqual(process.stdout.readline(),'loaded\n')
                        requests=len(server.requests)
                        if change=='pending': Path(str(root)+'.apkg-transaction').mkdir()
                        elif change in ('remove','floor','fork'):
                            code,ctx=open_ctx(root,1); self.assertEqual(code,0)
                            try:
                                if change=='remove':
                                    code,plan=op(ctx,'remove','blocked'); self.assertEqual(code,0)
                                    self.assertEqual(run_plan(ctx,plan),(0,1)); lib.apkg_plan_free(plan)
                                else: self.assertEqual(accept(ctx,catalog(entries=[],sequence=2 if change=='floor' else 1)),(0,1))
                            finally: lib.apkg_close(ctx)
                        else:
                            body=json.loads((root/'.apkg/state').read_bytes())['body']
                            body['generation']=str(int(body['generation'])+1)
                            if change=='author': body['policy']['body']['authorities']=[]
                            if change=='channel': body['policy']['body']['channels']=[]
                            if change=='target': body['target']='arm-aros-v11-native'
                            (root/'.apkg/state').write_bytes(wire('root',body))
                        before=(root/'.apkg/state').read_bytes()
                        stdout,stderr=process.communicate('\n',timeout=30)
                        self.assertEqual(process.returncode,0,stdout+stderr)
                        self.assertEqual(stdout,expected)
                        self.assertEqual(len(server.requests),requests)
                        self.assertEqual((root/'.apkg/state').read_bytes(),before)
                    finally:
                        if process.poll() is None: process.kill(); process.communicate()

    def test_signed_observation_export_is_exact_owned_and_reverified(self):
        from test_channel import read_channel
        lib.apkg_channel_observation.argtypes=[C.c_void_p,C.POINTER(Text),C.POINTER(Text)]
        lib.apkg_channel_observation.restype=C.c_int
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'root'; before=state(root,policy=seed()); code,ctx=open_ctx(root)
            data=catalog(); signature=sign(data,CP,CS,'channel')
            code,ch=read_channel(ctx,data); self.assertEqual(code,0)
            obj,sig=Text(),Text()
            self.assertEqual(lib.apkg_channel_observation(ch,C.byref(obj),C.byref(sig)),0)
            self.assertEqual(raw(obj),data); self.assertEqual(raw(sig),signature)
            copied=raw(obj),raw(sig); lib.apkg_channel_free(ch)
            # Original handle is gone; the copied signed pair is independently usable.
            identity=ID(); identity.bytes[:]=bytes.fromhex(CHANNEL)
            def reread(d,s):
                out=C.c_void_p()
                code=lib.apkg_channel_read(ctx,C.byref(identity),0,
                    Text(C.cast(C.c_char_p(d),C.c_void_p),len(d)),
                    Text(C.cast(C.c_char_p(s),C.c_void_p),len(s)),C.byref(out),None)
                if code: self.assertFalse(out)
                lib.apkg_channel_free(out); return code
            self.assertEqual(reread(*copied),0)
            self.assertNotEqual(reread(data.replace(b'1.0',b'2.0'),copied[1]),0)
            envelope=json.loads(copied[1]); envelope['signature']='00'*64
            self.assertNotEqual(reread(data,json.dumps(envelope,sort_keys=True,separators=(',',':')).encode()),0)
            obj=Text(C.c_void_p(1),1); sig=Text(C.c_void_p(1),1)
            self.assertEqual(lib.apkg_channel_observation(None,C.byref(obj),C.byref(sig)),1)
            self.assertFalse(obj.data); self.assertFalse(sig.data)
            lib.apkg_close(ctx); self.assertEqual((root/'.apkg/state').read_bytes(),before)

    def test_large_native_answers_refuse_without_truncation_and_scalar_fields_work(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root=d/'root'; endpoint=d/'channel'; records=[]; items=[]
            # Synthetic committed metadata: this reader makes no live-file claim.
            for i in range(320):
                name=('package-%04d-'%i)+'x'*242
                old=wire('descriptor',dict(authority=AUTH,name=name,version='1.0',revision='1',target=TARGET,kind='data',files=[]))
                new=wire('descriptor',dict(authority=AUTH,name=name,version='2.0',revision='1',target=TARGET,kind='data',files=[]))
                records.append(dict(descriptor=json.loads(old),signature=json.loads(sign(old)))); items.append((new,sign(new)))
            state(root,policy=seed(),installed=records); emit(endpoint,items)
            code,text=query(root,endpoint); self.assertEqual(code,0); self.assertGreater(len(text),65535)
            for field,expected in [(None,'status 3\n'),('newer','status 0\n320\n')]:
                args=[str(WORKER),str(root),str(endpoint),'17','7',CHANNEL]+([field] if field else [])
                result=subprocess.run(args,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr); self.assertEqual(result.stdout,expected)

    def test_owned_lifetime_allocation_failures_retry_and_readonly_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            root,endpoint=fixture(Path(tmp)); before=(root/'.apkg/state').read_bytes()
            code,s,ch=refresh(root,endpoint); self.assertEqual(code,0)
            for nth in range(1,101):
                lib.apkg_test_fail_alloc(nth); code,summary=summarize(s,ch); lib.apkg_test_fail_alloc(0)
                self.assertIn(code,(0,4),nth)
                if code: self.assertFalse(summary)
                lib.apkg_update_summary_free(summary)
            code,summary=summarize(s,ch); self.assertEqual(code,0)
            lib.apkg_snapshot_free(s); lib.apkg_channel_free(ch)
            self.assertEqual(formatted(summary,'newer'),(0,'3'))
            for nth in range(1,12):
                lib.apkg_test_fail_alloc(nth); code,text=formatted(summary); lib.apkg_test_fail_alloc(0)
                self.assertIn(code,(0,4));
                if code: self.assertIsNone(text)
            lib.apkg_update_summary_free(summary)
            for nth in range(1,301):
                lib.apkg_test_fail_alloc(nth); code,text=query(root,endpoint); lib.apkg_test_fail_alloc(0)
                self.assertIn(code,(0,4),nth)
                if code: self.assertIsNone(text)
            self.assertEqual(query(root,endpoint,'newer'),(0,'3'))
            self.assertEqual((root/'.apkg/state').read_bytes(),before)
