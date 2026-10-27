# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import copy
import ctypes as C
import os
import shutil
import subprocess
import tempfile
import unittest
from support import *
from test_client import CLI, WORKER
import test_client as clients
from test_channel import CHANNEL, CP, CS, catalog, entry, read_channel
from test_attestation import claim, trust, RP, RS, ISSUER
from test_channel_attestation import observation
from test_channel_heads import accepting
from test_updates import desc
from test_operations import install, op, run_plan
from test_http_channel import serve

ALT='dd'*16

def policy_with_mirror():
    body=json.loads(trust())['body']; body['channels'].append(dict(id=ALT,keys=[dict(algorithm='ed25519',public_key=CP.hex())])); return wire('policy',body)
def statements(data):
    first=claim(data); second=claim(data,sequence='2',supersedes=hashlib.sha256(first).hexdigest(),result='revoked',issued_at='1234567891')
    return [observation(first),observation(second)]
def document(data,records,identity=CHANNEL,sequence=1):
    item=entry(data,sign(data)); item['attestations']=sorted(records,key=lambda x:hashlib.sha256(wire('attestation',x['record']['body'])).digest())
    return catalog([item],identity=identity,sequence=sequence)
def emit(endpoint,data):
    digest=hashlib.sha256(data).hexdigest(); folder=endpoint/'snapshots'/digest; folder.mkdir(parents=True,exist_ok=True)
    (folder/'channel.json').write_bytes(data); (folder/'channel.signature.json').write_bytes(sign(data,CP,CS,'channel')); (endpoint/'current').write_text(digest+'\n')
def run(*args): return subprocess.run([str(CLI),*map(str,args)],capture_output=True,text=True,timeout=40)
def body(root): return json.loads((root/'.apkg/state').read_bytes())['body']
def memory(root): return body(root).get('qualification_records',[])
def fixture(d):
    data=desc(); records=statements(data); a,b=d/'a',d/'b'; emit(a,document(data,records)); emit(b,document(data,records[:1],ALT))
    root=d/'root'; state(root,policy=policy_with_mirror()); return root,a,b,data,records

def accept_root(root,endpoint,identity=CHANNEL): return clients.OperationClients().commit('channel-accept',root,endpoint,identity)
def inspect(root,endpoint,identity=ALT):
    result=run('channel-open',root,endpoint,identity); assert result.returncode==0,result.stdout+result.stderr
    return json.loads(result.stdout)['entries'][0]

