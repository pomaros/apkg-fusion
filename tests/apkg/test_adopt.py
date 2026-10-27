# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Existing-tree adoption, permanent ownership guards, and common clients."""
import copy
import os
import stat
import tempfile
import unittest
from support import *
from test_operations import payload, op, run_plan
import test_operations as operations
from test_client import cli, WORKER
import test_client as clients
import test_attributes as attributes

lib.apkg_plan_adopt.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, Text,
                                C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_plan_adopt.restype = C.c_int


def evidence(data, owned=None):
    files = json.loads(data)['body']['files']
    baseline = [{k: f[k] for k in ('path', 'sha256', 'size')}
                for f in files if f['type'] == 'file' and
                (owned is None or f['path'] in owned)]
    return wire('adoption', dict(baseline=baseline, reason='unrecorded',
                                record=b'{"legacy":"original bytes"}\n'.hex(),
                                source='arospkg-v1'))


def adopt(c, data, spec):
    code, s = snapshot(c)
    assert code == 0, code
    code, pkg = package(c, data, sign(data))
    assert code == 0, code
    out = C.c_void_p()
    code = lib.apkg_plan_adopt(c, s, pkg, Text(C.cast(C.c_char_p(spec), C.c_void_p), len(spec)), C.byref(out), None)
    lib.apkg_package_free(pkg)
    lib.apkg_snapshot_free(s)
    return code, out


def tree(root):
    rows={}
    for p in [root,*root.rglob('*')]:
        try: metadata=attributes.observed(p)
        except OSError: metadata=None
        rows[str(p.relative_to(root))]=(stat.S_IMODE(p.stat().st_mode),
            p.read_bytes() if p.is_file() else None,metadata)
    return rows


