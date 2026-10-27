# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Signed system requirements: probed when a plan brings a package in, bound
into the approved plan, re-probed at execution, never satisfied by a package."""
import ctypes as C
import json
import os
import tempfile
import unittest
from pathlib import Path
from support import *
from test_operations import install, run_plan
from test_policy import presentation
from test_author import author_fixture, key_fixture, build, members
from test_payload import import_package

lib.apkg_test_system.argtypes = [C.c_char_p]
lib.apkg_plan_upgrade.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_plan_upgrade.restype = C.c_int
REQUIREMENT = 19

def req(ident, minimum=0, kind='library'): return dict(id=ident, min_version=str(minimum), type=kind)

def wanting(reqs, version='1.0'):
    obj = json.loads(descriptor()); obj['body']['version'] = version
    if reqs is not None: obj['body']['requires_system'] = reqs
    return json.dumps(obj, sort_keys=True, separators=(',', ':')).encode()

def plan_install(c, data):
    code, s = snapshot(c); assert code == 0
    code, pkg = package(c, data, sign(data))
    if code: lib.apkg_snapshot_free(s); return code, None
    code, p = install_plan(c, s, pkg); lib.apkg_package_free(pkg); lib.apkg_snapshot_free(s)
    return code, p

def canonical(p):
    v = lib.apkg_plan_get(p).contents; return json.loads(C.string_at(v.canonical.data, v.canonical.size))

class SystemRequirements(unittest.TestCase):
    def setUp(self): lib.apkg_test_system(b'present.library=3')
    def tearDown(self): lib.apkg_test_system(None); lib.apkg_test_fault(0, 0)

    def test_missing_or_too_old_refuses_before_any_change(self):
        for reqs in ([req('absent.library')], [req('present.library', 4)]):
            with self.subTest(reqs=reqs), tempfile.TemporaryDirectory() as d:
                root = Path(d) / 'root'; c = init(root); before = (root / '.apkg/state').read_bytes()
                code, p = plan_install(c, wanting(reqs)); self.assertEqual(code, REQUIREMENT)
                self.assertEqual((root / '.apkg/state').read_bytes(), before); self.assertFalse((root / 'demo').exists())
                lib.apkg_close(c)

    def test_observations_are_bound_shown_and_installed(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root)
            code, p = plan_install(c, wanting([req('MyType', 0, 'datatype'), req('present.library', 2)])); self.assertEqual(code, 0)
            observed = canonical(p)['body']['requirements']
            self.assertEqual([(o['type'], o['id'], o['state'], o['version']) for o in observed],
                             [('datatype', 'MyType', 'undetermined', ''), ('library', 'present.library', 'satisfied', '3')])
            text = presentation(p)
            self.assertIn('demo needs library present.library version 2: satisfied', text)
            self.assertIn('demo needs datatype MyType: undetermined', text)
            self.assertEqual(run_plan(c, p), (0, 1)); lib.apkg_plan_free(p)
            self.assertTrue((root / 'demo/hello').exists()); lib.apkg_close(c)

    def test_a_package_without_requirements_plans_exactly_as_before(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root)
            code, p = plan_install(c, descriptor()); self.assertEqual(code, 0)
            self.assertNotIn('requirements', canonical(p)['body']); self.assertNotIn('System requirements', presentation(p))
            lib.apkg_plan_free(p); lib.apkg_close(c)

    def test_a_changed_machine_makes_the_approved_plan_stale(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root); before = (root / '.apkg/state').read_bytes()
            code, p = plan_install(c, wanting([req('present.library', 2)])); self.assertEqual(code, 0)
            code, a = approve(c, p); self.assertEqual(code, 0)
            lib.apkg_test_system(b'present.library=2')
            self.assertEqual(execute(c, p, a)[0], 8)
            self.assertEqual((root / '.apkg/state').read_bytes(), before); self.assertFalse((root / 'demo').exists())
            lib.apkg_approval_free(a); lib.apkg_plan_free(p); lib.apkg_close(c)

    def test_upgrade_probes_the_new_release(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root); install(c, descriptor())
            data = wanting([req('absent.library')], version='2.0')
            code, s = snapshot(c); code, pkg = package(c, data, sign(data)); self.assertEqual(code, 0)
            p = C.c_void_p(); self.assertEqual(lib.apkg_plan_upgrade(c, s, pkg, C.byref(p), None), REQUIREMENT)
            lib.apkg_package_free(pkg); lib.apkg_snapshot_free(s)
            data = wanting([req('present.library')], version='2.0')
            code, s = snapshot(c); code, pkg = package(c, data, sign(data))
            self.assertEqual(lib.apkg_plan_upgrade(c, s, pkg, C.byref(p), None), 0)
            self.assertEqual(canonical(p)['body']['requirements'][0]['state'], 'satisfied')
            self.assertEqual(run_plan(c, p), (0, 1))
            lib.apkg_plan_free(p); lib.apkg_package_free(pkg); lib.apkg_snapshot_free(s); lib.apkg_close(c)

    def test_malformed_declarations_refuse_the_signed_package(self):
        bad = ([], [req('b.library'), req('a.library')], [req('a.library'), req('a.library')],
               [req('a.library', 70000)], [req('a.library', 0, 'Library')], [dict(req('a.library'), extra='1')],
               [req('/abs.library')], [req('x%d.library' % i) for i in range(17)])
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root)
            for reqs in bad:
                with self.subTest(reqs=reqs):
                    data = wanting(sorted(reqs, key=lambda r: (r['type'], r['id'])) if len(reqs) == 17 else reqs)
                    self.assertNotEqual(package(c, data, sign(data))[0], 0)
            lib.apkg_close(c)

    def test_interrupted_install_recovers_with_its_observations(self):
        for point in (1, 4):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                root = Path(d) / 'root'; c = init(root)
                code, p = plan_install(c, wanting([req('present.library')])); code, a = approve(c, p); pid = os.fork()
                if pid == 0:
                    lib.apkg_test_fault(point, 2); execute(c, p, a); os._exit(98)
                _, status = os.waitpid(pid, 0); self.assertEqual(os.waitstatus_to_exitcode(status), 99)
                self.assertEqual(recover(c)[0], 0)
                self.assertEqual((root / 'demo/hello').exists(), point == 4)
                lib.apkg_approval_free(a); lib.apkg_plan_free(p); lib.apkg_close(c)

    def test_toml_requirements_are_signed_and_a_host_reports_undetermined(self):
        lib.apkg_test_system(None)
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); source = d / 'Drawer'; mp = author_fixture(source); key = key_fixture(d / 'key')
            mp.write_text(mp.read_text() + '\n[[requires_system]]\ntype = "library"\nid = "zlib.library"\nmin_version = 2\n'
                          '\n[[requires_system]]\ntype = "library"\nid = "crt.library"\n')
            out = d / 'p.pkg'; result = build(source, out, key); self.assertEqual(result.returncode, 0, result.stderr)
            body = json.loads(members(out)['descriptor.json'])['body']
            self.assertEqual(body['requires_system'], [req('crt.library'), req('zlib.library', 2)])
            root = d / 'root'; c = init(root); code, pkg = import_package(c, out); self.assertEqual(code, 0)
            code, s = snapshot(c); code, p = install_plan(c, s, pkg); self.assertEqual(code, 0)
            self.assertIn('this host build cannot probe an AROS library', presentation(p))
            self.assertEqual(run_plan(c, p), (0, 1))
            lib.apkg_plan_free(p); lib.apkg_package_free(pkg); lib.apkg_snapshot_free(s); lib.apkg_close(c)
            out.unlink(); mp.write_text(mp.read_text() + '\n[[requires_system]]\ntype = "library"\nid = "crt.library"\n')
            self.assertNotEqual(build(source, out, key).returncode, 0); self.assertFalse(out.exists())

if __name__ == '__main__':
    unittest.main()