class QualificationMemory(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fail_alloc(0); lib.apkg_test_fault(0,0)

    def test_acceptance_preserves_revocation_across_omitting_channels_http_and_old_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,a,b,data,records=fixture(d); before=(root/'.apkg/state').read_bytes(); profile=d/'old-profile'
            self.assertEqual(run('source-cache',root,profile,b,ALT).returncode,0); self.assertEqual((root/'.apkg/state').read_bytes(),before)
            old=inspect(root,b); self.assertEqual(len(old['attestations']),1); self.assertIsNone(old['attestations'][0]['accepted_sequence'])
            preview=run('channel-accept',root,a,CHANNEL); self.assertEqual(preview.returncode,16,preview.stdout+preview.stderr); self.assertEqual((root/'.apkg/state').read_bytes(),before)
            accepted=accept_root(root,a); self.assertIn('result: revoked; sequence: 2',accepted.stdout); self.assertEqual(len(memory(root)),2)
            self.assertEqual(body(root)['package_generation'],'1'); self.assertEqual(body(root)['installed'],[])
            accepted_bytes=(root/'.apkg/state').read_bytes()
            with serve(b) as (url,server):
                current=inspect(root,url); self.assertEqual(current['source_attestation_count'],'1')
                self.assertEqual(len(current['attestations']),2)
                byseq={r['record']['body']['sequence']:r for r in current['attestations']}
                self.assertEqual(byseq['1']['observation_origin'],'source-and-retained'); self.assertTrue(byseq['1']['older_than_accepted'])
                self.assertEqual(byseq['2']['record']['body']['result'],'revoked'); self.assertEqual(byseq['2']['observation_origin'],'retained')
                for r in byseq.values(): self.assertEqual(r['accepted_sequence'],'2'); self.assertEqual(r['freshness'],'unknown')
                native=subprocess.run([str(WORKER),str(root),url,'30','7',ALT],capture_output=True,text=True,timeout=30)
                self.assertEqual(native.returncode,0,native.stdout+native.stderr); self.assertIn('row demo 2 0 1\n',native.stdout)
                requests=len(server.requests)
                cached=subprocess.run([str(WORKER),str(root),str(profile),'31'],capture_output=True,text=True,timeout=30)
                self.assertEqual(cached.returncode,0,cached.stdout+cached.stderr); self.assertIn('origin retained\n',cached.stdout); self.assertIn('acceptedsequence 2\n',cached.stdout)
                self.assertEqual(len(server.requests),requests); self.assertFalse(any('/archives/' in p for p in server.requests))
            emit(b,document(data,[],ALT,2)); omitted=inspect(root,b)
            self.assertEqual(omitted['source_attestation_count'],'0'); self.assertEqual(len(omitted['attestations']),2)
            self.assertTrue(all(r['observation_origin']=='retained' for r in omitted['attestations']))
            self.assertEqual((root/'.apkg/state').read_bytes(),accepted_bytes)

    def test_cli_worker_acceptance_equivalence_decline_and_untrusted_key_cannot_advance_memory(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,a,b,data,records=fixture(d); other=d/'worker'; state(other,policy=policy_with_mirror()); before=(other/'.apkg/state').read_bytes()
            declined=subprocess.run([str(WORKER),str(other),str(a),'0','8',CHANNEL],capture_output=True,text=True,timeout=30)
            self.assertEqual(declined.returncode,0,declined.stdout+declined.stderr); self.assertEqual((other/'.apkg/state').read_bytes(),before)
            accept_root(root,a); clients.OperationClients().worker(other,a,8,CHANNEL)
            self.assertEqual((root/'.apkg/state').read_bytes(),(other/'.apkg/state').read_bytes())
            saved=copy.deepcopy(memory(root)); pk,sk=keypair(b'\x63'*32)
            fake=claim(data,sequence='1000',supersedes='ab'*32,summary='unauthorized high sequence')
            emit(b,document(data,[observation(fake,pk,sk)],ALT,2))
            row=inspect(root,b); rogue=next(r for r in row['attestations'] if r['record']['body']['sequence']=='1000')
            self.assertEqual(rogue['issuer_trust'],'rejected'); self.assertEqual(rogue['accepted_sequence'],'2')
            accept_root(root,b,ALT); self.assertEqual(memory(root),saved)
            # Withdrawing roles changes current facts, while accepted evidence survives.
            current=body(root); current['policy']['body']['reviewers']=[]; (root/'.apkg/state').write_bytes(wire('root',current))
            row=inspect(root,b); self.assertTrue(all(r['issuer_trust']=='rejected' for r in row['attestations'])); self.assertEqual(memory(root),saved)

    def test_install_operations_and_offline_package_rollback_never_undo_accepted_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,a,b,data,records=fixture(d); code,ctx=open_ctx(root,1); self.assertEqual(code,0)
            install(ctx,data)
            installed=(root/'.apkg/state').read_bytes()
            history=next(p for p in root.parent.glob(root.name+'.apkg-history.*') if (p/'new-state').read_bytes()==installed)
            # Metadata acceptance must not depend on installed publisher trust
            # or rewrite edited payloads.
            current=body(root); current['policy']['body']['authorities']=[]
            (root/'.apkg/state').write_bytes(wire('root',current))
            (root/'demo/hello').write_bytes(b'personal edit')
            accept_root(root,a); saved=copy.deepcopy(memory(root)); self.assertEqual(len(saved),2)
            self.assertEqual((root/'demo/hello').read_bytes(),b'personal edit')
            current=body(root); current['policy']['body']['authorities']=json.loads(policy_with_mirror())['body']['authorities']
            (root/'.apkg/state').write_bytes(wire('root',current)); (root/'demo/hello').write_bytes(b'hello\n')
            shutil.rmtree(a)
            code,p=op(ctx,'rollback',str(history)); self.assertEqual(code,0); self.assertEqual(run_plan(ctx,p),(0,1)); lib.apkg_plan_free(p)
            self.assertEqual(body(root)['installed'],[]); self.assertEqual(memory(root),saved)
            install(ctx,data); self.assertEqual(memory(root),saved)
            for operation in ('repair','remove'):
                code,p=op(ctx,operation); self.assertEqual(code,0); self.assertEqual(run_plan(ctx,p),(0,1)); lib.apkg_plan_free(p); self.assertEqual(memory(root),saved)
            self.assertEqual(len(inspect(root,b)['attestations']),2); lib.apkg_close(ctx)

    def test_selected_channel_install_records_observations_in_the_same_cli_worker_transaction(self):
        import test_channel_attestation as carriage
        from test_file_channel import artifact, channel_key
        from test_author import members
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); package=artifact(d); data=members(package)['descriptor.json']; records=statements(data)
            endpoint=d/'published'; paths=carriage.folders(d,records)
            published=carriage.publish(package,endpoint,channel_key(d/'channel-key'),*paths)
            self.assertEqual(published.returncode,0,published.stdout+published.stderr)
            a,b=d/'cli-root',d/'worker-root'
            for root in (a,b): state(root,policy=policy_with_mirror())
            found=run('channel-open',a,endpoint,CHANNEL); self.assertEqual(found.returncode,0,found.stdout+found.stderr)
            source=json.loads(found.stdout); selection=CHANNEL+'/'+source['snapshot_sha256']+'/'+source['entries'][0]['descriptor_sha256']
            clients.OperationClients().commit('channel-install',a,endpoint,*selection.split('/'))
            clients.OperationClients().worker(b,endpoint,7,selection)
            self.assertEqual((a/'.apkg/state').read_bytes(),(b/'.apkg/state').read_bytes())
            for root in (a,b):
                self.assertEqual(len(memory(root)),2); self.assertEqual(len(body(root)['installed']),1)
                self.assertEqual(body(root)['channel_heads'][0]['sequence'],'1')
                self.assertTrue((root/'demo/demo-drawer/program').exists())

    def test_interrupted_acceptance_and_interrupted_offline_recovery_are_atomic(self):
        for point in (2,4,5):
            with self.subTest(point=point),tempfile.TemporaryDirectory() as tmp:
                d=Path(tmp); root,a,b,data,records=fixture(d); before=(root/'.apkg/state').read_bytes(); code,ctx=open_ctx(root,1)
                code,ch=read_channel(ctx,document(data,records)); self.assertEqual(code,0); code,p=accepting(ctx,ch); self.assertEqual(code,0); code,approval=approve(ctx,p); self.assertEqual(code,0)
                pid=os.fork()
                if pid==0: lib.apkg_test_fault(point,2); execute(ctx,p,approval); os._exit(98)
                _,status=os.waitpid(pid,0); self.assertEqual(os.waitstatus_to_exitcode(status),99)
                shutil.rmtree(a)
                if point>=4:
                    pid=os.fork()
                    if pid==0: lib.apkg_test_fault(6,2); recover(ctx); os._exit(98)
                    _,status=os.waitpid(pid,0); self.assertEqual(os.waitstatus_to_exitcode(status),99)
                self.assertEqual(recover(ctx),(0,2 if point==2 else 1)); self.assertEqual(recover(ctx),(0,0))
                if point==2: self.assertEqual((root/'.apkg/state').read_bytes(),before)
                else:
                    self.assertEqual(len(memory(root)),2); self.assertEqual(len(body(root)['channel_heads']),1)
                    self.assertEqual(len(inspect(root,b)['attestations']),2)
                lib.apkg_approval_free(approval); lib.apkg_plan_free(p); lib.apkg_channel_free(ch); lib.apkg_close(ctx)

    def test_forks_signature_replacement_and_cross_content_do_not_replace_memory(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,a,b,data,records=fixture(d); accept_root(root,a); before=(root/'.apkg/state').read_bytes()
            first=wire('attestation',records[0]['record']['body'])
            fork=claim(data,sequence='2',supersedes=hashlib.sha256(first).hexdigest(),result='failed',summary='fork')
            emit(b,document(data,[observation(fork)],ALT,2)); refused=run('channel-open',root,b,ALT); self.assertEqual(refused.returncode,14,refused.stdout+refused.stderr)
            pk,sk=keypair(b'\x42'*32); emit(b,document(data,[observation(first,pk,sk)],ALT,3)); self.assertEqual(run('channel-open',root,b,ALT).returncode,14)
            different=desc(version='2.0'); emit(b,document(different,[],ALT,4)); self.assertEqual(inspect(root,b)['attestations'],[])
            self.assertEqual((root/'.apkg/state').read_bytes(),before)
            corrupted=body(root); corrupted['qualification_records'][0]['signature']['signature']='00'*64
            (root/'.apkg/state').write_bytes(wire('root',corrupted))
            self.assertNotEqual(run('channel-open',root,b,ALT).returncode,0)

    def test_one_context_reverifies_retained_records_whose_bytes_change(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,a,b,data,records=fixture(d); accept_root(root,a); self.assertTrue(memory(root))
            code,c=open_ctx(root); self.assertEqual(code,0)
            for _ in range(2):
                code,s=snapshot(c); self.assertEqual(code,0); lib.apkg_snapshot_free(s)
            corrupted=body(root); corrupted['qualification_records'][0]['signature']['signature']='00'*64
            (root/'.apkg/state').write_bytes(wire('root',corrupted))
            code,s=snapshot(c); self.assertNotEqual(code,0); self.assertFalse(s)
            lib.apkg_close(c)

    def test_memory_reads_and_acceptance_allocation_failures_clear_outputs_and_preserve_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,a,b,data,records=fixture(d); accept_root(root,a); before=(root/'.apkg/state').read_bytes(); code,ctx=open_ctx(root,1)
            code,s=snapshot(ctx); ready=lib.apkg_snapshot_root(s).contents.state; lib.apkg_snapshot_free(s)
            for nth in range(1,180):
                lib.apkg_test_fail_alloc(nth); code,s=snapshot(ctx); lib.apkg_test_fail_alloc(0); self.assertIn(code,(0,4),nth)
                if code: self.assertFalse(s)
                else: self.assertEqual(lib.apkg_snapshot_root(s).contents.state,ready)
                lib.apkg_snapshot_free(s)
            install(ctx,data); before=(root/'.apkg/state').read_bytes()
            history=next(p for p in root.parent.glob(root.name+'.apkg-history.*') if (p/'new-state').read_bytes()==before)
            code,ch=read_channel(ctx,document(data,records)); self.assertEqual(code,0)
            code,s=snapshot(ctx); self.assertEqual(code,0)
            for nth in range(1,300):
                p=C.c_void_p(); lib.apkg_test_fail_alloc(nth)
                code=lib.apkg_plan_channel_accept(ctx,s,ch,C.byref(p),None)
                lib.apkg_test_fail_alloc(0); self.assertIn(code,(0,4),nth)
                if code: self.assertFalse(p)
                lib.apkg_plan_free(p)
            path=str(history).encode()
            for nth in range(1,180):
                p=C.c_void_p(); lib.apkg_test_fail_alloc(nth)
                code=lib.apkg_plan_rollback(ctx,s,Text(C.cast(C.c_char_p(path),C.c_void_p),len(path)),C.byref(p),None)
                lib.apkg_test_fail_alloc(0); self.assertIn(code,(0,4),nth)
                if code: self.assertFalse(p)
                lib.apkg_plan_free(p)
            lib.apkg_snapshot_free(s)
            lib.apkg_channel_free(ch); lib.apkg_close(ctx); self.assertEqual((root/'.apkg/state').read_bytes(),before)
