# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""CLI and native query report the same current system requirement facts."""
import ctypes as C
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from support import *
from test_channel import CHANNEL, entry, grant, read_channel, seed
from test_client import cli
from test_rexx_catalogue import publish, query
from test_system import req, wanting

lib.apkg_package_requirements.argtypes = [C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_package_requirements.restype = C.c_int
lib.apkg_channel_requirements.argtypes = [C.c_void_p, C.c_void_p, C.c_char_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_channel_requirements.restype = C.c_int


def report(pkg):
    out = C.c_void_p()
    code = lib.apkg_package_requirements(pkg, C.byref(out), None)
    text = C.string_at(out).decode() if out else None
    lib.apkg_client_text_free(out)
    return code, text


class RequirementsQuery(unittest.TestCase):
    def tearDown(self):
        lib.apkg_test_system(None); lib.apkg_test_fail_alloc(0)

    def test_live_missing_unknown_and_satisfied_facts_do_not_make_a_plan_or_mutate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'; before = state(root, policy=seed())
            code, ctx = open_ctx(root); self.assertEqual(code, 0)
            data = wanting([req('SomeType', kind='datatype'), req('needed.library', 3)])
            code, pkg = package(ctx, data, sign(data)); self.assertEqual(code, 0)
            for specification, verdict in ((b'needed.library=3', 'satisfied'), (b'needed.library=2', 'missing'), (b'', 'missing')):
                lib.apkg_test_system(specification)
                code, text = report(pkg); self.assertEqual(code, 0)
                self.assertIn('library needed.library (minimum 3): ' + verdict, text)
                self.assertIn('datatype SomeType (minimum 0): undetermined', text)
            lib.apkg_close(ctx)
            self.assertEqual(report(pkg)[0], 0)  # immutable handle owns the declaration
            for nth in range(1, 15):
                lib.apkg_test_fail_alloc(nth)
                try: code, text = report(pkg)
                finally: lib.apkg_test_fail_alloc(0)
                self.assertIn(code, (0, 4)); self.assertEqual(text is None, code != 0)
            lib.apkg_package_free(pkg)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertEqual(sorted(p.name for p in root.parent.iterdir()), ['root'])

    def test_cli_and_native_choose_newest_revision_and_query_retained_metadata_offline(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            before = state(root, policy=seed())
            old = wanting([req('old.library')], '1.0')
            new = wanting([req('new.library')], '2.0')
            revision = json.loads(new); revision['body']['revision'] = '2'
            revision['body']['requires_system'] = [req('revision.library')]
            publish(endpoint, [entry(old), entry(new), entry(wire('descriptor', revision['body']))])
            result = cli('requirements', root, endpoint, CHANNEL, 'demo')
            self.assertEqual(result.returncode, 0, result.stderr)
            expected = result.stdout.removesuffix('status=0 outcome=0\n').rstrip('\n')
            self.assertIn('2.0 (revision 2)', expected)
            self.assertIn('revision.library', expected); self.assertNotIn('new.library', expected)
            self.assertIn('undetermined', expected)
            self.assertEqual(query(root, endpoint, 'demo', mode=46, change=lambda: shutil.rmtree(endpoint)), (0, expected))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertEqual(sorted(p.name for p in d.iterdir()), ['root'])

    def test_ambiguous_versions_unknown_name_and_withdrawn_trust_refuse(self):
        for versions in (('nightly', 'weekly'), ('1.0', '1.0.0')):
            with self.subTest(versions=versions), tempfile.TemporaryDirectory() as tmp:
                d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
                state(root, policy=seed())
                publish(endpoint, [entry(wanting([req('a.library')], versions[0])), entry(wanting([req('b.library')], versions[1]))])
                self.assertEqual(query(root, endpoint, 'demo', mode=46), (14, ''))
                self.assertEqual(query(root, endpoint, 'absent', mode=46), (5, ''))
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root, policy=seed()); publish(endpoint, [entry(wanting(None))])
            self.assertEqual(query(root, endpoint, 'demo', mode=46, change=lambda: state(root, 2, policy=seed(authors=False))), (12, ''))

    def test_different_trusted_authors_and_wrong_target_refuse_without_root_changes(self):
        other = '12' * 16; pk, sk = keypair(bytes([3]) * 32)
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            policy_data = json.loads(seed())
            policy_data['body']['authorities'].append(grant(other, pk))
            policy_data['body']['authorities'].sort(key=lambda row: row['id'])
            before = state(root, policy=wire('policy', policy_data['body']))
            foreign = json.loads(wanting(None)); foreign['body']['authority'] = other
            data = wire('descriptor', foreign['body'])
            publish(endpoint, [entry(wanting(None)), entry(data, sign(data, pk, sk))])
            self.assertEqual(query(root, endpoint, 'demo', mode=46), (14, ''))
            target_endpoint = d / 'other-target'
            wrong = json.loads(wanting(None)); wrong['body']['target'] = 'i386-aros-v0-native'
            publish(target_endpoint, [entry(wire('descriptor', wrong['body']))])
            self.assertEqual(query(root, target_endpoint, 'demo', mode=46), (13, ''))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_identical_opaque_versions_use_revision_and_empty_requirements_are_explicit(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root, policy=seed())
            older = wanting([req('obsolete.library')], 'nightly')
            newer = json.loads(wanting(None, 'nightly')); newer['body']['revision'] = '2'
            publish(endpoint, [entry(older), entry(wire('descriptor', newer['body']))])
            code, text = query(root, endpoint, 'demo', mode=46)
            self.assertEqual(code, 0); self.assertIn('nightly (revision 2)', text)
            self.assertIn('No system requirements declared.', text)
            self.assertNotIn('obsolete.library', text)

    def test_retained_channel_bound_to_previous_root_policy_is_stale_even_without_name_match(self):
        from test_channel import catalog
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'; state(root, policy=seed())
            code, ctx = open_ctx(root); self.assertEqual(code, 0)
            code, channel = read_channel(ctx, catalog()); self.assertEqual(code, 0)
            state(root, 2, policy=seed(authors=False))
            out = C.c_void_p()
            self.assertEqual(lib.apkg_channel_requirements(ctx, channel, b'absent', C.byref(out), None), 8)
            self.assertFalse(out)
            lib.apkg_channel_free(channel); lib.apkg_close(ctx)

if __name__ == '__main__':
    unittest.main()
