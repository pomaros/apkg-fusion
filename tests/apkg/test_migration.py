# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Exact signed adoption sets and original-preserving legacy-copy rehearsal."""
import copy
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from support import *
from test_client import cli, CLI, WORKER
from test_operations import payload, run_plan, op
import test_adopt as adoption
import test_relations as relations
import test_payload as payloads

spec = importlib.util.spec_from_file_location('apkg_migration', ROOT/'tools/apkg_migrate.py')
migration = importlib.util.module_from_spec(spec);spec.loader.exec_module(migration)

class AdoptionInput(C.Structure):
    _fields_ = [('package',C.c_void_p),('evidence',Text)]
lib.apkg_plan_adopt_many.argtypes = [C.c_void_p,C.c_void_p,C.POINTER(AdoptionInput),C.c_size_t,C.POINTER(C.c_void_p),C.c_void_p]
lib.apkg_plan_adopt_many.restype = C.c_int

def plan_many(c, data):
    code,s=snapshot(c);assert code==0
    owned=[];buffers=[];items=[]
    for item in data:
        code,pkg=package(c,item,sign(item));assert code==0;owned.append(pkg)
        buf=C.create_string_buffer(adoption.evidence(item));buffers.append(buf)
        items.append(AdoptionInput(pkg,Text(C.cast(buf,C.c_void_p),len(buf.value))))
    out=C.c_void_p();code=lib.apkg_plan_adopt_many(c,s,(AdoptionInput*len(items))(*items),len(items),C.byref(out),None)
    for pkg in owned:lib.apkg_package_free(pkg)
    lib.apkg_snapshot_free(s);return code,out