class Adoption(unittest.TestCase):
    def tearDown(self):
        lib.apkg_test_fault(0, 0)
        lib.apkg_test_fail_alloc(0)

    def setup_bundle(self, root):
        (root/'demo').mkdir()
        (root/'demo/hello').write_bytes(b'hello\n')
        (root/'demo/.personal').write_bytes(b'personal')
        (root/'demo/empty').mkdir(mode=0o750)
        (root/'demo/hello').chmod(0o644)
        attributes.setattrs(root/'demo',attributes.attrs(comment='personal drawer',protection='0'))
        attributes.setattrs(root/'demo/empty',attributes.attrs(comment='keep empty',protection='0'))

    def test_adoption_preserves_tree_and_rollback_returns_unmanaged(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)/'root'; c = init(root); self.setup_bundle(root)
            before = tree(root/'demo'); state_before = (root/'.apkg/state').read_bytes()
            data = descriptor(); spec = evidence(data)
            code, p = adopt(c, data, spec); self.assertEqual(code, 0)
            plan = json.loads(raw(lib.apkg_plan_get(p).contents.canonical))['body']
            self.assertEqual(plan['changes'][0]['before'], plan['changes'][0]['next'])
            self.assertEqual((root/'.apkg/state').read_bytes(), state_before)
            self.assertEqual(run_plan(c, p), (0, 1)); lib.apkg_plan_free(p)
            self.assertEqual(tree(root/'demo'), before)
            record = json.loads((root/'.apkg/state').read_bytes())['body']['installed'][0]
            self.assertEqual(record['reason'], 'unrecorded')
            self.assertEqual(record['adoption']['unowned'], ['.personal', 'empty'])
            self.assertEqual(record['adoption']['evidence'], json.loads(spec))
            self.assertEqual(adopt(c, data, spec)[0], 14)
            history = operations.Rollback.last_archive(self, root)
            code, p = op(c, 'rollback', str(history)); self.assertEqual(code, 0)
            self.assertEqual(run_plan(c, p), (0, 1)); lib.apkg_plan_free(p)
            self.assertEqual(tree(root/'demo'), before)
            self.assertEqual(json.loads((root/'.apkg/state').read_bytes())['body']['installed'], [])
            lib.apkg_close(c)

    def test_repair_upgrade_remove_keep_edits_and_even_identical_unowned_files(self):
        for resolved in (False, True):
            with self.subTest(resolved=resolved), tempfile.TemporaryDirectory() as d:
                root = Path(d)/'root'; c = init(root); self.setup_bundle(root)
                files = [payload('edited', 'baseline'), payload('foreign', 'same'),
                         payload('hello', 'hello\n'), payload('missing', 'restore')]
                data = descriptor(files=files)
                (root/'demo/edited').write_text('personal edit')
                (root/'demo/foreign').write_text('same')
                code, p = adopt(c, data, evidence(data, ['edited','hello','missing']))
                self.assertEqual(code, 0); self.assertEqual(run_plan(c,p), (0,1)); lib.apkg_plan_free(p)
                code, p = op(c, 'repair'); self.assertEqual(code, 0)
                self.assertEqual(run_plan(c,p), (0,1)); lib.apkg_plan_free(p)
                self.assertEqual((root/'demo/edited').read_text(), 'personal edit')
                self.assertEqual((root/'demo/missing').read_text(), 'restore')
                newer = json.loads(data); newer['body']['version'] = '2.0'
                newer['body']['files'] = [payload('edited','new'),payload('foreign','new'),payload('hello','v2'),payload('missing','new')]
                newer = json.dumps(newer,sort_keys=True,separators=(',',':')).encode()
                code, pkg = package(c,newer,sign(newer)); self.assertEqual(code,0)
                code, s = snapshot(c); p=C.c_void_p()
                if resolved:
                    lib.apkg_plan_resolve.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.c_size_t,C.POINTER(C.c_void_p),C.c_size_t,C.c_int,C.POINTER(C.c_void_p),C.c_void_p]
                    lib.apkg_plan_resolve.restype=C.c_int
                    code=lib.apkg_plan_resolve(c,s,(C.c_void_p*1)(pkg),1,None,0,0,C.byref(p),None)
                else: code=lib.apkg_plan_upgrade(c,s,pkg,C.byref(p),None)
                self.assertEqual(code,0); self.assertEqual(run_plan(c,p),(0,1))
                lib.apkg_plan_free(p);lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s)
                self.assertEqual((root/'demo/edited').read_text(),'personal edit')
                self.assertEqual((root/'demo/foreign').read_text(),'same')
                self.assertEqual((root/'demo/hello').read_text(),'v2')
                code,p=op(c,'remove');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
                self.assertEqual((root/'demo/edited').read_text(),'personal edit')
                self.assertEqual((root/'demo/foreign').read_text(),'same')
                self.assertFalse((root/'demo/hello').exists());self.assertFalse((root/'demo/missing').exists())
                self.assertTrue((root/'demo/empty').is_dir());lib.apkg_close(c)

    def test_personal_attributes_and_unowned_bundle_drawer_survive(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c=init(root);(root/'demo').mkdir();(root/'demo/hello').write_bytes(b'hello\n');(root/'demo/hello').chmod(0o644)
            original=attributes.attrs(comment='original')
            modified=attributes.attrs(comment='personal file comment')
            attributes.setattrs(root/'demo/hello',modified)
            attributes.setattrs(root/'demo',attributes.attrs(comment='personal drawer',protection='0'))
            f=payload('hello','hello\n');f['amiga']=original;data=descriptor(files=[f])
            code,p=adopt(c,data,evidence(data));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            code,p=op(c,'repair');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            self.assertEqual(attributes.observed(root/'demo/hello'),modified)
            code,p=op(c,'remove');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            self.assertTrue((root/'demo/hello').is_file())
            self.assertEqual(attributes.observed(root/'demo/hello'),modified)
            self.assertEqual(attributes.observed(root/'demo')['comment'],b'personal drawer'.hex())
            lib.apkg_close(c)
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c=init(root);(root/'demo').mkdir();(root/'demo/hello').write_bytes(b'hello\n');(root/'demo/hello').chmod(0o644)
            data=descriptor();code,p=adopt(c,data,evidence(data));self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            code,p=op(c,'remove');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            self.assertEqual(list((root/'demo').iterdir()),[])
            lib.apkg_close(c)

    def test_invalid_or_mismatched_baselines_refuse_without_writes(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c=init(root);self.setup_bundle(root)
            data=descriptor();good=json.loads(evidence(data));bad=[]
            for field,value in [('sha256','00'*32),('size','7'),('path','../escape')]:
                item=copy.deepcopy(good);item['body']['baseline'][0][field]=value;bad.append(item)
            item=copy.deepcopy(good);item['body']['baseline']*=2;bad.append(item)
            for field,value in [('source','guessed'),('record','GG'),('reason','inferred')]:
                item=copy.deepcopy(good);item['body'][field]=value;bad.append(item)
            before=tree(root)
            for item in bad:
                code,p=adopt(c,data,json.dumps(item,sort_keys=True,separators=(',',':')).encode())
                self.assertEqual(code,1);self.assertFalse(p);self.assertEqual(tree(root),before)
            lib.apkg_close(c)

    def test_unsupported_live_nodes_and_case_aliases_are_not_adopted(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c=init(root);self.setup_bundle(root);data=descriptor();spec=evidence(data)
            (root/'demo/link').symlink_to('hello')
            self.assertEqual(adopt(c,data,spec)[0],2)
            (root/'demo/link').unlink()
            (root/'demo/alias').write_text('lower')
            (root/'demo/ALIAS').write_text('upper')
            if not (root/'demo/alias').samefile(root/'demo/ALIAS'):
                self.assertEqual(adopt(c,data,spec)[0],14)
            self.assertEqual(json.loads((root/'.apkg/state').read_bytes())['body']['installed'],[])
            lib.apkg_close(c)

    def test_stale_plan_cannot_claim_changed_tree(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c=init(root);self.setup_bundle(root);data=descriptor()
            code,p=adopt(c,data,evidence(data));self.assertEqual(code,0)
            code,a=approve(c,p);self.assertEqual(code,0)
            (root/'demo/new-personal').write_text('after approval')
            self.assertEqual(execute(c,p,a)[0],8)
            self.assertEqual((root/'demo/new-personal').read_text(),'after approval')
            self.assertEqual(json.loads((root/'.apkg/state').read_bytes())['body']['installed'],[])
            lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)

    def test_common_journal_interruption_and_repeat_recovery(self):
        for point in (101,102,4,5):
            with self.subTest(point=point),tempfile.TemporaryDirectory() as d:
                root=Path(d)/'root';c=init(root);self.setup_bundle(root);before=tree(root/'demo')
                data=descriptor();code,p=adopt(c,data,evidence(data));self.assertEqual(code,0)
                code,a=approve(c,p);self.assertEqual(code,0);pid=os.fork()
                if pid==0:
                    lib.apkg_test_fault(point,2);execute(c,p,a);os._exit(98)
                _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99)
                self.assertEqual(recover(c),(0,1 if point in (4,5) else 2))
                self.assertEqual(recover(c)[0],0);self.assertEqual(tree(root/'demo'),before)
                installed=json.loads((root/'.apkg/state').read_bytes())['body']['installed']
                self.assertEqual(len(installed),1 if point in (4,5) else 0)
                lib.apkg_approval_free(a);lib.apkg_plan_free(p);lib.apkg_close(c)

    def test_allocation_failures_are_typed_and_read_only(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'root';c=init(root);self.setup_bundle(root);data=descriptor();spec=evidence(data)
            code,s=snapshot(c);code,pkg=package(c,data,sign(data));self.assertEqual(code,0)
            before=tree(root)
            for nth in range(1,1001):
                out=C.c_void_p();lib.apkg_test_fail_alloc(nth)
                code=lib.apkg_plan_adopt(c,s,pkg,Text(C.cast(C.c_char_p(spec),C.c_void_p),len(spec)),C.byref(out),None)
                lib.apkg_test_fail_alloc(0)
                self.assertIn(code,(0,4),f'allocation {nth}: status {code}')
                lib.apkg_plan_free(out)
            self.assertEqual(tree(root),before)
            code,p=adopt(c,data,spec);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p)
            lib.apkg_snapshot_free(s);code,s=snapshot(c);self.assertEqual(code,0)
            before=tree(root)
            for name in ('snapshot','repair'):
                for nth in range(1,1001):
                    out=C.c_void_p();lib.apkg_test_fail_alloc(nth)
                    if name=='snapshot':code=lib.apkg_snapshot_read(c,C.byref(out),None)
                    else:code=lib.apkg_plan_repair(c,s,operations.text('demo'),C.byref(out),None)
                    lib.apkg_test_fail_alloc(0)
                    self.assertIn(code,(0,4),f'{name} allocation {nth}: {code}')
                    if name=='snapshot':lib.apkg_snapshot_free(out)
                    else:lib.apkg_plan_free(out)
            self.assertEqual(tree(root),before)
            lib.apkg_package_free(pkg);lib.apkg_snapshot_free(s);lib.apkg_close(c)


class AdoptionClients(unittest.TestCase):
    setup_root_package = clients.OperationClients.setup_root_package
    commit = clients.OperationClients.commit
    worker = clients.OperationClients.worker
    compare = clients.OperationClients.compare
    def test_cli_worker_adoption_and_inspection_equivalence(self):
        with tempfile.TemporaryDirectory() as d:
            a,pa=self.setup_root_package(d,'left');b,pb=self.setup_root_package(d,'right')
            data=(pa/'descriptor.json').read_bytes();spec=Path(d)/'adoption.json';spec.write_bytes(evidence(data))
            for root in (a,b):Adoption.setup_bundle(self,root)
            before=tree(a/'demo')
            result=self.commit('adopt',a,pa,'--evidence',spec)
            self.assertIn('Original legacy authentication: unknown',result.stdout)
            self.worker(b,pb,9,spec);self.compare(a,b)
            self.assertEqual(tree(a/'demo'),before);self.assertEqual(tree(b/'demo'),before)
            info=cli('info',a,'demo')
            self.assertEqual(info.returncode,0,info.stdout+info.stderr)
            self.assertIn('unrecorded',info.stdout);self.assertIn('original-authentication=unknown',info.stdout)
            self.assertIn('verified',info.stdout)
            self.commit('repair',a,'demo');self.worker(b,'demo',2);self.compare(a,b)
            self.assertEqual(tree(a/'demo'),before)
            newer=json.loads(data);newer['body']['version']='2.0'
            newer['body']['files']=[payload('.personal','publisher replacement'),payload('hello','new version')]
            newer=json.dumps(newer,sort_keys=True,separators=(',',':')).encode()
            for folder in (pa,pb):
                (folder/'descriptor.json').write_bytes(newer);(folder/'signature.json').write_bytes(sign(newer))
            self.commit('upgrade',a,pa);self.worker(b,pb,3);self.compare(a,b)
            self.assertEqual((a/'demo/.personal').read_bytes(),b'personal')
            self.assertEqual((a/'demo/hello').read_text(),'new version')
            self.assertEqual(tree(a/'demo'),tree(b/'demo'))
            self.commit('remove',a,'demo');self.worker(b,'demo',1);self.compare(a,b)
            self.assertEqual((a/'demo/.personal').read_bytes(),b'personal')
            self.assertFalse((a/'demo/hello').exists())
            self.assertEqual(tree(a/'demo'),tree(b/'demo'))
            self.assertEqual(attributes.observed(a/'demo')['comment'],b'personal drawer'.hex())
