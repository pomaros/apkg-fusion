# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import copy
import subprocess
import tempfile
import unittest
from support import *
from test_client import CLI, WORKER
from test_channel import CHANNEL, seed
from test_updates import desc

ISSUER='ef'*16
RP,RS=keypair(b'\x51'*32)
lib.apkg_attestation_read.argtypes=[C.c_void_p,Text,Text,Text,Text,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p]
lib.apkg_attestation_read.restype=C.c_int
lib.apkg_attestation_format.argtypes=[C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p]
lib.apkg_attestation_format.restype=C.c_int
lib.apkg_attestation_free.argtypes=[C.c_void_p]

def grant(identity=ISSUER,key=RP): return dict(id=identity,keys=[dict(algorithm='ed25519',public_key=key.hex())])
def trust():
    body=json.loads(seed())['body']; body.update(reviewers=[grant()],testers=[grant()]); return wire('policy',body)

def claim(data=None,**changes):
    data=data or desc(); d=json.loads(data)['body']
    b=dict(authority=d['authority'],name=d['name'],version=d['version'],revision=d['revision'],target=d['target'],
        descriptor_sha256=hashlib.sha256(data).hexdigest(),issuer=ISSUER,kind='review',result='passed',sequence='1',issued_at='1234567890',
        supersedes=None,environment=None,summary='Checked exact bytes',evidence='https://example.invalid/evidence')
    b.update(changes)
    if b['kind']=='runtime' and 'environment' not in changes:
        b['environment']=dict(hardware='test board',os_build='exact os build',test_suite='suite/1',log_sha256='aa'*32)
    return wire('attestation',b)

def read(s,data,record,previous=None,pk=RP,sk=RS,signature=None,descriptor_signature=None):
    inputs=[data,descriptor_signature or sign(data),record,signature or sign(record,pk,sk,'attestation')]
    buffers=[C.create_string_buffer(x) for x in inputs]
    out=C.c_void_p(); code=lib.apkg_attestation_read(s,*[Text(C.cast(b,C.c_void_p),len(x)) for b,x in zip(buffers,inputs)],previous,C.byref(out),None)
    if code: assert not out
    return code,out

def formatted(a):
    out=C.c_void_p(); code=lib.apkg_attestation_format(a,C.byref(out),None)
    text=C.string_at(out).decode() if out else None; lib.apkg_client_text_free(out)
    return code,text

def fixture(d,record=None,data=None,policy=None):
    root=d/'root'; state(root,policy=policy or trust()); folder=d/'claim'; folder.mkdir()
    data=data or desc(); record=record or claim(data)
    for name,contents in [('descriptor.json',data),('signature.json',sign(data)),('attestation.json',record),('attestation.signature.json',sign(record,RP,RS,'attestation'))]:
        (folder/name).write_bytes(contents)
    return root,folder

