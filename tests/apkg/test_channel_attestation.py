# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import copy
import json
import subprocess
import tempfile
import unittest
from support import *
from test_client import CLI, WORKER
from test_channel import CHANNEL, CP, CS, entry, catalog, read_channel
from test_file_channel import artifact, channel_key, command, head
from test_author import members, author_fixture, key_fixture, build
from test_attestation import claim, trust, RP, RS, ISSUER
from test_http_channel import serve
from test_channel_heads import accept
from test_updates import desc

lib.apkg_channel_attestation_at.argtypes=[C.c_void_p,C.c_uint64,C.c_uint64,C.POINTER(C.c_void_p),C.c_void_p]
lib.apkg_channel_attestation_at.restype=C.c_int
lib.apkg_channel_attestations_format.argtypes=[C.c_void_p,C.c_uint64,C.POINTER(C.c_void_p),C.c_void_p]
lib.apkg_channel_attestations_format.restype=C.c_int

def observation(record,pk=RP,sk=RS): return dict(record=json.loads(record),signature=json.loads(sign(record,pk,sk,'attestation')))
def channel(data,records):
    item=entry(data,sign(data)); item['attestations']=sorted(records,key=lambda x:hashlib.sha256(wire('attestation',x['record']['body'])).digest())
    return catalog([item])
def invoke(*args): return subprocess.run([str(CLI),*map(str,args)],capture_output=True,text=True,timeout=30)
def publish(package,endpoint,key,*folders):
    args=command(package,endpoint,key)
    for folder in folders: args+=['--attestation',str(folder)]
    return subprocess.run(args,capture_output=True,text=True,timeout=60)
def reports(data):
    first=claim(data); revoked=claim(data,result='revoked',sequence='2',supersedes=hashlib.sha256(first).hexdigest(),issued_at='1234567891')
    return [observation(first),observation(revoked),observation(claim(data,kind='runtime')),observation(claim(data,issuer=AUTH),PK,SK)]
def folders(d,records):
    result=[]
    for i,r in enumerate(records):
        folder=d/('claim-%d'%i); folder.mkdir(); (folder/'attestation.json').write_bytes(wire('attestation',r['record']['body']))
        (folder/'attestation.signature.json').write_bytes(json.dumps(r['signature'],sort_keys=True,separators=(',',':')).encode()); result.append(folder)
    return result

