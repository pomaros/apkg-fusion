# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import *
import test_client as clients
from test_author import author_fixture, key_fixture, build
from test_channel import CHANNEL, seed
from test_file_channel import channel_key, publish
from test_channel_clients import CONFIRM, apply, selection, committed

lib.apkg_client_self_update.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_char_p, C.c_int,
    CONFIRM, C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p), C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_self_update.restype = C.c_int
lib.apkg_client_text_free.argtypes = [C.c_void_p]

def release(d, endpoint, version, body, path='program', key=None, ckey=None):
    # Every release uses the same drawer name, as a real client does.
    (d / version).mkdir(); source = d / version / 'Client'
    author_fixture(source, name='client', version=version, kind='tool')
    if path != 'program':
        (source / 'program').rename(source / path)
        ameta = source / '.ameta'; ameta.write_text(ameta.read_text().replace('file program', 'file ' + path))
    (source / path).write_bytes(body)
    archive = d / version / 'client.pkg'
    result = build(source, archive, key or d / 'author-key')
    assert result.returncode == 0, result.stderr
    result = publish(archive, endpoint, ckey or d / 'channel-key')
    assert result.returncode == 0, result.stderr

def fixture(d):
    key_fixture(d / 'author-key'); channel_key(d / 'channel-key'); endpoint = d / 'channel'
    release(d, endpoint, '1.2', b'client one')
    root = d / 'root'; state(root, policy=seed())
    assert apply(root, endpoint, selection(endpoint, 'client', '1.2')) == (0, 1)
    return root, endpoint, root / 'client' / 'Client' / 'program'

def self_update(root, endpoint, executable, check=False, accept=True):
    seen = []
    def decide(user, plan, text):
        seen.append(text); return 1 if accept else 0
    confirm = CONFIRM(decide); report = C.c_void_p(); out = C.c_void_p()
    code = lib.apkg_client_self_update(str(root).encode(), str(endpoint).encode(), CHANNEL.encode(),
        str(executable).encode(), int(check), confirm, None, None, C.byref(report),
        None if check else C.byref(out), None)
    text = C.string_at(report).decode() if report else ''
    lib.apkg_client_text_free(report)
    outcome = lib.apkg_result_get(out).contents.outcome if out else None
    lib.apkg_result_free(out)
    return code, outcome, text, seen