class AdoptionSets(unittest.TestCase):
    def tearDown(self):lib.apkg_test_fault(0,0);lib.apkg_test_fail_alloc(0)
    def setup(self,root):
        c=init(root)
        for name in ('a','b'):
            (root/name).mkdir();(root/name/'hello').write_bytes(b'hello\n');(root/name/'hello').chmod(0o644)
        a=relations.related('a',depends=[relations.requirement('b')])
        b=relations.related('b',depends=[relations.requirement('a')])
        return c,[a,b]
    def test_cyclic_existing_bundles_commit_and_rollback_together(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c,data=self.setup(root);before=migration.inventory(root)
            self.assertEqual(plan_many(c,data[:1])[0],15)
            code,p=plan_many(c,data);self.assertEqual(code,0);self.assertEqual(migration.inventory(root),before)
            self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            state=json.loads((root/'.apkg/state').read_bytes())['body'];self.assertEqual(state['generation'],'2');self.assertEqual(len(state['installed']),2)
            history=adoption.operations.Rollback.last_archive(self,root);code,p=op(c,'rollback',str(history));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            self.assertEqual((root/'a/hello').read_bytes(),b'hello\n');self.assertEqual((root/'b/hello').read_bytes(),b'hello\n');lib.apkg_close(c)
    def test_revision_zero_is_preserved_and_zero_dependency_floor_is_usable(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c,data=self.setup(root)
            a=json.loads(relations.related('a',depends=[relations.requirement('b',revision='0')]))
            b=json.loads(relations.related('b'));b['body']['revision']='0'
            wiredata=[canonical(a),canonical(b)];code,p=plan_many(c,wiredata);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            state=json.loads((root/'.apkg/state').read_bytes())['body'];self.assertEqual(state['installed'][1]['descriptor']['body']['revision'],'0')
            b['body']['revision']='1';newer=canonical(b);code,pkg=package(c,newer,sign(newer));self.assertEqual(code,0);code,s=snapshot(c);p=C.c_void_p();code=lib.apkg_plan_upgrade(c,s,pkg,C.byref(p),None);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1))
            lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);lib.apkg_close(c)

    def test_set_journal_recovers_all_or_none(self):
        for point in (101,102,4,5):
            with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
                root=Path(d)/'root';c,data=self.setup(root);code,p=plan_many(c,data);self.assertEqual(code,0);code,a=approve(c,p);pid=os.fork()
                if pid==0:lib.apkg_test_fault(point,2);execute(c,p,a);os._exit(98)
                _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99)
                self.assertEqual(recover(c),(0,1 if point in (4,5) else 2));self.assertEqual(recover(c)[0],0)
                state=json.loads((root/'.apkg/state').read_bytes())['body'];self.assertEqual(len(state['installed']),2 if point in (4,5) else 0)
                for name in ('a','b'):self.assertEqual((root/name/'hello').read_bytes(),b'hello\n')
                lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)
    def test_duplicate_missing_and_changed_second_bundle_refuse(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c,data=self.setup(root)
            self.assertEqual(plan_many(c,[data[0],data[0]])[0],14)
            (root/'b').rename(Path(d)/'missing-b')
            self.assertEqual(plan_many(c,data)[0],5)
            (Path(d)/'missing-b').rename(root/'b')
            code,p=plan_many(c,data);self.assertEqual(code,0);code,a=approve(c,p)
            (root/'b/personal').write_text('late')
            self.assertEqual(execute(c,p,a)[0],8)
            self.assertEqual(json.loads((root/'.apkg/state').read_bytes())['body']['installed'],[])
            lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)

    def test_many_plan_allocation_failures_release_partial_inputs_and_clear_outputs(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c,data=self.setup(root);code,s=snapshot(c);owned=[];buffers=[];items=[]
            for item in data:
                code,pkg=package(c,item,sign(item));self.assertEqual(code,0);owned.append(pkg)
                buf=C.create_string_buffer(adoption.evidence(item));buffers.append(buf);items.append(AdoptionInput(pkg,Text(C.cast(buf,C.c_void_p),len(buf.value))))
            inputs=(AdoptionInput*2)(*items);before=migration.inventory(root)
            for nth in range(1,1201):
                out=C.c_void_p();lib.apkg_test_fail_alloc(nth)
                code=lib.apkg_plan_adopt_many(c,s,inputs,2,C.byref(out),None);lib.apkg_test_fail_alloc(0)
                self.assertIn(code,(0,4),f'allocation {nth}: {code}')
                if code:self.assertFalse(out)
                lib.apkg_plan_free(out)
            self.assertEqual(migration.inventory(root),before)
            for pkg in owned:lib.apkg_package_free(pkg)
            lib.apkg_snapshot_free(s);lib.apkg_close(c)

class MigrationCopies(unittest.TestCase):
    def setup_fixture(self,d,legacy=False):
        base=Path(d);source=base/'original';(source/'db/installed').mkdir(parents=True);(source/'db/transactions').mkdir()
        (source/'cache/previous').mkdir(parents=True);(source/'tmp').mkdir();(source/'map-demo').mkdir()
        fixture=ROOT/'tests/fixtures/migration'/('registry-legacy.json' if legacy else 'registry-current.json')
        raw_record=fixture.read_bytes();record=json.loads(raw_record);(source/'db/installed/map-demo.json').write_bytes(raw_record)
        (source/'map-demo/Demo').write_bytes(b'synthetic program\n');(source/'map-demo/Demo').chmod(0o755)
        (source/'map-demo/settings.cfg').write_bytes(b'theme=user\n');(source/'map-demo/notes.txt').write_bytes(b'personal\n')
        (source/'map-demo/empty').mkdir();(source/'map-demo/.hidden').write_bytes(b'hidden\n')
        (source/'map-demo.info').write_bytes(b'Workbench edited icon');(source/'settings.json').write_bytes(b'{"channel":"https://example.invalid/old"}\n')
        adoption.attributes.setattrs(source/'map-demo',adoption.attributes.attrs(comment='personal drawer',protection='0'))
        delivered={'Demo':b'synthetic program\n','settings.cfg':b'theme=original\n'}
        files=[]
        for row in record['contents']:
            content=delivered[row['path']];self.assertEqual(hashlib.sha256(content).hexdigest(),row['sha256']);self.assertEqual(len(content),row['size'])
            f=payload(row['path'],content.decode(),row['path']=='settings.cfg')
            if row['path']=='Demo':f['mode']='493'
            files.append(f)
        data=json.loads(descriptor(name='map-demo',files=files));data['body']['version']=record['version'];data['body']['revision']=str(record.get('revision',0))
        data=json.dumps(data,sort_keys=True,separators=(',',':')).encode()
        artifact=base/'artifact';artifact.mkdir();(artifact/'descriptor.json').write_bytes(data);(artifact/'signature.json').write_bytes(sign(data))
        mapping=base/'mapping.json';mapping.write_bytes(canonical(dict(format='apkg-legacy-import-1',legacy_root='Work:Packages',packages=[dict(name='map-demo',artifact=str(artifact))])))
        seed=base/'policy.json';seed.write_bytes(policy())
        return source,base/'copy',mapping,seed,artifact
    def helper(self,*args):
        return subprocess.run([sys.executable,str(ROOT/'tools/apkg_migrate.py'),'--apkg',str(CLI),*map(str,args)],capture_output=True,text=True)
    def prepare(self,source,destination,mapping,seed):
        result=self.helper('prepare',source,destination,mapping,seed,TARGET,'--quiescent')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr);return json.loads(result.stdout)
    def test_pinned_fixture_copy_commit_operations_and_offline_rollback(self):
        with tempfile.TemporaryDirectory() as d:
            source,destination,mapping,seed,artifact=self.setup_fixture(d);before=migration.inventory(source)
            prepared=self.prepare(source,destination,mapping,seed)
            self.assertEqual(migration.inventory(source),before)
            self.assertEqual(json.loads((destination/'.apkg/state').read_bytes())['body']['installed'],[])
            bad=self.helper('commit',destination,'--confirm','0'*64);self.assertNotEqual(bad.returncode,0)
            result=self.helper('commit',destination,'--confirm',prepared['approval']);self.assertEqual(result.returncode,0,result.stderr)
            entry=json.loads((destination/'.apkg/state').read_bytes())['body']['installed'][0]
            self.assertEqual(entry['reason'],'dependency');self.assertEqual(entry['descriptor']['body']['revision'],'2')
            self.assertEqual(bytes.fromhex(entry['adoption']['evidence']['body']['record']),(source/'db/installed/map-demo.json').read_bytes())
            self.assertEqual((destination/'.apkg-legacy/db/installed/map-demo.json').read_bytes(),(source/'db/installed/map-demo.json').read_bytes())
            self.assertEqual((destination/'settings.json').read_bytes(),(source/'settings.json').read_bytes())
            self.assertEqual(prepared['observations'][0]['history']['archive']['bytes'],'unavailable-or-unresolved')
            # Consumer operations use the common CLI; original runtime is not invoked.
            for command in ('info','show','list'):
                result=cli(command,destination,'map-demo');self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            libpath=str(destination)
            code,c=open_ctx(libpath,1);self.assertEqual(code,0)
            (destination/'map-demo/Demo').write_bytes(b'damaged');code,p=op(c,'repair','map-demo');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            self.assertEqual((destination/'map-demo/Demo').read_bytes(),b'synthetic program\n');self.assertEqual((destination/'map-demo/settings.cfg').read_bytes(),b'theme=user\n')
            code,p=op(c,'remove','map-demo');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            history=adoption.operations.Rollback.last_archive(self,destination)
            artifact.rename(Path(d)/'artifact-offline')
            code,p=op(c,'rollback',str(history));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_close(c)
            self.assertTrue((destination/'map-demo/Demo').exists());self.assertEqual((destination/'map-demo/notes.txt').read_bytes(),b'personal\n')
            self.assertEqual((destination/'map-demo.info').read_bytes(),b'Workbench edited icon');self.assertEqual(migration.inventory(source),before)
    def test_unrecorded_facts_remain_unknown_and_committed_retry_is_offline(self):
        with tempfile.TemporaryDirectory() as d:
            source,destination,mapping,seed,folder=self.setup_fixture(d,legacy=True)
            data=json.loads((folder/'descriptor.json').read_bytes());objects=[]
            for entry in data['body']['files']:
                content=bytes.fromhex(entry.pop('data'));entry['blob']=entry['sha256'];objects.append(('objects/'+entry['blob'],content))
            packed=canonical(data);artifact=payloads.archive(Path(d)/'signed.pkg',packed,objects)
            mapping.write_bytes(canonical(dict(format='apkg-legacy-import-1',legacy_root='Work:Packages',packages=[dict(name='map-demo',artifact=str(artifact))])))
            prepared=self.prepare(source,destination,mapping,seed)
            self.assertEqual(prepared['observations'][0]['abi'],'unrecorded');self.assertEqual(prepared['observations'][0]['relations'],'unrecorded')
            result=self.helper('commit',destination,'--confirm',prepared['approval']);self.assertEqual(result.returncode,0,result.stderr)
            state=json.loads((destination/'.apkg/state').read_bytes())['body']['installed'][0];self.assertEqual(state['reason'],'unrecorded');self.assertEqual(state['descriptor']['body']['revision'],'0')
            source.rename(Path(d)/'source-offline');artifact.rename(Path(d)/'artifact-offline')
            result=self.helper('commit',destination,'--confirm',prepared['approval']);self.assertEqual(result.returncode,0,result.stderr);self.assertTrue(json.loads(result.stdout)['already_committed'])
    def test_ambiguous_pending_and_hostile_input_refuse_with_original_preserved(self):
        cases=('lock','pending','mixed','relations','unknown-field','traversal','symlink','duplicate-json')
        for case in cases:
            with self.subTest(case=case),tempfile.TemporaryDirectory() as d:
                source,destination,mapping,seed,artifact=self.setup_fixture(d);record=source/'db/installed/map-demo.json'
                if case=='lock':(source/'db/lock.owner').write_text('owned')
                elif case=='pending':(source/'db/transactions/active.json').write_text('{}')
                elif case=='mixed':(source/'.pkg').mkdir()
                elif case=='symlink':(source/'map-demo/link').symlink_to('Demo')
                elif case=='duplicate-json':record.write_bytes(b'{"schema":1,"schema":1}')
                else:
                    data=json.loads(record.read_bytes())
                    if case=='relations':data['depends']=['unmapped']
                    elif case=='unknown-field':data['system_effects']=['unknown']
                    elif case=='traversal':data['contents'][0]['path']='../escape'
                    record.write_bytes(canonical(data))
                result=self.helper('prepare',source,destination,mapping,seed,TARGET,'--quiescent')
                self.assertNotEqual(result.returncode,0,result.stdout);self.assertFalse(destination.exists());self.assertTrue(record.exists())
    def test_bad_signature_or_valid_wrong_build_cannot_commit_import(self):
        for changed in ('signature','version','target'):
            with self.subTest(changed=changed),tempfile.TemporaryDirectory() as d:
                source,destination,mapping,seed,artifact=self.setup_fixture(d);before=migration.inventory(source)
                if changed=='signature':(artifact/'signature.json').write_bytes(b'not a signature')
                else:
                    data=json.loads((artifact/'descriptor.json').read_bytes());data['body'][changed]='9.0' if changed=='version' else 'aarch64-aros-v1-native';raw=canonical(data);(artifact/'descriptor.json').write_bytes(raw);(artifact/'signature.json').write_bytes(sign(raw))
                result=self.helper('prepare',source,destination,mapping,seed,TARGET,'--quiescent');self.assertNotEqual(result.returncode,0)
                self.assertEqual(migration.inventory(source),before)
                self.assertEqual(json.loads((destination/'.apkg/state').read_bytes())['body']['installed'],[])
                self.assertFalse((destination/migration.RECEIPT).exists())

    def test_source_copy_and_artifact_changes_invalidate_approval(self):
        for changed in ('source','copy','artifact'):
            with self.subTest(changed=changed),tempfile.TemporaryDirectory() as d:
                source,destination,mapping,seed,artifact=self.setup_fixture(d);prepared=self.prepare(source,destination,mapping,seed)
                if changed=='artifact':(artifact/'signature.json').write_bytes(b'changed')
                else:((source if changed=='source' else destination)/'map-demo/notes.txt').write_text('changed after preparation')
                result=self.helper('commit',destination,'--confirm',prepared['approval']);self.assertNotEqual(result.returncode,0)
                self.assertEqual(json.loads((destination/'.apkg/state').read_bytes())['body']['installed'],[])
    def test_interrupted_commit_uses_only_common_recovery_and_decided_recovery_is_offline(self):
        confirm_type=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_char_p)
        lib.apkg_client_apply.argtypes=[C.c_int,C.c_char_p,C.c_char_p,C.c_char_p,confirm_type,C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p]
        lib.apkg_client_apply.restype=C.c_int
        for point in (101,102,4,5):
            with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
                source,destination,mapping,seed,artifact=self.setup_fixture(d);prepared=self.prepare(source,destination,mapping,seed);before=migration.inventory(source)
                receipt=json.loads((destination/migration.RECEIPT).read_bytes());expected=migration.canonical(receipt['body']['plan'])
                @confirm_type
                def confirm(user,plan,presentation):
                    if raw(lib.apkg_plan_get(plan).contents.canonical)!=expected:return 0
                    lib.apkg_test_fault(point,2);return 1
                pid=os.fork()
                if pid==0:
                    result=C.c_void_p();lib.apkg_client_apply(10,os.fsencode(destination),os.fsencode(destination/'.apkg-legacy/adoption-set.json'),None,confirm,None,None,C.byref(result),None);os._exit(98)
                _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99)
                if point in (4,5):
                    source.rename(Path(d)/'source-offline');source=Path(d)/'source-offline';artifact.rename(Path(d)/'artifact-offline')
                result=self.helper('commit',destination,'--confirm',prepared['approval']);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertEqual(len(json.loads((destination/'.apkg/state').read_bytes())['body']['installed']),1)
                self.assertEqual(migration.inventory(source),before)
                again=self.helper('commit',destination,'--confirm',prepared['approval']);self.assertEqual(again.returncode,0,again.stderr)

    def test_unrelated_pending_common_plan_is_not_recovered_by_import_approval(self):
        confirm_type=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_char_p)
        lib.apkg_client_apply.argtypes=[C.c_int,C.c_char_p,C.c_char_p,C.c_char_p,confirm_type,C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p]
        lib.apkg_client_apply.restype=C.c_int
        with tempfile.TemporaryDirectory() as d:
            source,destination,mapping,seed,artifact=self.setup_fixture(d);prepared=self.prepare(source,destination,mapping,seed)
            other=Path(d)/'other';other.mkdir();data=descriptor(name='other');(other/'descriptor.json').write_bytes(data);(other/'signature.json').write_bytes(sign(data))
            @confirm_type
            def confirm(user,plan,presentation):lib.apkg_test_fault(4,2);return 1
            pid=os.fork()
            if pid==0:
                result=C.c_void_p();lib.apkg_client_apply(0,os.fsencode(destination),os.fsencode(other),None,confirm,None,None,C.byref(result),None);os._exit(98)
            _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99)
            state=(destination/'.apkg/state').read_bytes();txn=Path(str(destination)+'.apkg-transaction');before=migration.inventory(txn)
            result=self.helper('commit',destination,'--confirm',prepared['approval']);self.assertNotEqual(result.returncode,0);self.assertIn('another plan',result.stderr)
            self.assertEqual((destination/'.apkg/state').read_bytes(),state);self.assertEqual(migration.inventory(txn),before)

    def test_interrupted_private_preparation_leaves_original_and_fresh_retry_usable(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as d:
            source,destination,mapping,seed,artifact=self.setup_fixture(d);before=migration.inventory(source)
            pid=os.fork()
            if pid==0:
                original=migration.copy_inventory
                def interrupted(*args):original(*args);os._exit(91)
                migration.copy_inventory=interrupted
                migration.prepare(SimpleNamespace(source=source,destination=destination,mapping=mapping,policy=seed,target=TARGET,quiescent=True,apkg=CLI));os._exit(98)
            _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),91)
            self.assertEqual(migration.inventory(source),before);self.assertEqual(migration.inventory(destination),before)
            refusal=self.helper('prepare',source,destination,mapping,seed,TARGET,'--quiescent');self.assertNotEqual(refusal.returncode,0)
            fresh=Path(d)/'fresh-retry';prepared=self.prepare(source,fresh,mapping,seed)
            result=self.helper('commit',fresh,'--confirm',prepared['approval']);self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(migration.inventory(source),before);self.assertEqual(migration.inventory(destination),before)

    def test_cli_and_actual_worker_set_equivalence_and_machine_preview(self):
        with tempfile.TemporaryDirectory() as d:
            source,a,mapping,seed,artifact=self.setup_fixture(d);b=Path(d)/'second-copy'
            left=self.prepare(source,a,mapping,seed);right=self.prepare(source,b,mapping,seed)
            result=self.helper('commit',a,'--confirm',left['approval']);self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(WORKER),str(b),str(b/'.apkg-legacy/adoption-set.json'),'1','10'],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(json.loads((a/'.apkg/state').read_bytes())['body']['installed'],json.loads((b/'.apkg/state').read_bytes())['body']['installed'])

def canonical(value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()