class ChannelAttestations(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fail_alloc(0)

    def test_real_publication_merge_http_cli_worker_and_offline_profile_preserve_signed_history(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); package=artifact(d); data=members(package)['descriptor.json']; records=reports(data)
            key=channel_key(d/'channel-key'); endpoint=d/'channel'; root=d/'root'; before=state(root,policy=trust())
            initial=publish(package,endpoint,key); self.assertEqual(initial.returncode,0,initial.stderr); old=head(endpoint)
            paths=folders(d,records); result=publish(package,endpoint,key,*paths); self.assertEqual(result.returncode,0,result.stderr)
            answer=json.loads(result.stdout); self.assertEqual(answer['sequence'],'2'); self.assertTrue(answer['changed']); self.assertNotEqual(head(endpoint),old)
            self.assertTrue((endpoint/'snapshots'/old/'channel.json').exists())
            for supplied in (paths,[]):
                again=publish(package,endpoint,key,*supplied); self.assertEqual(again.returncode,0,again.stderr); self.assertFalse(json.loads(again.stdout)['changed']); self.assertEqual(head(endpoint),answer['snapshot'])
            profile=d/'profile'
            with serve(endpoint) as (url,server):
                result=invoke('channel-open',root,url,CHANNEL); self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                row=json.loads(result.stdout)['entries'][0]; self.assertEqual(len(row['attestations']),4)
                bykind={}
                for r in row['attestations']:
                    body=r['record']['body']; bykind.setdefault(body['kind'],[]).append(r)
                    self.assertEqual(r['freshness'],'unknown'); self.assertEqual(r['history'],'linked-prefix')
                    self.assertEqual(r['root_snapshot_sha256'],hashlib.sha256(before).hexdigest())
                    self.assertEqual(r['issuer_trust'],'rejected' if body['issuer']==AUTH else 'accepted')
                    self.assertEqual(r['independent'],body['issuer']!=AUTH)
                self.assertEqual([r['record']['body']['result'] for r in bykind['runtime']],['passed'])
                self.assertEqual(row['review'],'not-asserted'); self.assertEqual(row['runtime_tests'],'not-asserted')
                updates=invoke('channel-updates',root,url,CHANNEL); self.assertEqual(updates.returncode,0,updates.stdout+updates.stderr)
                self.assertEqual(json.loads(updates.stdout)['entries'][0]['attestations'],row['attestations'])
                native=subprocess.run([str(WORKER),str(root),url,'30','7',CHANNEL],capture_output=True,text=True,timeout=30)
                self.assertEqual(native.returncode,0,native.stdout+native.stderr); self.assertIn('row demo 3 1 1\n',native.stdout)
                self.assertEqual(native.stdout.count('freshness unknown\n'),4); self.assertIn('result revoked\n',native.stdout)
                for r in row['attestations']: self.assertIn(r['record_sha256'],native.stdout)
                saved=invoke('source-cache',root,profile,url,CHANNEL); self.assertEqual(saved.returncode,0,saved.stdout+saved.stderr)
                request_count=len(server.requests)
                cached=subprocess.run([str(WORKER),str(root),str(profile),'31'],capture_output=True,text=True,timeout=30)
                self.assertEqual(cached.returncode,0,cached.stdout+cached.stderr); self.assertEqual(cached.stdout,native.stdout)
                self.assertEqual(len(server.requests),request_count); self.assertFalse(any('/archives/' in p for p in server.requests))
            cached=subprocess.run([str(WORKER),str(root),str(profile),'31'],capture_output=True,text=True,timeout=30)
            self.assertEqual(cached.returncode,0,cached.stdout+cached.stderr); self.assertEqual(cached.stdout,native.stdout)
            # Current role withdrawal is re-evaluated; historical signed bytes stay.
            body=json.loads(before)['body']; body['policy']['body']['reviewers']=[]; body['policy']['body']['testers']=[]
            (root/'.apkg/state').write_bytes(wire('root',body)); saved=profile.read_bytes()
            cached=subprocess.run([str(WORKER),str(root),str(profile),'31'],capture_output=True,text=True,timeout=30)
            self.assertEqual(cached.returncode,0,cached.stdout+cached.stderr); self.assertEqual(cached.stdout.count('issuertrust rejected\n'),4)
            self.assertIn('row demo 3 1 1\n',cached.stdout); self.assertEqual(profile.read_bytes(),saved)

    def test_invalid_signatures_binding_order_forks_and_history_scope_refuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'root'; before=state(root,policy=trust()); code,ctx=open_ctx(root); self.assertEqual(code,0); data=desc()
            first=claim(data); first_obj=observation(first)
            def check(records,expected):
                code,ch=read_channel(ctx,channel(data,records)); self.assertEqual(code,expected)
                self.assertFalse(ch) if code else lib.apkg_channel_free(ch)
            check(reports(data),0)
            check([observation(claim(data,sequence='2',supersedes=hashlib.sha256(first).hexdigest()))],0)
            check([first_obj,observation(claim(data,summary='fork'))],14)
            check([first_obj,observation(claim(data,sequence='2',supersedes='bb'*32))],14)
            check([observation(claim(desc(content=b'other')))],10)
            bad=copy.deepcopy(first_obj); bad['signature']['signature']='00'*64; check([bad],11)
            check([first_obj,first_obj],14)
            runtime=claim(data,kind='runtime'); env=json.loads(runtime)['body']['environment']; env['os_build']='other'
            check([observation(runtime),observation(claim(data,kind='runtime',environment=env,sequence='2',supersedes=hashlib.sha256(runtime).hexdigest()))],14)
            item=entry(data,sign(data)); ordered=sorted(reports(data),key=lambda x:hashlib.sha256(wire('attestation',x['record']['body'])).digest()); item['attestations']=list(reversed(ordered))
            self.assertEqual(read_channel(ctx,catalog([item]))[0],14)
            item['attestations']=[dict(record=json.loads(first),signature=first_obj['signature'],extra='ignored')]; self.assertEqual(read_channel(ctx,catalog([item]))[0],2)
            item['attestations']=None; self.assertEqual(read_channel(ctx,catalog([item]))[0],2)
            lib.apkg_close(ctx); self.assertEqual((root/'.apkg/state').read_bytes(),before)

    def test_publication_refusals_keep_current_head_and_never_require_a_curator(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); package=artifact(d); data=members(package)['descriptor.json']; key=channel_key(d/'channel-key'); endpoint=d/'channel'
            self.assertEqual(publish(package,endpoint,key).returncode,0); before=head(endpoint)
            fork=folders(d,[observation(claim(data)),observation(claim(data,summary='fork'))])
            result=publish(package,endpoint,key,*fork); self.assertNotEqual(result.returncode,0); self.assertEqual(head(endpoint),before)
            absent=d/'absent'; result=publish(package,absent,key,*fork); self.assertNotEqual(result.returncode,0); self.assertFalse(absent.exists())
            # An author report is retained as such; publisher rights remain independent.
            author=d/'author'; author.mkdir(); author_records=folders(author,[observation(claim(data,issuer=AUTH),PK,SK)])
            result=publish(package,endpoint,key,*author_records); self.assertEqual(result.returncode,0,result.stderr)
            root=d/'root'; state(root,policy=trust()); result=invoke('channel-open',root,endpoint,CHANNEL)
            obs=json.loads(result.stdout)['entries'][0]['attestations'][0]
            self.assertFalse(obs['independent']); self.assertEqual(obs['issuer_trust'],'rejected')
            from test_client import OperationClients
            OperationClients().commit('install',root,endpoint/'archives'/(json.loads(publish(package,endpoint,key).stdout)['archive_sha256']+'.pkg'))
            self.assertTrue((root/'demo').exists())
            self.assertEqual(publish(package,endpoint,key,fork[0]).returncode,0)
            retained=head(endpoint); replacement=d/'replacement'; replacement.mkdir()
            other_pk,other_sk=keypair(b'\x91'*32)
            alternate=folders(replacement,[observation(claim(data),other_pk,other_sk)])
            result=publish(package,endpoint,key,*alternate); self.assertNotEqual(result.returncode,0)
            self.assertEqual(head(endpoint),retained)


    def test_new_content_does_not_inherit_reports_by_name_version_or_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); package=artifact(d); data=members(package)['descriptor.json']; key=channel_key(d/'channel-key'); endpoint=d/'channel'
            paths=folders(d,[observation(claim(data))]); result=publish(package,endpoint,key,*paths); self.assertEqual(result.returncode,0,result.stderr)
            source=d/'new-drawer'; author_fixture(source,name='demo',version='2.0'); new=d/'new.pkg'; result=build(source,new,key_fixture(d/'new.author')); self.assertEqual(result.returncode,0,result.stderr)
            result=publish(new,endpoint,key); self.assertEqual(result.returncode,0,result.stderr)
            root=d/'root'; state(root,policy=trust()); result=invoke('channel-open',root,endpoint,CHANNEL); self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            rows=json.loads(result.stdout)['entries']; self.assertEqual(len(rows),2)
            for row in rows: self.assertEqual(len(row['attestations']),0 if row['version']=='2.0' else 1)

    def test_bounds_owned_lifetime_and_allocation_failure_are_readonly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'root'; before=state(root,policy=trust()); code,ctx=open_ctx(root); data=desc()
            records=[observation(claim(data,issuer=('%032x'%i))) for i in range(1,66)]
            self.assertEqual(read_channel(ctx,channel(data,records))[0],3)
            code,ch=read_channel(ctx,channel(data,records[:64])); self.assertEqual(code,0)
            view=C.c_void_p(); self.assertEqual(lib.apkg_channel_attestation_at(ch,0,64,C.byref(view),None),5); self.assertFalse(view)
            lib.apkg_channel_free(ch)
            document=channel(data,reports(data))
            for nth in range(1,340):
                lib.apkg_test_fail_alloc(nth); code,ch=read_channel(ctx,document); lib.apkg_test_fail_alloc(0); self.assertIn(code,(0,4),nth)
                if code: self.assertFalse(ch)
                lib.apkg_channel_free(ch)
            code,ch=read_channel(ctx,document); self.assertEqual(code,0); lib.apkg_close(ctx)
            text=C.c_void_p(); self.assertEqual(lib.apkg_channel_attestations_format(ch,0,C.byref(text),None),0)
            result=C.string_at(text).decode(); lib.apkg_client_text_free(text)
            self.assertEqual(result.count('freshness unknown\n'),4); self.assertIn('result revoked\n',result)
            lib.apkg_channel_free(ch); self.assertEqual((root/'.apkg/state').read_bytes(),before)
