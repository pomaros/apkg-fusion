# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Inspection reports captured facts without authorization or runtime claims."""
import ctypes as C
import subprocess
import tempfile
import unittest
from support import *
from test_client import CLI, WORKER
from test_operations import install

lib.apkg_snapshot_inspect.argtypes = [C.c_void_p, C.c_uint64, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_snapshot_inspect.restype = C.c_int
lib.apkg_inspection_free.argtypes = [C.c_void_p]
lib.apkg_inspection_format.argtypes = [C.c_void_p, C.c_char_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_inspection_format.restype = C.c_int
lib.apkg_client_info.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_info.restype = C.c_int


def info(root, name='demo', field=None):
    out = C.c_void_p()
    code = lib.apkg_client_info(str(root).encode(), name.encode(),
        field.encode() if field is not None else None, C.byref(out), None)
    value = C.string_at(out).decode() if out else None
    lib.apkg_client_text_free(out)
    return code, value


def inspect(snapshot, index=0):
    out = C.c_void_p()
    code = lib.apkg_snapshot_inspect(snapshot, index, C.byref(out), None)
    return code, out


def formatted(inspection, field=None):
    out = C.c_void_p()
    code = lib.apkg_inspection_format(inspection, field.encode() if field else None,
        C.byref(out), None)
    value = C.string_at(out).decode() if out else None
    lib.apkg_client_text_free(out)
    return code, value


def fixture(root, name='demo'):
    state(root, policy=policy())
    code, ctx = open_ctx(root, 1)
    assert code == 0
    try:
        body = json.loads(descriptor(name=name))['body']
        body['files'].insert(0, dict(path='empty', type='directory'))
        install(ctx, wire('descriptor', body))
    finally:
        lib.apkg_close(ctx)
    return (root / '.apkg/state').read_bytes()


def document(root, body):
    record = json.loads((root / '.apkg/state').read_bytes())['body']
    data = wire('descriptor', body)
    record['installed'][0]['descriptor'] = json.loads(data)
    record['installed'][0]['signature'] = json.loads(sign(data))
    (root / '.apkg/state').write_bytes(wire('root', record))


class Inspection(unittest.TestCase):
    def tearDown(self):
        lib.apkg_test_fail_alloc(0)

    def test_complete_facts_are_readonly_and_cli_native_adapter_equivalent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            before = fixture(root)
            code, value = info(root)
            self.assertEqual(code, 0)
            fields = dict(line.split(' ', 1) for line in value.splitlines())
            for field, expected in dict(id='demo', kind='data', arch='x86_64', abi='v11',
                    signature='verified', authortrust='accepted', compatibility='target-match',
                    review='not-asserted', runtime='not-asserted', payloadverification='not-checked',
                    requirements='not-checked', canupgrade='unknown', canrollback='unknown',
                    size='unknown', summary='unknown', payloadbytes='6', files='1', directories='1',
                    rootstate='ready', generation='2').items():
                self.assertEqual(fields[field], expected)
            self.assertEqual(fields['snapshot'], hashlib.sha256(before).hexdigest())
            self.assertEqual(json.loads(fields['relations']), dict(depends=[], recommends=[], conflicts=[], replaces=[]))
            result = subprocess.run([str(CLI), 'info', str(root), 'demo'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.split('status=')[0].rstrip(), value)
            for mode in ('14', '15'):
                result = subprocess.run([str(WORKER), str(root), 'demo', mode], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout, 'status 0\n' + value + '\n')
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertTrue((root / 'demo/empty').is_dir())
            self.assertFalse((root / 'db').exists())

    def test_captured_policy_lifetime_and_new_policy_revocation_are_distinct(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            fixture(root)
            code, ctx = open_ctx(root)
            code, snap = snapshot(ctx)
            code, inspection = inspect(snap)
            self.assertEqual(code, 0)
            lib.apkg_snapshot_free(snap)
            lib.apkg_close(ctx)
            body = json.loads((root / '.apkg/state').read_bytes())['body']
            body['policy'] = json.loads(wire('policy', dict(authorities=[])))
            (root / '.apkg/state').write_bytes(wire('root', body))
            self.assertEqual(formatted(inspection, 'authortrust'), (0, 'accepted'))
            self.assertEqual(info(root, field='authortrust'), (0, 'rejected'))
            self.assertEqual(info(root, field='signature'), (0, 'verified'))
            lib.apkg_inspection_free(inspection)

    def test_signature_invalid_target_mismatch_and_live_edits_never_become_qualifications(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            fixture(root)
            (root / 'demo/hello').write_text('personal edit')
            self.assertEqual(info(root, field='signature'), (0, 'verified'))
            self.assertEqual(info(root, field='payloadverification'), (0, 'not-checked'))
            body = json.loads((root / '.apkg/state').read_bytes())['body']
            body['installed'][0]['descriptor']['body']['version'] = '2.0'
            (root / '.apkg/state').write_bytes(wire('root', body))
            self.assertEqual(info(root, field='signature'), (0, 'invalid'))
            self.assertEqual(info(root, field='authortrust'), (0, 'unknown'))
            desc = body['installed'][0]['descriptor']['body']
            desc['target'] = 'arm-aros-v11-native'
            document(root, desc)
            self.assertEqual(info(root, field='compatibility'), (0, 'target-mismatch'))
            self.assertEqual(info(root, field='review'), (0, 'not-asserted'))
            self.assertEqual(info(root, field='runtime'), (0, 'not-asserted'))
            self.assertEqual((root / 'demo/hello').read_text(), 'personal edit')

    def test_full_values_relations_and_rexx_size_boundary_are_not_truncated(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            name = 'a' * 255
            fixture(root, name)
            body = json.loads(descriptor(name=name))['body']
            body.update(version='v' * 255, revision='18446744073709551615')
            relations = dict(depends=[], recommends=[], conflicts=[], replaces=[])
            for i in range(600):
                relations['depends'].append(dict(authority=AUTH, name='provider-%04d' % i,
                    target=TARGET, min_version='1.0', min_revision='18446744073709551615'))
            body['relations'] = relations
            document(root, body)
            self.assertEqual(info(root, name, 'id'), (0, name))
            self.assertEqual(info(root, name, 'VERSION'), (0, 'v' * 255))
            self.assertEqual(info(root, name, 'revision'), (0, '18446744073709551615'))
            code, text = info(root, name, 'relations')
            self.assertEqual(code, 0)
            self.assertEqual(json.loads(text), relations)
            self.assertGreater(len(text), 65535)
            result = subprocess.run([str(WORKER), str(root), name, '14'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout, 'status 3\n')
            result = subprocess.run([str(WORKER), str(root), name, '14', 'signature'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout, 'status 0\nverified\n')
            result = subprocess.run([str(WORKER), str(root), name, '15', 'relations'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout, 'status 0\n' + text + '\n')

    def test_pending_absent_legacy_unknown_names_and_fields_stay_readonly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            self.assertEqual(info(root), (17, None))
            self.assertFalse(root.exists())
            before = fixture(root)
            self.assertEqual(info(root, 'missing'), (5, None))
            self.assertEqual(info(root, field='not-a-field'), (1, None))
            pending = Path(str(root) + '.apkg-transaction')
            pending.mkdir()
            self.assertEqual(info(root, field='rootstate'), (0, 'pending'))
            self.assertEqual(info(root, field='signature'), (0, 'verified'))
            self.assertTrue(pending.is_dir())
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            legacy = Path(tmp) / 'legacy'
            (legacy / 'db').mkdir(parents=True)
            self.assertEqual(info(legacy), (17, None))
            self.assertFalse((legacy / '.apkg').exists())

    def test_payload_counter_overflow_is_refused_without_wrapping_or_io(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            files = [dict(path=name, type='file', mode='420', size=str(size),
                sha256='ab' * 32, blob='ab' * 32) for name, size in [('a', 2**64-1), ('b', 1)]]
            data = descriptor(files=files)
            before = state(root, policy=policy(), installed=[dict(
                descriptor=json.loads(data), signature=json.loads(sign(data)))])
            self.assertEqual(info(root, field='payloadbytes'), (3, None))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertFalse(Path(str(root) + '.apkg-objects').exists())

    def test_allocation_failures_leave_no_partial_output_and_can_retry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            before = fixture(root)
            code, ctx = open_ctx(root)
            code, snap = snapshot(ctx)
            for nth in range(1, 81):
                lib.apkg_test_fail_alloc(nth)
                code, inspection = inspect(snap)
                lib.apkg_test_fail_alloc(0)
                self.assertIn(code, (0, 4), nth)
                if code:
                    self.assertFalse(inspection)
                else:
                    for failure in range(1, 10):
                        lib.apkg_test_fail_alloc(failure)
                        code, text = formatted(inspection)
                        lib.apkg_test_fail_alloc(0)
                        self.assertIn(code, (0, 4))
                        if code:
                            self.assertIsNone(text)
                lib.apkg_inspection_free(inspection)
            self.assertEqual(inspect(snap, 99)[0], 5)
            self.assertEqual(inspect(None)[0], 1)
            lib.apkg_snapshot_free(snap)
            lib.apkg_close(ctx)
            for nth in range(1, 181):
                lib.apkg_test_fail_alloc(nth)
                code, text = info(root)
                lib.apkg_test_fail_alloc(0)
                self.assertIn(code, (0, 4), nth)
                if code:
                    self.assertIsNone(text)
            self.assertEqual(info(root, field='signature'), (0, 'verified'))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
