# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Installed verification observes signed declarations and never repairs them."""
import ctypes as C
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import *
from test_client import CLI, WORKER
from test_operations import install, payload
from test_attributes import attrs, putattr, encoded, ATTR

lib.apkg_client_verify.argtypes = [C.c_char_p, C.c_char_p, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_verify.restype = C.c_int


def verify(root, name='demo', observer=None):
    out = C.c_void_p()
    code = lib.apkg_client_verify(os.fsencode(root), name.encode(), observer, C.byref(out), None)
    text = C.string_at(out).decode() if out else None
    lib.apkg_client_text_free(out)
    return code, text


def setup(root, name='demo'):
    state(root, policy=policy()); code, ctx = open_ctx(root, 1); assert code == 0
    body = json.loads(descriptor(name=name))['body']
    config = payload('settings', 'original'); config['config'] = 'preserve'
    body['files'] = [dict(path='empty', type='directory'), body['files'][0], config]
    install(ctx, wire('descriptor', body)); lib.apkg_close(ctx)
    return (root / '.apkg/state').read_bytes()


def worker(root, name='demo', choice=0):
    p = subprocess.run([str(WORKER), str(root), name, '47', str(choice)], capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, p.stdout + p.stderr
    header, _, text = p.stdout.partition('\n')
    return int(header.split()[1]), text


class Verify(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fail_alloc(0)

    def test_cli_and_worker_report_edits_missing_and_unknown_without_repair(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'; before = setup(root)
            self.assertIn('ok 3\nchanged 0\nmissing 0\nunknown 0', verify(root)[1])
            (root / 'demo/hello').unlink(); (root / 'demo/settings').write_bytes(b'edited')
            (root / 'demo/personal').write_bytes(b'keep')
            code, text = verify(root); self.assertEqual(code, 0)
            self.assertIn('ok 1\nchanged 1\nmissing 1\nunknown 1', text)
            self.assertIn('changed settings [configuration]', text)
            self.assertIn('unknown personal', text)
            self.assertEqual(worker(root), (0, text))
            p = subprocess.run([str(CLI), 'verify', str(root), 'demo'], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout.removesuffix('status=0 outcome=0\n').rstrip('\n'), text.rstrip('\n'))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertEqual((root / 'demo/settings').read_bytes(), b'edited')
            self.assertFalse((root / 'demo/hello').exists())
            self.assertEqual((root / 'demo/personal').read_bytes(), b'keep')

    def test_permissions_amiga_metadata_and_directory_types_are_verified(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'; before = setup(root)
            (root / 'demo/hello').chmod(0o600)
            putattr(root / 'demo/settings', ATTR, encoded(attrs()))
            (root / 'demo/empty').rmdir(); (root / 'demo/empty').write_bytes(b'not a directory')
            code, text = verify(root); self.assertEqual(code, 0)
            self.assertIn('ok 0\nchanged 3\nmissing 0', text)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_missing_drawer_and_unknown_name_are_distinct(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'; setup(root)
            shutil.rmtree(root / 'demo')
            self.assertIn('missing 3\nunknown 0', verify(root)[1])
            self.assertEqual(verify(root, 'absent'), (5, None))
            self.assertFalse((root / 'demo').exists())

    def test_link_fifo_and_interrupted_root_refuse_without_partial_report(self):
        for kind in ('link', 'fifo', 'rootlink', 'pending'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp) / 'root'; before = setup(root)
                if kind == 'pending':
                    Path(str(root) + '.apkg-transaction').mkdir()
                elif kind == 'rootlink':
                    shutil.rmtree(root / 'demo'); (root / 'demo').symlink_to(root / '.apkg', target_is_directory=True)
                elif kind == 'link': (root / 'demo/external').symlink_to(root / '.apkg/state')
                else: os.mkfifo(root / 'demo/pipe')
                code, text = verify(root)
                self.assertNotEqual(code, 0); self.assertIsNone(text)
                self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_invalid_signature_refuses_but_withdrawn_author_stays_a_diagnostic_fact(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'; setup(root)
            path = root / '.apkg/state'; obj = json.loads(path.read_bytes())
            obj['body']['policy'] = json.loads(wire('policy', dict(authorities=[])))
            path.write_bytes(wire('root', obj['body']))
            code, text = verify(root); self.assertEqual(code, 0)
            self.assertIn('Author trust: rejected', text)
            obj['body']['installed'][0]['signature']['signature'] = '00' * 64
            path.write_bytes(wire('root', obj['body']))
            self.assertEqual(verify(root), (11, None))

    def test_full_package_name_and_oversized_report_are_not_truncated(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'; name = 'n' * 255; before = setup(root, name)
            code, text = verify(root, name); self.assertEqual(code, 0)
            self.assertIn(name, text); self.assertEqual(worker(root, name), (0, text))
            for i in range(300): (root / name / ('p%03d-' % i + 'x' * 225)).write_bytes(b'personal')
            self.assertEqual(verify(root, name), (3, None))
            self.assertEqual(worker(root, name), (3, ''))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertEqual(len(list((root / name).iterdir())), 303)

    def test_allocation_failure_cancellation_and_public_message_failure_are_readonly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'; before = setup(root)
            for nth in range(1, 161):
                lib.apkg_test_fail_alloc(nth)
                try: code, text = verify(root)
                finally: lib.apkg_test_fail_alloc(0)
                self.assertIn(code, (0, 4)); self.assertEqual(text is None, code != 0)
            self.assertEqual(worker(root, choice=1), (16, ''))
            self.assertEqual(worker(root, choice=2), (4, ''))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

if __name__ == '__main__': unittest.main()
