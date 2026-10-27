# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""The actual native installed-list adapter retains complete common views."""
import subprocess
import tempfile
import unittest
from support import *
from test_client import WORKER
from test_operations import install


def query(root, term='', fail=0):
    result = subprocess.run([str(WORKER), str(root), term, '13', str(fail)],
        capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    lines = result.stdout.splitlines()
    code = int(lines[0].split()[1])
    return code, [line.split('\t', 4) for line in lines[1:]]


def installed(root):
    state(root, policy=policy())
    code, ctx = open_ctx(root, 1)
    assert code == 0
    names = ['a' * 255, 'a' * 254 + 'b']
    version = 'v' * 255
    try:
        for name in names:
            body = json.loads(descriptor(name=name))['body']
            body.update(version=version, revision='18446744073709551615')
            install(ctx, wire('descriptor', body))
    finally:
        lib.apkg_close(ctx)
    return names, version


class InstalledBrowser(unittest.TestCase):
    def test_run_worker_keeps_full_name_to_the_explicit_host_runtime_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            state(root, policy=policy())
            name = 'r' * 255
            binary = bytearray(64)
            binary[:7] = b'\x7fELF\x02\x01\x01'
            binary[18:20] = (62).to_bytes(2, 'little')
            app = dict(schema=1, id='demo', name='Demo', version='1.0', revision=1,
                targets=[dict(os='aros', arch='x86_64', abi='v11')],
                launch=dict(mode='shell', executable='program', working_directory='.'))
            files = []
            for path, data, mode in [('program', bytes(binary), '493'),
                    ('application.json', json.dumps(app).encode(), '420')]:
                files.append(dict(path=path, type='file', data=data.hex(), mode=mode,
                    size=str(len(data)), sha256=hashlib.sha256(data).hexdigest()))
            files.sort(key=lambda row: row['path'])
            code, ctx = open_ctx(root, 1)
            self.assertEqual(code, 0)
            try:
                install(ctx, descriptor(name=name, files=files))
            finally:
                lib.apkg_close(ctx)
            before = (root / '.apkg/state').read_bytes()
            result = subprocess.run([str(WORKER), str(root), name, '12'],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_full_values_and_record_evidence_survive_context_close(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            names, version = installed(root)
            before = (root / '.apkg/state').read_bytes()
            code, rows = query(root)
            self.assertEqual(code, 0)
            self.assertEqual({r[0] for r in rows}, set(names))
            records = json.loads(before)['body']['installed']
            for name, actual_version, revision, label, record in rows:
                self.assertEqual(actual_version, version)
                self.assertEqual(revision, '18446744073709551615')
                self.assertEqual(label, version + ' (revision 18446744073709551615)')
                expected = next(r for r in records if r['descriptor']['body']['name'] == name)
                self.assertEqual(json.loads(record), expected)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertFalse((root / 'db').exists())

    def test_search_preserves_distinct_names_and_does_not_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            names, _ = installed(root)
            before = (root / '.apkg/state').read_bytes()
            for term, expected in [(names[1], [names[1]]), (AUTH, names),
                    (TARGET, names), ('no match', [])]:
                code, rows = query(root, term)
                self.assertEqual(code, 0)
                self.assertEqual({r[0] for r in rows}, set(expected))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_model_allocation_failure_empty_and_nonready_roots(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            absent = d / 'absent'
            self.assertEqual(query(absent), (17, []))
            self.assertFalse(absent.exists())
            root = d / 'root'
            state(root)
            before = (root / '.apkg/state').read_bytes()
            self.assertEqual(query(root, fail=1), (4, []))
            self.assertEqual(query(root), (0, []))
            pending = Path(str(root) + '.apkg-transaction')
            pending.mkdir()
            self.assertEqual(query(root), (9, []))
            self.assertTrue(pending.is_dir())
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            legacy = d / 'legacy'
            (legacy / 'db').mkdir(parents=True)
            (legacy / 'db/installed.json').write_text('{"original":true}')
            self.assertEqual(query(legacy), (17, []))
            self.assertEqual((legacy / 'db/installed.json').read_text(), '{"original":true}')
            self.assertFalse((legacy / '.apkg').exists())

    def test_selected_full_name_drives_actual_repair_and_remove_worker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            names, _ = installed(root)
            code, rows = query(root, names[1])
            self.assertEqual(code, 0)
            selected = rows[0][0]
            (root / selected / 'hello').write_bytes(b'damaged')
            for action in (2, 1):
                result = subprocess.run([str(WORKER), str(root), selected, '1', str(action)],
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((root / names[0] / 'hello').read_bytes(), b'hello\n')
                if action == 2:
                    self.assertEqual((root / selected / 'hello').read_bytes(), b'hello\n')
            self.assertFalse((root / selected).exists())
            code, rows = query(root)
            self.assertEqual(code, 0)
            self.assertEqual([r[0] for r in rows], [names[0]])
