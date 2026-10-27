# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import json
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import *
from test_operations import install, op, run_plan
import test_client as clients

lib.apkg_plan_policy.argtypes = [C.c_void_p, C.c_void_p, Text, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_plan_policy.restype = C.c_int
lib.apkg_plan_format.argtypes = [C.c_void_p, C.POINTER(C.c_void_p)]
lib.apkg_plan_format.restype = C.c_int
lib.apkg_client_text_free.argtypes = [C.c_void_p]
PK2, SK2 = keypair(bytes([7]) * 32)

def rotated(*keys, reviewers=None):
    body = dict(authorities=[dict(id=AUTH, keys=sorted([dict(algorithm='ed25519', public_key=k.hex()) for k in keys], key=lambda k: k['public_key']))])
    if reviewers:
        body['reviewers'] = reviewers
    return wire('policy', body)

def plan_policy(c, data):
    code, s = snapshot(c); assert code == 0
    buf = C.create_string_buffer(data); p = C.c_void_p()
    code = lib.apkg_plan_policy(c, s, Text(C.cast(buf, C.c_void_p), len(data)), C.byref(p), None)
    lib.apkg_snapshot_free(s); return code, p

def presentation(p):
    text = C.c_void_p(); assert lib.apkg_plan_format(p, C.byref(text)) == 0
    out = C.string_at(text).decode(); lib.apkg_client_text_free(text); return out

def body(root): return json.loads((root / '.apkg/state').read_bytes())['body']

class TrustPolicy(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fault(0, 0)

    def test_rotation_shows_every_grant_and_lost_trust_then_commits_state_only(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root); install(c, descriptor()); before = body(root)
            code, p = plan_policy(c, rotated(PK2)); self.assertEqual(code, 0)
            text = presentation(p)
            self.assertIn('Change trust policy', text)
            self.assertIn('Grant package authority %s key %s' % (AUTH, PK2.hex()), text)
            self.assertIn('Revoke package authority %s key %s' % (AUTH, PK.hex()), text)
            self.assertIn('Installed package demo loses author trust', text)
            self.assertEqual(run_plan(c, p), (0, 1)); lib.apkg_plan_free(p)
            after = body(root)
            self.assertEqual(after['policy'], json.loads(rotated(PK2)))
            self.assertEqual(after['installed'], before['installed'])
            self.assertEqual(int(after['generation']), int(before['generation']) + 1)
            self.assertEqual(after['package_generation'], before['package_generation'])
            self.assertTrue((root / 'demo/hello').exists())
            lib.apkg_close(c)

    def test_adding_a_role_keeps_trust_and_reports_no_loss(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root); install(c, descriptor())
            reviewers = [dict(id='cd' * 16, keys=[dict(algorithm='ed25519', public_key=PK2.hex())])]
            code, p = plan_policy(c, rotated(PK, reviewers=reviewers)); self.assertEqual(code, 0)
            text = presentation(p)
            self.assertIn('Grant reviewer %s key %s' % ('cd' * 16, PK2.hex()), text)
            self.assertNotIn('Revoke', text); self.assertNotIn('loses author trust', text)
            self.assertEqual(run_plan(c, p), (0, 1)); lib.apkg_plan_free(p); lib.apkg_close(c)

    def test_unchanged_invalid_noncanonical_and_readonly_refuse_without_state_change(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root); before = (root / '.apkg/state').read_bytes()
            self.assertEqual(plan_policy(c, policy())[0], 1)
            self.assertEqual(plan_policy(c, wire('policy', dict(authorities=[dict(id=AUTH, keys=[])])))[0], 17)
            self.assertEqual(plan_policy(c, rotated(PK2) + b' ')[0], 17)
            code, r = open_ctx(root); self.assertEqual(plan_policy(r, rotated(PK2))[0], 17); lib.apkg_close(r)
            self.assertEqual((root / '.apkg/state').read_bytes(), before); lib.apkg_close(c)

    def test_a_plan_from_an_older_snapshot_is_stale(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root)
            code, p = plan_policy(c, rotated(PK2)); self.assertEqual(code, 0)
            install(c, descriptor()); before = (root / '.apkg/state').read_bytes()
            code, a = approve(c, p); self.assertEqual(execute(c, p, a)[0], 8)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            lib.apkg_approval_free(a); lib.apkg_plan_free(p); lib.apkg_close(c)

    def test_rollback_never_crosses_a_policy_change(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root); install(c, descriptor())
            install_history = next(path for path in root.parent.glob(root.name + '.apkg-history.*')
                                   if (path / 'new-state').exists() and (path / 'new-state').read_bytes() == (root / '.apkg/state').read_bytes())
            code, p = plan_policy(c, rotated(PK, PK2)); self.assertEqual(run_plan(c, p), (0, 1)); lib.apkg_plan_free(p)
            policy_history = next(path for path in root.parent.glob(root.name + '.apkg-history.*')
                                  if (path / 'new-state').exists() and (path / 'new-state').read_bytes() == (root / '.apkg/state').read_bytes())
            before = (root / '.apkg/state').read_bytes()
            self.assertNotEqual(op(c, 'rollback', str(install_history))[0], 0)
            self.assertEqual(op(c, 'rollback', str(policy_history))[0], 17)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertEqual(body(root)['policy'], json.loads(rotated(PK, PK2)))
            lib.apkg_close(c)

    def test_interrupted_change_recovers_to_one_whole_policy(self):
        # 1/2 precede publication of the ready journal's decision; 4/5 follow it.
        for point, expected in ((1, None), (2, None), (4, 'new'), (5, 'new')):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                root = Path(d) / 'root'; c = init(root); old = body(root)['policy']
                code, p = plan_policy(c, rotated(PK2)); code, a = approve(c, p); pid = os.fork()
                if pid == 0:
                    lib.apkg_test_fault(point, 2); execute(c, p, a); os._exit(98)
                _, status = os.waitpid(pid, 0); self.assertEqual(os.waitstatus_to_exitcode(status), 99)
                self.assertEqual(recover(c)[0], 0)
                self.assertEqual(body(root)['policy'], json.loads(rotated(PK2)) if expected else old)
                lib.apkg_approval_free(a); lib.apkg_plan_free(p); lib.apkg_close(c)

    def test_cli_previews_then_applies_the_confirmed_policy(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root); lib.apkg_close(c)
            path = Path(d) / 'policy.json'; path.write_bytes(rotated(PK, PK2))
            run = lambda *a: subprocess.run([str(clients.CLI), 'policy', str(root), str(path), *a], capture_output=True, text=True, timeout=60)
            first = run(); self.assertNotEqual(first.returncode, 0); self.assertIn('Grant package authority', first.stdout)
            digest = re.search(r'--confirm ([0-9a-f]{64})', first.stderr).group(1)
            done = run('--confirm', digest); self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual(body(root)['policy'], json.loads(rotated(PK, PK2)))

if __name__ == '__main__':
    unittest.main()