class Attestations(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fail_alloc(0)

    def test_signing_cli_and_actual_worker_share_exact_claim_facts_without_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,folder=fixture(d); before=(root/'.apkg/state').read_bytes()
            key=d/'review.key'; key.write_text('Pkg-Secret-Key: 1\nSeed: '+(b'\x51'*32).hex()+'\nPublic: '+RP.hex()+'\n'); key.chmod(0o600)
            signed=subprocess.run([str(CLI),'sign-object',str(key),'attestation',str(folder/'attestation.json')],capture_output=True,text=True)
            self.assertEqual(signed.returncode,0,signed.stdout+signed.stderr)
            self.assertEqual(json.loads(signed.stdout),json.loads((folder/'attestation.signature.json').read_bytes()))
            verified=subprocess.run([str(CLI),'verify-object','attestation',str(folder/'attestation.json'),str(folder/'attestation.signature.json')],capture_output=True,text=True)
            self.assertEqual(verified.returncode,0,verified.stdout+verified.stderr)
            result=subprocess.run([str(CLI),'attestation-check',str(root),str(folder)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            fields=dict(x.split(' ',1) for x in result.stdout.strip().splitlines())
            for k,v in dict(kind='review',result='passed',issuertrust='accepted',authortrust='accepted',independent='yes',targetmatches='yes',history='linked-prefix',freshness='unknown',verification='signed-claim-only',environment='null').items(): self.assertEqual(fields[k],v,k)
            native=subprocess.run([str(WORKER),str(root),str(folder),'28'],capture_output=True,text=True)
            self.assertEqual(native.returncode,0,native.stdout+native.stderr); self.assertEqual(native.stdout,'status 0\n'+result.stdout)
            for fault,expected in [(-1,16),(1,4)]:
                native=subprocess.run([str(WORKER),str(root),str(folder),'28',str(fault)],capture_output=True,text=True)
                self.assertEqual(native.returncode,0,native.stdout+native.stderr); self.assertEqual(native.stdout,'status %d\n'%expected)
            self.assertEqual((root/'.apkg/state').read_bytes(),before); self.assertEqual(list(root.parent.glob('root.apkg-*')),[])

    def test_runtime_has_exact_environment_and_owned_lifetime_after_snapshot_and_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'root'; state(root,policy=trust()); code,ctx=open_ctx(root); code,s=snapshot(ctx)
            data=desc(name='a'*255,version='v'*255,revision=2**64-1)
            record=claim(data,kind='runtime',issued_at=str(2**64-1),summary='s'*1000,evidence='e'*4096)
            code,a=read(s,data,record); self.assertEqual(code,0)
            lib.apkg_snapshot_free(s); lib.apkg_close(ctx)
            code,text=formatted(a); self.assertEqual(code,0)
            fields=dict(x.split(' ',1) for x in text.splitlines())
            self.assertEqual(fields['kind'],'runtime'); self.assertEqual(fields['issuedat'],str(2**64-1)); self.assertEqual(fields['revision'],str(2**64-1))
            self.assertEqual(fields['name'],'a'*255); self.assertEqual(fields['version'],'v'*255)
            self.assertEqual(json.loads(fields['environment']),json.loads(record)['body']['environment'])
            self.assertEqual(fields['freshness'],'unknown'); lib.apkg_attestation_free(a)

    def test_roles_and_independence_never_follow_author_channel_or_issuer_names(self):
        data=desc()
        for case in ('untrusted','author','same-id','alias-key','author-second-key','review-only-runtime','withdrawn-author','other-target'):
            with self.subTest(case=case),tempfile.TemporaryDirectory() as tmp:
                body=json.loads(trust())['body']; pk,sk=RP,RS; issuer=ISSUER; kind='review'; target=TARGET
                if case=='untrusted': body.pop('reviewers'); body.pop('testers')
                if case=='author': pk,sk=PK,SK; issuer=AUTH
                if case=='same-id': issuer=AUTH; body['reviewers']=[grant(AUTH)]
                if case=='alias-key': pk,sk=PK,SK; body['reviewers']=[grant(ISSUER,PK)]
                if case=='author-second-key':
                    body['authorities'][0]['keys']=sorted(body['authorities'][0]['keys']+[grant()['keys'][0]],key=lambda x:x['public_key'])
                if case=='review-only-runtime': body['testers']=[]; kind='runtime'
                if case=='withdrawn-author': body['authorities']=[]
                if case=='other-target': target='arm-aros-v11-native'
                root=Path(tmp)/'root'; state(root,policy=wire('policy',body),target=target)
                code,ctx=open_ctx(root); code,s=snapshot(ctx); self.assertEqual(code,0)
                code,a=read(s,data,claim(data,issuer=issuer,kind=kind),pk=pk,sk=sk); self.assertEqual(code,0)
                code,text=formatted(a); self.assertEqual(code,0); fields=dict(x.split(' ',1) for x in text.splitlines())
                expected_independent='no' if case in ('author','same-id','alias-key','author-second-key') else 'yes'
                self.assertEqual(fields['independent'],expected_independent)
                self.assertEqual(fields['issuertrust'],'rejected' if case in ('untrusted','author','review-only-runtime') else 'accepted')
                self.assertEqual(fields['authortrust'],'rejected' if case=='withdrawn-author' else 'accepted')
                self.assertEqual(fields['targetmatches'],'no' if case=='other-target' else 'yes')
                lib.apkg_attestation_free(a); lib.apkg_snapshot_free(s); lib.apkg_close(ctx)

    def test_exact_content_identity_domains_schema_and_environment_refuse_misleading_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'root'; state(root,policy=trust()); code,ctx=open_ctx(root); code,s=snapshot(ctx); data=desc()
            for field,value in [('name','different'),('version','2.0'),('revision','2'),('authority','ee'*16),('target','arm-aros-v11-native')]:
                code,a=read(s,data,claim(data,**{field:value})); self.assertEqual(code,14,field)
            self.assertEqual(read(s,desc(content=b'different'),claim(data))[0],10)
            record=claim(data)
            self.assertEqual(read(s,data,record,signature=sign(record,RP,RS,'descriptor'))[0],11)
            self.assertEqual(read(s,data,record.replace(b'Checked',b'Altered'),signature=sign(record,RP,RS,'attestation'))[0],10)
            self.assertNotEqual(read(s,data,record,descriptor_signature=sign(data,RP,RS,'channel'))[0],0)
            base=json.loads(record)
            variants=[]
            for field,value in [('sequence','0'),('issued_at','0'),('result','approved'),('kind','curator'),('supersedes','ab'*32),('summary','bad\ntext'),('environment',{}),('extra','ignored')]:
                b=copy.deepcopy(base); b['body'][field]=value; variants.append(wire('attestation',b['body']))
            variants += [wire('attestation',base['body'],['new-feature']),claim(data,kind='runtime',environment=None),claim(data,result='revoked')]
            for record in variants: self.assertNotEqual(read(s,data,record)[0],0,record)
            lib.apkg_snapshot_free(s); lib.apkg_close(ctx)

    def test_revocation_history_links_and_missing_prefix_never_claim_latest_approval(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'root'; state(root,policy=trust()); code,ctx=open_ctx(root); code,s=snapshot(ctx); data=desc()
            first=claim(data); code,a=read(s,data,first); self.assertEqual(code,0)
            revoked=claim(data,result='revoked',sequence='2',supersedes=hashlib.sha256(first).hexdigest(),issued_at='1234567891')
            code,standalone=read(s,data,revoked); self.assertEqual(code,0); self.assertIn('history not-checked\n',formatted(standalone)[1]); lib.apkg_attestation_free(standalone)
            code,b=read(s,data,revoked,previous=a); self.assertEqual(code,0); lib.apkg_attestation_free(a)
            self.assertIn('result revoked\n',formatted(b)[1]); self.assertIn('history linked-prefix\n',formatted(b)[1]); self.assertIn('freshness unknown\n',formatted(b)[1])
            for changes in [dict(sequence='4'),dict(issued_at='1'),dict(supersedes='00'*32),dict(issuer='dd'*16),dict(kind='runtime')]:
                record=claim(data,sequence='3',supersedes=hashlib.sha256(revoked).hexdigest(),issued_at='1234567892')
                body=json.loads(record)['body']; body.update(changes)
                if changes.get('kind')=='runtime': body['environment']=json.loads(claim(data,kind='runtime'))['body']['environment']
                self.assertEqual(read(s,data,wire('attestation',body),previous=b)[0],14)
            self.assertEqual(read(s,data,first,previous=b)[0],14)
            lib.apkg_attestation_free(b)
            first=claim(data,kind='runtime'); code,a=read(s,data,first); self.assertEqual(code,0)
            environment=json.loads(first)['body']['environment']
            for field in ('hardware','os_build','test_suite','log_sha256'):
                changed=dict(environment); changed[field]='bb'*32 if field=='log_sha256' else 'different'
                next_record=claim(data,kind='runtime',environment=changed,sequence='2',supersedes=hashlib.sha256(first).hexdigest())
                code,b=read(s,data,next_record,previous=a)
                self.assertEqual(code,0 if field=='log_sha256' else 14,field)
                lib.apkg_attestation_free(b)
                folder=Path(tmp)/('chain-'+field); folder.mkdir()
                for name,contents in [('descriptor.json',data),('signature.json',sign(data)),('first.json',first),('first.signature.json',sign(first,RP,RS,'attestation')),('attestation.json',next_record),('attestation.signature.json',sign(next_record,RP,RS,'attestation'))]: (folder/name).write_bytes(contents)
                native=subprocess.run([str(WORKER),str(root),str(folder),'29'],capture_output=True,text=True)
                self.assertEqual(native.returncode,0,native.stdout+native.stderr)
                self.assertTrue(native.stdout.startswith('status %d\n'%(0 if field=='log_sha256' else 14)),native.stdout)
                if field=='log_sha256': self.assertIn('history linked-prefix\n',native.stdout)
            lib.apkg_attestation_free(a); lib.apkg_snapshot_free(s); lib.apkg_close(ctx)

    def test_role_grants_are_shown_in_initialization_and_invalid_policy_refuses(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); seedfile=d/'policy'; seedfile.write_bytes(trust())
            result=subprocess.run([str(CLI),'init',str(d/'root'),TARGET,str(seedfile)],capture_output=True,text=True)
            self.assertEqual(result.returncode,16,result.stdout+result.stderr)
            self.assertIn('Authorize reviewer '+ISSUER+' with key '+RP.hex(),result.stdout)
            self.assertIn('Authorize tester '+ISSUER+' with key '+RP.hex(),result.stdout)
            self.assertFalse((d/'root/.apkg/state').exists())
            for changes in [dict(reviewers=[grant(),grant()]),dict(testers=[dict(id=ISSUER,keys=[])]),dict(reviewers='any')]:
                body=json.loads(trust())['body']; body.update(changes); seedfile.write_bytes(wire('policy',body))
                result=subprocess.run([str(CLI),'init',str(d/'root'),TARGET,str(seedfile)],capture_output=True,text=True)
                self.assertNotEqual(result.returncode,0); self.assertFalse((d/'root/.apkg/state').exists())

    def test_allocation_failures_null_outputs_readonly_and_pending_roots(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'root'; before=state(root,policy=trust()); code,ctx=open_ctx(root); code,s=snapshot(ctx); data=desc(); record=claim(data)
            for nth in range(1,180):
                lib.apkg_test_fail_alloc(nth); code,a=read(s,data,record); lib.apkg_test_fail_alloc(0); self.assertIn(code,(0,4),nth)
                lib.apkg_attestation_free(a)
            code,a=read(s,data,record); self.assertEqual(code,0)
            for nth in range(1,8):
                lib.apkg_test_fail_alloc(nth); code,text=formatted(a); lib.apkg_test_fail_alloc(0); self.assertIn(code,(0,4))
                if code: self.assertIsNone(text)
            lib.apkg_attestation_free(a); lib.apkg_snapshot_free(s)
            Path(str(root)+'.apkg-transaction').mkdir(); code,s=snapshot(ctx); self.assertEqual(read(s,data,record)[0],9)
            lib.apkg_snapshot_free(s); lib.apkg_close(ctx); self.assertEqual((root/'.apkg/state').read_bytes(),before)