class SelfUpdate(unittest.TestCase):
    def tearDown(self):
        lib.apkg_test_fault(0, 0)
        os.environ.pop('APKG_LEGACY_SELF_UPDATE_JOURNAL', None)
        os.environ.pop('APKG_LEGACY_SELF_UPDATE_COPIES', None)

    def test_current_client_reports_current_and_changes_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            root, endpoint, exe = fixture(Path(d)); before = (root / '.apkg/state').read_bytes()
            code, outcome, text, seen = self_update(root, endpoint, exe)
            self.assertEqual(code, 0); self.assertIn('is current', text); self.assertEqual(seen, [])
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_check_reports_the_newer_release_without_planning(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); root, endpoint, exe = fixture(d); release(d, endpoint, '2.0', b'client two')
            before = (root / '.apkg/state').read_bytes()
            code, outcome, text, seen = self_update(root, endpoint, exe, check=True)
            self.assertEqual(code, 0); self.assertIn('1.2 -> 2.0', text); self.assertIn('restart=required', text)
            self.assertEqual(seen, []); self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_update_goes_through_the_common_journal_and_rolls_back_offline(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); root, endpoint, exe = fixture(d); release(d, endpoint, '2.0', b'client two')
            declined = self_update(root, endpoint, exe, accept=False)
            self.assertEqual(declined[0], 16); self.assertEqual(exe.read_bytes(), b'client one')
            code, outcome, text, seen = self_update(root, endpoint, exe)
            self.assertEqual((code, outcome), (0, 1), text); self.assertEqual(len(seen), 1)
            self.assertEqual(exe.read_bytes(), b'client two')
            names = [r['descriptor']['body']['version'] for r in committed(root)['installed']]
            self.assertEqual(names, ['2.0'])
            history = next(path for path in root.parent.glob(root.name + '.apkg-history.*')
                if (path / 'decision').exists() and (path / 'new-state').read_bytes() == (root / '.apkg/state').read_bytes())
            shutil.rmtree(endpoint)
            from test_operations import op, run_plan
            code, c = open_ctx(root, 1); code, p = op(c, 'rollback', str(history)); self.assertEqual(code, 0)
            self.assertEqual(run_plan(c, p), (0, 1)); lib.apkg_plan_free(p); lib.apkg_close(c)
            self.assertEqual(exe.read_bytes(), b'client one')

    def test_interrupted_update_recovers_to_one_whole_release(self):
        # 101/102 precede the commit decision, 4/5 follow it.
        for point, version, body in ((101, '1.2', b'client one'), (102, '1.2', b'client one'),
                                     (4, '2.0', b'client two'), (5, '2.0', b'client two')):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                d = Path(d); root, endpoint, exe = fixture(d); release(d, endpoint, '2.0', b'client two')
                pid = os.fork()
                if pid == 0:
                    lib.apkg_test_fault(point, 2); self_update(root, endpoint, exe); os._exit(98)
                _, status = os.waitpid(pid, 0); self.assertEqual(os.waitstatus_to_exitcode(status), 99)
                self.assertEqual(self_update(root, endpoint, exe, check=True)[0], 9)
                code, c = open_ctx(root, 1); self.assertEqual(recover(c)[0], 0); lib.apkg_close(c)
                self.assertEqual(exe.read_bytes(), body)
                self.assertEqual([r['descriptor']['body']['version'] for r in committed(root)['installed']], [version])
                if version == '1.2':
                    self.assertEqual(self_update(root, endpoint, exe)[:2], (0, 1)); self.assertEqual(exe.read_bytes(), b'client two')

    def test_edited_client_is_refused_before_planning(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); root, endpoint, exe = fixture(d); release(d, endpoint, '2.0', b'client two')
            exe.write_bytes(b'locally patched'); before = (root / '.apkg/state').read_bytes()
            code, outcome, text, seen = self_update(root, endpoint, exe)
            self.assertEqual(code, 14); self.assertEqual(seen, [])
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_unowned_or_outside_client_is_not_found(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); root, endpoint, exe = fixture(d); release(d, endpoint, '2.0', b'client two')
            stray = root / 'client' / 'Client' / 'stray'; stray.write_bytes(b'x')
            outside = d / 'apkg'; outside.write_bytes(b'client one')
            for path in (stray, outside):
                with self.subTest(path=path.name):
                    self.assertEqual(self_update(root, endpoint, path)[0], 5)

    def test_unsettled_legacy_self_update_and_recorded_copies_refuse(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); root, endpoint, exe = fixture(d); release(d, endpoint, '2.0', b'client two')
            journal = d / 'self-update.json'; journal.write_text('{}')
            os.environ['APKG_LEGACY_SELF_UPDATE_JOURNAL'] = str(journal)
            self.assertEqual(self_update(root, endpoint, exe)[0], 9)
            os.environ.pop('APKG_LEGACY_SELF_UPDATE_JOURNAL')
            copies = d / 'copies.json'; copies.write_text('{"C:apkg":"Work:arospkg"}')
            os.environ['APKG_LEGACY_SELF_UPDATE_COPIES'] = str(copies)
            self.assertEqual(self_update(root, endpoint, exe)[0], 2)
            copies.write_text('{}')
            self.assertEqual(self_update(root, endpoint, exe, check=True)[0], 0)

    def test_release_without_the_client_at_the_same_path_refuses(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); root, endpoint, exe = fixture(d); release(d, endpoint, '2.0', b'client two', path='renamed')
            before = (root / '.apkg/state').read_bytes()
            self.assertEqual(self_update(root, endpoint, exe)[0], 2)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_pkgmanager_worker_checks_then_updates_through_the_same_path(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); root, endpoint, exe = fixture(d); release(d, endpoint, '2.0', b'client two')
            run = lambda check: subprocess.run([str(clients.WORKER), str(root), str(endpoint), '40', check, CHANNEL, str(exe)], capture_output=True, text=True, timeout=60)
            check = run('1'); self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
            self.assertIn('status 0 outcome 0 previews 0', check.stdout); self.assertIn('1.2 -> 2.0', check.stdout)
            self.assertEqual(exe.read_bytes(), b'client one')
            done = run('0'); self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn('status 0 outcome 1 previews 1', done.stdout); self.assertEqual(exe.read_bytes(), b'client two')

    def test_cli_check_then_confirmed_update(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); root, endpoint, exe = fixture(d); release(d, endpoint, '2.0', b'client two')
            run = lambda *a: subprocess.run([str(clients.CLI), 'self-update', str(root), str(endpoint), CHANNEL, str(exe), *a], capture_output=True, text=True, timeout=60)
            check = run('--check'); self.assertEqual(check.returncode, 0, check.stderr); self.assertIn('1.2 -> 2.0', check.stdout)
            first = run(); self.assertNotEqual(first.returncode, 0)
            digest = re.search(r'--confirm ([0-9a-f]{64})', first.stderr).group(1)
            done = run('--confirm', digest); self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual(exe.read_bytes(), b'client two')

if __name__ == '__main__':
    unittest.main()
