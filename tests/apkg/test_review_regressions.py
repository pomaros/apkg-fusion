# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Integration regressions found while reviewing the fusion additions."""
import copy
import re
import subprocess
import tempfile
import unittest
from support import *
import test_relations as relations
import test_system as system
import test_channel_clients as channel
import test_migration as migration
import test_author as author

class ReviewRegressions(unittest.TestCase):
    def tearDown(self):lib.apkg_test_system(None)

    def test_unversioned_dependency_means_any_version_and_revision(self):
        for version in ('nightly','0-rc1','1.0'):
            with self.subTest(version=version),tempfile.TemporaryDirectory() as d:
                root=Path(d)/'root';c=init(root)
                wanted=relations.related('app',depends=[relations.requirement('lib',version='',revision='0')])
                provider=json.loads(relations.related('lib',version=version));provider['body']['revision']='0';provider=json.dumps(provider,sort_keys=True,separators=(',',':')).encode()
                code,p=relations.resolve(c,[wanted],[provider]);self.assertEqual(code,0);self.assertEqual(channel.run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_close(c)

    def test_toml_does_not_invent_version_or_packaging_revision_floors(self):
        with tempfile.TemporaryDirectory() as d:
            d=Path(d);source=d/'source';author.author_fixture(source,depends=['dependency'])
            key=author.key_fixture(d/'key');archive=d/'p.pkg';result=author.build(source,archive,key);self.assertEqual(result.returncode,0,result.stderr)
            body=json.loads(author.members(archive)['descriptor.json'])['body']
            req=body['relations']['depends'][0];self.assertEqual(req['min_version'],'');self.assertEqual(req['min_revision'],'0')

    def published(self,d):
        source=d/'source';manifest=author.author_fixture(source)
        manifest.write_text(manifest.read_text()+'\n[[requires_system]]\ntype="library"\nid="present.library"\nmin_version=2\n')
        key=author.key_fixture(d/'key');ckey=channel.channel_key(d/'channel-key');archive=d/'p.pkg'
        result=author.build(source,archive,key);self.assertEqual(result.returncode,0,result.stderr)
        endpoint=d/'channel';result=channel.publish(archive,endpoint,ckey);self.assertEqual(result.returncode,0,result.stderr)
        return endpoint

    def test_channel_system_observations_belong_to_plan_body_and_execute(self):
        with tempfile.TemporaryDirectory() as d:
            d=Path(d);endpoint=self.published(d);root=d/'root';state(root,policy=channel.seed());code,c=open_ctx(root,1)
            lib.apkg_test_system(b'present.library=3');code,ch=channel.open_file(c,endpoint);self.assertEqual(code,0)
            code,p=channel.plan(c,ch,channel.digest(endpoint,'demo'));self.assertEqual(code,0)
            body=json.loads(raw(lib.apkg_plan_get(p).contents.canonical))['body']
            self.assertEqual(body['requirements'][0]['state'],'satisfied');self.assertNotIn('requirements',body['origins'][0])
            self.assertEqual(channel.run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_channel_free(ch);lib.apkg_close(c)

    def test_actual_cli_and_worker_install_channel_requirements_on_host(self):
        with tempfile.TemporaryDirectory() as d:
            d=Path(d);endpoint=self.published(d);left=d/'cli-root';right=d/'worker-root'
            for root in (left,right):state(root,policy=channel.seed())
            selection=f"{channel.CHANNEL}/{channel.head(endpoint)}/{channel.digest(endpoint,'demo')}"
            args=('channel-install',left,endpoint,*selection.split('/'))
            preview=channel.clients.cli(*args);self.assertEqual(preview.returncode,16,preview.stdout+preview.stderr)
            self.assertIn('undetermined',preview.stdout)
            approved=re.search(r'Approved plan: ([a-f0-9]{64})',preview.stdout).group(1)
            done=channel.clients.cli(*args,'--confirm',approved);self.assertEqual(done.returncode,0,done.stdout+done.stderr)
            worker=subprocess.run([str(channel.clients.WORKER),str(right),str(endpoint),'1','7',selection],capture_output=True,text=True)
            self.assertEqual(worker.returncode,0,worker.stdout+worker.stderr);self.assertEqual(channel.committed(left),channel.committed(right))

    def test_machine_change_after_commit_decision_does_not_undo_commit(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c=init(root);lib.apkg_test_system(b'present.library=3')
            code,p=system.plan_install(c,system.wanting([system.req('present.library',2)]));self.assertEqual(code,0)
            code,a=approve(c,p);self.assertEqual(code,0)
            @EVENT_FN
            def changed(user,event):
                if event.contents.phase==4:lib.apkg_test_system(b'')
                return 0
            observer=Observer(changed,None,None);self.assertEqual(execute(c,p,a,C.byref(observer)),(0,1))
            self.assertTrue((root/'demo/hello').exists());lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)

    def test_missing_system_requirement_refuses_before_archive_transfer(self):
        with tempfile.TemporaryDirectory() as d:
            d=Path(d);endpoint=self.published(d);root=d/'root';before=state(root,policy=channel.seed());code,c=open_ctx(root,1)
            lib.apkg_test_system(b'')
            with channel.serve(endpoint) as (url,server):
                code,ch=channel.http_open(c,url);self.assertEqual(code,0)
                code,p=channel.plan(c,ch,channel.digest(endpoint,'demo'));self.assertEqual(code,19);self.assertFalse(p)
                self.assertFalse(any(path.startswith('/archives/') for path in server.requests),server.requests)
                lib.apkg_channel_free(ch)
            self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_close(c)

    def test_changed_requirements_restore_state_at_stage_and_apply_boundaries(self):
        for upgrade in (False,True):
            for phase in (2,3):
                with self.subTest(upgrade=upgrade,phase=phase),tempfile.TemporaryDirectory() as d:
                    root=Path(d)/'root';c=init(root)
                    if upgrade:system.install(c,descriptor())
                    before=(root/'.apkg/state').read_bytes();lib.apkg_test_system(b'present.library=3')
                    data=system.wanting([system.req('present.library',2)],version='2.0' if upgrade else '1.0')
                    if upgrade:
                        code,s=snapshot(c);code,pkg=package(c,data,sign(data));p=C.c_void_p();code=lib.apkg_plan_upgrade(c,s,pkg,C.byref(p),None);lib.apkg_snapshot_free(s);lib.apkg_package_free(pkg)
                    else:code,p=system.plan_install(c,data)
                    self.assertEqual(code,0);code,a=approve(c,p);self.assertEqual(code,0)
                    @EVENT_FN
                    def changed(user,event):
                        if event.contents.phase==phase:lib.apkg_test_system(b'')
                        return 0
                    observer=Observer(changed,None,None)
                    self.assertEqual(execute(c,p,a,C.byref(observer)),(8,2))
                    self.assertEqual((root/'.apkg/state').read_bytes(),before)
                    if upgrade:self.assertEqual((root/'demo/hello').read_bytes(),b'hello\n')
                    else:self.assertFalse((root/'demo').exists())
                    lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)

    def test_legacy_binding_checks_author_target_revision_and_absent_namespace(self):
        case=migration.MigrationRelations()
        with tempfile.TemporaryDirectory() as d:
            source,destination,mapping,seed=case.fixture(d,case.LEGACY,case.depends('1.0'))
            prepared=case.helper('prepare',source,destination,mapping,seed,TARGET,'--quiescent');self.assertEqual(prepared.returncode,0,prepared.stderr)
            receipt=json.loads((destination/'.apkg-legacy/import.json').read_bytes())['body'];records={}
            for record in (source/'db/installed').iterdir():
                item=json.loads(record.read_bytes());records[item['name']]=item
            plan=receipt['plan'];mapping_data=json.loads(mapping.read_bytes())
            for field,value in [('authority','cc'*16),('target','aarch64-aros-v1-native'),('min_revision','99')]:
                wrong=copy.deepcopy(plan)
                entry=next(e for e in wrong['body']['new_state']['body']['installed'] if e['descriptor']['body']['name']=='map-demo')
                entry['descriptor']['body']['relations']['recommends'][0][field]=value
                with self.subTest(field=field),self.assertRaises(migration.migration.Refused):migration.migration.validate_selected(records,wrong,mapping_data)
            with self.assertRaises(migration.migration.Refused):migration.migration.validate_selected(records,plan,dict(mapping_data,identities=[]))
            # Recommendations are deliberately omitted by arospkg's registry writer.
            del records['map-demo']['recommends']
            migration.migration.validate_selected(records,plan,mapping_data)
