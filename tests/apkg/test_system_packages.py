# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""System packages: files placed one by one in the root itself, among files
the package does not own, with no second copy and roll-forward recovery."""
import ctypes as C
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from support import *
from test_operations import op, run_plan, payload
import test_client as clients
from test_channel import CHANNEL, CP
from test_file_channel import channel_key, publish
from test_author import key_fixture, build

CONFLICT, UNSUPPORTED, STALE, RECOVERY, TRUST, CANCELLED, POLICY = 14, 2, 8, 9, 12, 16, 17
COMMITTED, RESTORED, NEEDS_RECOVERY = 1, 2, 3
READY, PENDING = 2, 3


class ErrorView(C.Structure):
    _fields_ = [('code', C.c_int), ('message', Text), ('field', Text), ('subject', Text)]


class Update(C.Structure):
    _fields_ = [('version', C.c_uint32), ('size', C.c_size_t), ('root', C.c_char_p),
                ('endpoint', C.c_char_p), ('identity', C.c_char_p), ('name', C.c_char_p),
                ('policy', Text), ('target', C.c_char_p), ('takeover', C.c_int),
                ('recommendations', C.c_int), ('object_max', C.c_uint64),
                ('memory_max', C.c_uint64), ('confirm', C.c_void_p), ('user', C.c_void_p),
                ('observer', C.c_void_p)]


lib.apkg_error_get.argtypes = [C.c_void_p]
lib.apkg_error_get.restype = C.POINTER(ErrorView)
lib.apkg_error_free.argtypes = [C.c_void_p]
lib.apkg_plan_system_takeover.argtypes = [C.c_void_p, C.c_int]
lib.apkg_plan_upgrade.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_plan_install.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_plan_rollback_last.argtypes = [C.c_void_p, C.c_void_p, Text, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_plan_rollback_last.restype = C.c_int
lib.apkg_test_fault_after.argtypes = [C.c_int, C.c_int, C.c_int]
lib.pkg_fs_test_replace.argtypes = [C.c_int, C.c_int]
# Signatures other test modules also declare are kept identical to theirs.
lib.apkg_client_verify.argtypes = [C.c_char_p, C.c_char_p, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_verify.restype = C.c_int
lib.apkg_plan_format.argtypes = [C.c_void_p, C.POINTER(C.c_void_p)]
lib.apkg_plan_format.restype = C.c_int
lib.apkg_client_text_free.argtypes = [C.c_void_p]
lib.apkg_plan_adopt.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, Text, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_plan_adopt.restype = C.c_int
lib.apkg_client_channel_update.argtypes = [C.POINTER(Update), C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_channel_update.restype = C.c_int
lib.apkg_cancel_create.argtypes = [C.POINTER(C.c_void_p)]
lib.apkg_cancel_free.argtypes = [C.c_void_p]


def grant(system=True, channel=False):
    author = dict(id=AUTH, keys=[dict(algorithm='ed25519', public_key=PK.hex())])
    if system:
        author['permissions'] = ['system']
    body = dict(authorities=[author])
    if channel:
        body['channels'] = [dict(id=CHANNEL, keys=[dict(algorithm='ed25519', public_key=CP.hex())])]
    return wire('policy', body)


def system_root(directory, seed=None, populate=None):
    """A root that already holds a user's files, as a system volume does."""
    root = Path(directory) / 'root'
    root.mkdir()
    for path, text in (populate or {}).items():
        (root / path).parent.mkdir(parents=True, exist_ok=True)
        (root / path).write_text(text)
    code, c = open_ctx(root, 1)
    assert code == 0
    code, s = snapshot(c)
    code, p = initialize_plan(c, s, seed=seed or grant())
    assert code == 0, code
    code, a = approve(c, p)
    assert execute(c, p, a) == (0, COMMITTED)
    lib.apkg_approval_free(a)
    lib.apkg_plan_free(p)
    lib.apkg_snapshot_free(s)
    return root, c


def folder(path):
    return dict(path=path, type='directory')


def system(files, version='1.0', revision='1', name='aros-system', kind='system'):
    return wire('descriptor', dict(authority=AUTH, files=sorted(files, key=lambda f: f['path']),
                                   kind=kind, name=name, revision=revision, target=TARGET,
                                   version=version))


def plan(c, data, action='install', takeover=False):
    """(status, plan, error subject)."""
    lib.apkg_plan_system_takeover(c, int(takeover))
    code, s = snapshot(c)
    assert code == 0
    code, pkg = package(c, data, sign(data))
    if code:
        lib.apkg_snapshot_free(s)
        return code, None, ''
    p, e = C.c_void_p(), C.c_void_p()
    if action == 'install':
        code = lib.apkg_plan_install(c, s, pkg, None, C.byref(p), C.byref(e))
    else:
        code = lib.apkg_plan_upgrade(c, s, pkg, C.byref(p), C.byref(e))
    subject = raw(lib.apkg_error_get(e).contents.subject).decode() if e else ''
    lib.apkg_error_free(e)
    lib.apkg_package_free(pkg)
    lib.apkg_snapshot_free(s)
    lib.apkg_plan_system_takeover(c, 0)
    return code, p, subject


def records(p):
    view = lib.apkg_plan_get(p).contents
    return json.loads(C.string_at(view.canonical.data, view.canonical.size))['body']['system']


def apply(test, c, data, action='install', takeover=False):
    code, p, subject = plan(c, data, action, takeover)
    test.assertEqual(code, 0, subject)
    record = records(p)[0]
    test.assertEqual(run_plan(c, p), (0, COMMITTED))
    lib.apkg_plan_free(p)
    return record


def tree(root):
    """Every path of the root outside the engine's own database."""
    out = {}
    for path in sorted(root.rglob('*')):
        rel = str(path.relative_to(root))
        if rel == '.apkg' or rel.startswith('.apkg/'):
            continue
        out[rel] = path.read_text() if path.is_file() else None
    return out


def root_state(c):
    code, s = snapshot(c)
    assert code == 0
    value = lib.apkg_snapshot_root(s).contents.state
    lib.apkg_snapshot_free(s)
    return value


def leftovers(root):
    return [str(p) for p in root.rglob('*') if p.name.startswith(('.apkg-placing', '.pkgold'))]


def owned_text(pointer):
    text = C.string_at(pointer).decode() if pointer else ''
    lib.apkg_client_text_free(pointer)
    return text


def verify(root, name='aros-system'):
    text = C.c_void_p()
    code = lib.apkg_client_verify(os.fsencode(root), name.encode(), None, C.byref(text), None)
    return code, owned_text(text)


BASE = [folder('C'), payload('C/Copy', 'copy-1'), payload('C/List', 'list-1'),
        folder('Libs'), payload('Libs/old.library', 'old-1'),
        folder('Libs/Extras'), payload('Libs/Extras/gone.library', 'gone-1'),
        folder('S'), payload('S/Startup', 'startup-1', True), payload('S/Prefs', 'prefs-1', True)]
NEXT = [folder('C'), payload('C/Copy', 'copy-2'), payload('C/List', 'list-1'), payload('C/New', 'new-2'),
        folder('Libs'),
        folder('S'), payload('S/Startup', 'startup-2', True), payload('S/Prefs', 'prefs-2', True)]


class SystemPackages(unittest.TestCase):
    def tearDown(self):
        lib.apkg_test_fault(0, 0)
        lib.pkg_fs_test_replace(0, 0)

    def test_install_places_files_among_the_users_files(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, populate={'C/Mine': 'user', 'Notes': 'keep'})
            mode = dict(payload('C/Copy', 'copy-1'), mode='493')
            record = apply(self, c, system([folder('C'), mode, folder('S'), payload('S/Startup', 'startup-1', True)]))
            self.assertEqual(record['operation'], 'install')
            self.assertEqual(record['write'], ['C/Copy', 'S/Startup'])
            self.assertEqual(record['directories'], ['S'])
            self.assertEqual(record['bytes'], str(len('copy-1') + len('startup-1')))
            self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-1', 'C/Mine': 'user', 'Notes': 'keep',
                                          'S': None, 'S/Startup': 'startup-1'})
            self.assertEqual((root / 'C/Copy').stat().st_mode & 0o777, 0o755)
            self.assertFalse((root / 'aros-system').exists())
            entry = json.loads((root / '.apkg/state').read_bytes())['body']['installed'][0]
            self.assertEqual(entry['placement'], {'directories': ['S']})
            self.assertEqual(entry['descriptor']['body']['kind'], 'system')
            self.assertEqual(leftovers(root), [])
            # Nothing of the transaction holds a copy of the tree.
            history = [p for p in Path(d).iterdir() if '.apkg-history.' in p.name and b'"system":[' in (p / 'plan').read_bytes()]
            self.assertEqual(len(history), 1)
            self.assertEqual(sorted(p.name for p in history[0].iterdir()),
                             ['archive-id', 'decision', 'new-state', 'old-state', 'plan', 'plan-sha', 'ready'])
            self.assertEqual(root_state(c), READY)
            lib.apkg_close(c)

    def test_identical_bytes_are_owned_without_a_write(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, populate={'C/Copy': 'copy-1'})
            before = (root / 'C/Copy').stat()
            record = apply(self, c, system([folder('C'), payload('C/Copy', 'copy-1'), payload('C/List', 'list-1')]))
            self.assertEqual(record['write'], ['C/List'])
            self.assertEqual(record['taken'], [])
            after = (root / 'C/Copy').stat()
            self.assertEqual((before.st_ino, before.st_mtime_ns), (after.st_ino, after.st_mtime_ns))
            code, report = verify(root)
            self.assertEqual(code, 0)
            self.assertIn('ok C/Copy\n', report)
            lib.apkg_close(c)

    def test_other_bytes_conflict_unless_takeover_is_a_plan_input(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, populate={'C/Copy': 'someone else'})
            data = system([folder('C'), payload('C/Copy', 'copy-1'), payload('C/List', 'list-1')])
            before = tree(root)
            code, p, subject = plan(c, data)
            self.assertEqual((code, subject), (CONFLICT, 'C/Copy'))
            self.assertEqual(tree(root), before)
            code, p, subject = plan(c, data, takeover=True)
            self.assertEqual(code, 0, subject)
            record = records(p)[0]
            self.assertEqual((record['takeover'], record['taken'], record['write']), ('1', ['C/Copy'], ['C/List']))
            text = C.c_void_p()
            self.assertEqual(lib.apkg_plan_format(p, C.byref(text)), 0)
            text = owned_text(text)
            self.assertIn('Takeover of existing unowned files: requested', text)
            self.assertIn('Take over existing file C/Copy', text)
            self.assertEqual(run_plan(c, p), (0, COMMITTED))
            lib.apkg_plan_free(p)
            self.assertEqual((root / 'C/Copy').read_text(), 'copy-1')
            lib.apkg_close(c)

    def test_takeover_is_part_of_the_approved_digest(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            data = system([folder('C'), payload('C/Copy', 'copy-1')])
            code, without, _ = plan(c, data)
            code, with_takeover, _ = plan(c, data, takeover=True)
            self.assertEqual(records(without)[0]['takeover'], '0')
            self.assertEqual(records(with_takeover)[0]['takeover'], '1')
            self.assertNotEqual(bytes(lib.apkg_plan_get(without).contents.plan_digest.bytes),
                                bytes(lib.apkg_plan_get(with_takeover).contents.plan_digest.bytes))
            lib.apkg_plan_free(without)
            lib.apkg_plan_free(with_takeover)
            lib.apkg_close(c)

    def test_takeover_never_takes_a_path_of_another_package(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system([folder('C'), payload('C/Copy', 'copy-1')]))
            for files in ([folder('C'), payload('C/Copy', 'other')],      # same file
                          [payload('C', 'a file where a shared directory is')]):
                code, p, subject = plan(c, system(files, name='second-system'), takeover=True)
                self.assertEqual(code, CONFLICT, files)
            # A shared directory with another file of its own is fine.
            apply(self, c, system([folder('C'), payload('C/Other', 'other')], name='second-system'))
            self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-1', 'C/Other': 'other'})
            lib.apkg_close(c)

    def test_a_file_handed_from_one_package_to_another_survives(self):
        lib.apkg_plan_resolve.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p), C.c_size_t,
                                          C.POINTER(C.c_void_p), C.c_size_t, C.c_int, C.POINTER(C.c_void_p), C.c_void_p]
        lib.apkg_plan_resolve.restype = C.c_int
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system([folder('C'), payload('C/Shared', 'same bytes'), folder('Made'), payload('Made/a', 'a')]))
            apply(self, c, system([folder('C'), payload('C/Other', 'other')], name='second-system'))
            # One plan: the first drops the file and its directory, the second lists both.
            first = system([folder('C')] + [payload('C/First', 'first')], version='2.0')
            second = system([folder('C'), payload('C/Other', 'other'), payload('C/Shared', 'same bytes'), folder('Made')],
                            version='2.0', name='second-system')
            code, s = snapshot(c)
            packages = (C.c_void_p * 2)()
            for index, data in enumerate((first, second)):
                code, pkg = package(c, data, sign(data))
                self.assertEqual(code, 0)
                packages[index] = pkg
            p = C.c_void_p()
            self.assertEqual(lib.apkg_plan_resolve(c, s, packages, 2, None, 0, 0, C.byref(p), None), 0)
            by_name = {r['name']: r for r in records(p)}
            self.assertEqual((by_name['aros-system']['delete'], by_name['aros-system']['rmdir']), (['Made/a'], []))
            self.assertEqual(by_name['second-system']['write'], [])
            self.assertEqual(run_plan(c, p), (0, COMMITTED))
            self.assertEqual(tree(root), {'C': None, 'C/First': 'first', 'C/Other': 'other', 'C/Shared': 'same bytes', 'Made': None})
            self.assertIn('ok C/Shared\n', verify(root, 'second-system')[1])
            lib.apkg_plan_free(p)
            lib.apkg_snapshot_free(s)
            lib.apkg_close(c)

    def test_upgrade_three_way_per_file(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, populate={'S/User-Startup': 'mine'})
            apply(self, c, system(BASE))
            (root / 'C/List').write_text('patched by hand')          # modified, unchanged upstream
            (root / 'C/Copy').write_text('patched by hand')          # modified ordinary file
            (root / 'S/Startup').write_text('my startup')            # edited configuration
            (root / 'Libs/old.library').write_text('patched')        # dropped, modified
            (root / 'C/Neighbour').write_text('not listed')
            record = apply(self, c, system(NEXT, version='2.0'), 'upgrade')
            self.assertEqual(record['operation'], 'upgrade')
            self.assertEqual(record['write'], ['C/New', 'S/Prefs'])           # absent, and pristine
            self.assertEqual(record['overwrite'], ['C/Copy', 'C/List'])       # a system file is the signed one
            self.assertEqual(record['keep'], ['S/Startup'])
            self.assertEqual(record['delete'], ['Libs/Extras/gone.library'])
            self.assertEqual(record['release'], ['Libs/old.library'])
            self.assertEqual(record['rmdir'], ['Libs/Extras'])
            self.assertEqual(tree(root), {
                'C': None, 'C/Copy': 'copy-2', 'C/List': 'list-1', 'C/New': 'new-2', 'C/Neighbour': 'not listed',
                'Libs': None, 'Libs/old.library': 'patched',
                'S': None, 'S/Prefs': 'prefs-2', 'S/Startup': 'my startup', 'S/User-Startup': 'mine'})
            entry = json.loads((root / '.apkg/state').read_bytes())['body']['installed'][0]
            self.assertEqual(entry['placement'], {'directories': ['C', 'Libs']})
            self.assertEqual(entry['descriptor']['body']['version'], '2.0')
            lib.apkg_close(c)

    def test_directories_go_only_when_created_and_empty(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, populate={'Old/keep': 'x'})
            (root / 'Old/keep').unlink()                              # Old exists, empty, not ours
            apply(self, c, system([folder('Old'), payload('Old/a', 'a'), folder('Made'), payload('Made/a', 'a'),
                                   folder('Used'), payload('Used/a', 'a'), folder('Stay'), payload('Stay/a', 'a')]))
            (root / 'Used/mine').write_text('user file')
            record = apply(self, c, system([folder('Stay'), payload('Stay/a', 'a')], version='2.0'), 'upgrade')
            self.assertEqual(record['rmdir'], ['Made', 'Used'])
            self.assertEqual(tree(root), {'Old': None, 'Stay': None, 'Stay/a': 'a', 'Used': None, 'Used/mine': 'user file'})
            lib.apkg_close(c)

    def test_new_file_over_unowned_bytes_conflicts_on_upgrade(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system([folder('C'), payload('C/Copy', 'copy-1')]))
            (root / 'C/New').write_text('the user had one')
            newer = system([folder('C'), payload('C/Copy', 'copy-1'), payload('C/New', 'new-2')], version='2.0')
            self.assertEqual(plan(c, newer, 'upgrade')[::2], (CONFLICT, 'C/New'))
            record = apply(self, c, newer, 'upgrade', takeover=True)
            self.assertEqual(record['taken'], ['C/New'])
            self.assertEqual((root / 'C/New').read_text(), 'new-2')
            lib.apkg_close(c)

    def test_type_conflicts_and_links_refuse(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, populate={'C/Copy/inside': 'a directory here', 'Libs': 'a file here'})
            self.assertEqual(plan(c, system([folder('C'), payload('C/Copy', 'x')]), takeover=True)[0], CONFLICT)
            self.assertEqual(plan(c, system([folder('Libs'), payload('Libs/a', 'x')]), takeover=True)[0], CONFLICT)
            (root / 'S').symlink_to(root / 'C')
            self.assertEqual(plan(c, system([folder('S'), payload('S/a', 'x')]), takeover=True)[0], CONFLICT)
            lib.apkg_close(c)

    def test_no_grant_no_system_package(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, seed=grant(system=False))
            code, p, subject = plan(c, system([folder('C'), payload('C/Copy', 'copy-1')]))
            self.assertEqual((code, subject), (TRUST, 'aros-system'))
            self.assertEqual(tree(root), {})
            # The same authority still installs bundles.
            data = descriptor()
            code, s = snapshot(c)
            code, pkg = package(c, data, sign(data))
            code, p = install_plan(c, s, pkg)
            self.assertEqual(code, 0)
            self.assertEqual(run_plan(c, p), (0, COMMITTED))
            lib.apkg_close(c)

    def test_permission_schema_is_strict(self):
        with tempfile.TemporaryDirectory() as d:
            key = dict(algorithm='ed25519', public_key=PK.hex())
            for index, bad in enumerate((
                    dict(authorities=[dict(id=AUTH, keys=[key], permissions=[])]),
                    dict(authorities=[dict(id=AUTH, keys=[key], permissions=['root'])]),
                    dict(authorities=[dict(id=AUTH, keys=[key], permissions=['system', 'system'])]),
                    dict(authorities=[dict(id=AUTH, keys=[key], permissions='system')]),
                    dict(authorities=[], channels=[dict(id=AUTH, keys=[key], permissions=['system'])]))):
                root = Path(d) / f'root{index}'
                code, c = open_ctx(root, 1)
                code, s = snapshot(c)
                self.assertEqual(initialize_plan(c, s, seed=wire('policy', bad))[0], POLICY, bad)
                lib.apkg_snapshot_free(s)
                lib.apkg_close(c)

    def test_reserved_paths_are_refused_by_the_signed_reader(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            for files in ([folder('.apkg'), payload('.apkg/state', 'x')],
                          [folder('C'), payload('C/.apkg-placing', 'x')],
                          [folder('C'), payload('C/.pkgold12345678', 'x')],
                          [payload('.APKG-transaction', 'x')],
                          [folder('db'), payload('db/x', 'x')],
                          [payload('.pkg', 'x')]):
                data = system(files)
                code, pkg = package(c, data, sign(data))
                self.assertNotEqual(code, 0, files)
            # The same names stay legal inside a bundle's own folder.
            data = system([folder('C'), payload('C/.apkg-placing', 'x')], kind='data', name='demo')
            code, pkg = package(c, data, sign(data))
            self.assertEqual(code, 0)
            lib.apkg_package_free(pkg)
            lib.apkg_close(c)

    def test_remove_rollback_and_adoption_are_refused(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system([folder('C'), payload('C/Copy', 'copy-1')]))
            before = tree(root)
            self.assertEqual(op(c, 'remove', 'aros-system')[0], UNSUPPORTED)
            code, s = snapshot(c)
            for name in (b'aros-system', b''):
                out, e = C.c_void_p(), C.c_void_p()
                buffer = C.create_string_buffer(name)
                code = lib.apkg_plan_rollback_last(c, s, Text(C.cast(buffer, C.c_void_p), len(name)), C.byref(out), C.byref(e))
                self.assertEqual(code, UNSUPPORTED)
                self.assertIn(b'cannot be rolled back', raw(lib.apkg_error_get(e).contents.message))
                lib.apkg_error_free(e)
            history = next(p for p in Path(d).iterdir() if '.apkg-history.' in p.name and (p / 'decision').exists()
                           and b'"system":[' in (p / 'plan').read_bytes())
            self.assertEqual(op(c, 'rollback', str(history))[0], UNSUPPORTED)
            # The bundle adoption path cannot claim a system package either.
            data = system([folder('C'), payload('C/Copy', 'copy-1')], name='second-system')
            code, pkg = package(c, data, sign(data))
            evidence = wire('adoption', dict(baseline=[], reason='explicit', record='', source='manual'))
            buffer = C.create_string_buffer(evidence)
            out = C.c_void_p()
            self.assertEqual(lib.apkg_plan_adopt(c, s, pkg, Text(C.cast(buffer, C.c_void_p), len(evidence)), C.byref(out), None), UNSUPPORTED)
            lib.apkg_package_free(pkg)
            lib.apkg_snapshot_free(s)
            self.assertEqual(tree(root), before)
            lib.apkg_close(c)

    def test_verify_and_repair(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system(BASE))
            (root / 'C/Copy').write_text('damaged')
            (root / 'C/List').unlink()
            (root / 'S/Startup').write_text('my startup')
            (root / 'S/Prefs').unlink()
            (root / 'C/Neighbour').write_text('not listed')
            code, report = verify(root)
            self.assertEqual(code, 0)
            self.assertIn('\nok 6\nchanged 1\nedited 1\nmissing 2\n', report)
            for line in ('changed C/Copy', 'missing C/List', 'edited S/Startup [configuration]',
                         'missing S/Prefs [configuration]', 'ok Libs/old.library', 'ok C'):
                self.assertIn(line + '\n', report)
            self.assertNotIn('Neighbour', report)
            code, p = op(c, 'repair', 'aros-system')
            self.assertEqual(code, 0)
            record = records(p)[0]
            self.assertEqual((record['operation'], record['overwrite'], record['write'], record['keep']),
                             ('repair', ['C/Copy'], ['C/List', 'S/Prefs'], ['S/Startup']))
            self.assertEqual(run_plan(c, p), (0, COMMITTED))
            lib.apkg_plan_free(p)
            self.assertEqual((root / 'C/Copy').read_text(), 'copy-1')
            self.assertEqual((root / 'C/List').read_text(), 'list-1')
            self.assertEqual((root / 'S/Startup').read_text(), 'my startup')
            self.assertEqual((root / 'S/Prefs').read_text(), 'prefs-1')
            self.assertEqual((root / 'C/Neighbour').read_text(), 'not listed')
            self.assertIn('\nok 9\nchanged 0\nedited 1\nmissing 0\n', verify(root)[1])
            lib.apkg_close(c)

    def test_bundles_and_system_packages_share_a_root(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            install_bundle(self, c, descriptor(name='demo'))
            # No system path inside a bundle's folder, whatever the case.
            for top in ('demo', 'Demo'):
                code, p, subject = plan(c, system([folder(top), payload(top + '/x', 'x')]), takeover=True)
                self.assertEqual((code, subject), (CONFLICT, 'demo'))
            apply(self, c, system([folder('Tools'), payload('Tools/Clock', 'clock-1'), folder('C'), payload('C/Copy', 'copy-1')]))
            # No bundle folder on a name the system package's files occupy.
            data = descriptor(name='tools')
            code, s = snapshot(c)
            code, pkg = package(c, data, sign(data))
            self.assertEqual(install_plan(c, s, pkg)[0], CONFLICT)
            lib.apkg_package_free(pkg)
            lib.apkg_snapshot_free(s)
            # Bundle operations go on unchanged beside it.
            install_bundle(self, c, descriptor(name='other'))
            newer = json.loads(descriptor(name='demo', files=[payload('hello', 'second')]))
            newer['body']['version'] = '2.0'
            data = json.dumps(newer, sort_keys=True, separators=(',', ':')).encode()
            code, p, subject = plan(c, data, 'upgrade')
            self.assertEqual(code, 0)
            self.assertEqual(run_plan(c, p), (0, COMMITTED))
            code, p = op(c, 'remove', 'other')
            self.assertEqual(run_plan(c, p), (0, COMMITTED))
            apply(self, c, system([folder('Tools'), payload('Tools/Clock', 'clock-2'), folder('C'), payload('C/Copy', 'copy-1')],
                                  version='2.0'), 'upgrade')
            self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-1', 'Tools': None, 'Tools/Clock': 'clock-2',
                                          'demo': None, 'demo/hello': 'second'})
            code, s = snapshot(c)
            self.assertEqual(lib.apkg_snapshot_installed_count(s), 2)
            lib.apkg_snapshot_free(s)
            # A bundle transaction can still be rolled back; the system one cannot.
            lib.apkg_close(c)

    def test_a_live_edit_after_the_preview_is_stale(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, populate={'C/Copy': 'copy-1'})
            data = system([folder('C'), payload('C/Copy', 'copy-1'), payload('C/List', 'list-1')])
            for edit in (lambda: (root / 'C/Copy').write_text('edited after the preview'),
                         lambda: (root / 'C/List').write_text('appeared after the preview')):
                code, p, _ = plan(c, data)
                self.assertEqual(code, 0)
                before = (root / '.apkg/state').read_bytes()
                edit()
                self.assertEqual(run_plan(c, p), (STALE, 0))
                self.assertEqual((root / '.apkg/state').read_bytes(), before)
                lib.apkg_plan_free(p)
                (root / 'C/Copy').write_text('copy-1')
                (root / 'C/List').unlink(missing_ok=True)
            lib.apkg_close(c)

    def test_events_and_cancellation(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            data = system([folder('C'), payload('C/Copy', 'copy-1'), payload('C/List', 'list-11')])
            # Any cancellable boundary stops it with nothing written.
            for boundary in (1, 2, 3):
                seen = []

                def cancel(user, event, boundary=boundary, seen=seen):
                    e = event.contents
                    seen.append((e.phase, e.cancellable))
                    return 1 if e.cancellable and len([x for x in seen if x[1]]) == boundary else 0
                callback = EVENT_FN(cancel)
                observer = Observer(callback, None, None)
                code, p, _ = plan(c, data)
                code, a = approve(c, p)
                self.assertEqual(execute(c, p, a, C.byref(observer))[0], CANCELLED)
                self.assertEqual(tree(root), {})
                self.assertEqual(root_state(c), READY)
                lib.apkg_approval_free(a)
                lib.apkg_plan_free(p)
            self.assertEqual(seen[-1], (3, 1))                      # APPLY, the last cancellable boundary
            # After the first write it runs to the end, and says so.
            events = []

            def late(user, event):
                e = event.contents
                events.append((e.phase, e.cancellable, raw(e.unit), e.completed, e.total.state, e.total.value, raw(e.subject)))
                return 0 if e.cancellable else 1
            callback = EVENT_FN(late)
            observer = Observer(callback, None, None)
            code, p, _ = plan(c, data)
            total = int(records(p)[0]['bytes'])
            code, a = approve(c, p)
            r = C.c_void_p()
            self.assertEqual(lib.apkg_execute(c, p, a, C.byref(observer), C.byref(r), None), 0)
            view = lib.apkg_result_get(r).contents
            self.assertEqual((view.outcome, view.cancellation_requested), (COMMITTED, 1))
            lib.apkg_result_free(r)
            placing = [e for e in events if e[0] == 3 and not e[1]]
            self.assertTrue(placing)
            byte_events = [e for e in placing if e[2] == b'bytes']
            file_events = [e for e in placing if e[2] == b'files']
            self.assertEqual(byte_events[0][3:6], (0, 1, total))
            self.assertEqual(byte_events[-1][3:6], (total, 1, total))
            self.assertEqual([e[3] for e in file_events], [1, 2])
            self.assertEqual(file_events[-1][4:6], (1, 2))
            self.assertEqual(file_events[-1][6], b'C/List')
            self.assertEqual(sorted(e[3] for e in byte_events), [e[3] for e in byte_events])
            self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-1', 'C/List': 'list-11'})
            lib.apkg_close(c)


def install_bundle(test, c, data):
    code, s = snapshot(c)
    code, pkg = package(c, data, sign(data))
    test.assertEqual(code, 0)
    code, p = install_plan(c, s, pkg)
    test.assertEqual(code, 0)
    test.assertEqual(run_plan(c, p), (0, COMMITTED))
    lib.apkg_plan_free(p)
    lib.apkg_package_free(pkg)
    lib.apkg_snapshot_free(s)


EXPECTED = {'C': None, 'C/Copy': 'copy-2', 'C/List': 'list-1', 'C/New': 'new-2', 'C/Neighbour': 'not listed',
            'Libs': None, 'Libs/old.library': 'patched',
            'S': None, 'S/Prefs': 'prefs-2', 'S/Startup': 'my startup'}


class SystemRecovery(unittest.TestCase):
    """An interruption at any step leaves a pending root that recovery finishes
    forward to exactly the approved result."""

    def tearDown(self):
        lib.apkg_test_fault(0, 0)
        lib.pkg_fs_test_replace(0, 0)

    def prepared(self, d):
        root, c = system_root(d)
        apply(self, c, system(BASE))
        (root / 'C/Copy').write_text('patched by hand')
        (root / 'S/Startup').write_text('my startup')
        (root / 'Libs/old.library').write_text('patched')
        (root / 'C/Neighbour').write_text('not listed')
        code, p, subject = plan(c, system(NEXT, version='2.0'), 'upgrade')
        self.assertEqual(code, 0, subject)
        code, a = approve(c, p)
        return root, c, p, a

    def interrupted(self, root, c, p, a, arm):
        pid = os.fork()
        if pid == 0:
            arm()
            execute(c, p, a)
            os._exit(98)
        _, status = os.waitpid(pid, 0)
        self.assertEqual(os.waitstatus_to_exitcode(status), 99)

    def settled(self, root, c, outcome=COMMITTED):
        self.assertEqual(recover(c), (0, outcome))
        self.assertEqual(root_state(c), READY)
        self.assertEqual(recover(c), (0, 0))
        self.assertEqual(leftovers(root), [])

    def test_process_death_before_the_decision_changes_nothing(self):
        for point in (1201, 1, 1203, 1204, 1205, 2):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                root, c, p, a = self.prepared(d)
                before = tree(root)
                state = (root / '.apkg/state').read_bytes()
                self.interrupted(root, c, p, a, lambda: lib.apkg_test_fault(point, 2))
                if point in (1205, 2):
                    self.assertEqual(root_state(c), PENDING)
                    self.assertEqual(recover(c), (0, RESTORED))
                self.assertEqual(root_state(c), READY)
                self.assertEqual(tree(root), before)
                self.assertEqual((root / '.apkg/state').read_bytes(), state)
                lib.apkg_close(c)

    def test_process_death_at_every_step_rolls_forward(self):
        # (point, passes let through first): decision, directories, temporary
        # file written, file replaced, placements done, a deletion, all system
        # steps done, state published, recovery start and end.
        steps = [(4, 0), (201, 0), (202, 0), (202, 2), (203, 0), (203, 1), (203, 2), (204, 0), (205, 0),
                 (206, 0), (5, 0), (7, 0)]
        for point, skip in steps:
            with self.subTest(point=point, skip=skip), tempfile.TemporaryDirectory() as d:
                root, c, p, a = self.prepared(d)
                self.interrupted(root, c, p, a, lambda: lib.apkg_test_fault_after(point, 2, skip))
                self.assertEqual(root_state(c), PENDING)        # never a mix that reads as ready
                code, s = snapshot(c)
                self.assertEqual(install_plan_refused(c, s), RECOVERY)
                lib.apkg_snapshot_free(s)
                self.settled(root, c)
                self.assertEqual(tree(root), EXPECTED)
                state = json.loads((root / '.apkg/state').read_bytes())['body']
                self.assertEqual(state['installed'][0]['descriptor']['body']['version'], '2.0')
                self.assertEqual(verify(root)[1].count('\nchanged '), 1)
                self.assertIn('\nchanged 0\nedited 1\nmissing 0\n', verify(root)[1])
                lib.apkg_close(c)

    def test_recovery_interrupted_again_still_finishes(self):
        for first, second in (((202, 1), (203, 1)), ((203, 0), (205, 0)), ((201, 0), (6, 0)), ((204, 0), (206, 0))):
            with self.subTest(first=first, second=second), tempfile.TemporaryDirectory() as d:
                root, c, p, a = self.prepared(d)
                self.interrupted(root, c, p, a, lambda: lib.apkg_test_fault_after(first[0], 2, first[1]))
                pid = os.fork()
                if pid == 0:
                    lib.apkg_test_fault_after(second[0], 2, second[1])
                    recover(c)
                    os._exit(98)
                _, status = os.waitpid(pid, 0)
                self.assertEqual(os.waitstatus_to_exitcode(status), 99)
                self.assertEqual(root_state(c), PENDING)
                self.settled(root, c)
                self.assertEqual(tree(root), EXPECTED)
                lib.apkg_close(c)

    def test_write_failure_after_the_decision_needs_recovery(self):
        for point, skip in ((201, 0), (202, 1), (203, 1), (204, 0), (205, 0), (206, 0), (5, 0)):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                root, c, p, a = self.prepared(d)
                lib.apkg_test_fault_after(point, 1, skip)
                self.assertEqual(execute(c, p, a), (RECOVERY, NEEDS_RECOVERY))
                lib.apkg_test_fault(0, 0)
                self.assertEqual(root_state(c), PENDING)
                self.settled(root, c)
                self.assertEqual(tree(root), EXPECTED)
                lib.apkg_close(c)

    def test_replacement_through_the_aside_recovers_at_each_step(self):
        # A DOS without ReplaceObject replaces in three steps; the process
        # stops after the old file moved aside, and after the new one arrived.
        for step in (1, 2):
            with self.subTest(step=step), tempfile.TemporaryDirectory() as d:
                root, c, p, a = self.prepared(d)
                self.interrupted(root, c, p, a, lambda: lib.pkg_fs_test_replace(1, step))
                self.assertEqual(root_state(c), PENDING)
                if step == 1:
                    self.assertFalse((root / 'C/Copy').exists())     # its old bytes wait aside
                    self.assertEqual(sorted(Path(p).name[:7] for p in leftovers(root)), ['.apkg-p', '.pkgold'])
                else:
                    self.assertEqual((root / 'C/Copy').read_text(), 'copy-2')
                    self.assertEqual([Path(p).name[:7] for p in leftovers(root)], ['.pkgold'])
                lib.pkg_fs_test_replace(1, 0)
                self.settled(root, c)
                self.assertEqual(tree(root), EXPECTED)
                lib.apkg_close(c)

    def test_recovery_reports_its_placement(self):
        with tempfile.TemporaryDirectory() as d:
            root, c, p, a = self.prepared(d)
            self.interrupted(root, c, p, a, lambda: lib.apkg_test_fault_after(203, 2, 0))
            events = []
            callback = EVENT_FN(lambda user, event: events.append(
                (event.contents.phase, event.contents.cancellable, raw(event.contents.unit),
                 event.contents.completed, event.contents.total.value)) or 0)
            observer = Observer(callback, None, None)
            result = C.c_void_p()
            self.assertEqual(lib.apkg_recover(c, C.byref(observer), C.byref(result), None), 0)
            lib.apkg_result_free(result)
            files = [e for e in events if e[2] == b'files']
            self.assertEqual([e[:2] for e in files], [(5, 0)] * 3)      # RECOVER, never cancellable
            self.assertEqual([e[3:] for e in files], [(1, 3), (2, 3), (3, 3)])
            self.assertEqual(tree(root), EXPECTED)
            lib.apkg_close(c)

    def test_a_missing_retained_object_keeps_the_root_pending(self):
        with tempfile.TemporaryDirectory() as d:
            root, c, p, a = self.prepared(d)
            self.interrupted(root, c, p, a, lambda: lib.apkg_test_fault(201, 2))
            store = Path(str(root) + '.apkg-objects')
            wanted = store / hashlib.sha256(b'new-2').hexdigest()
            held = wanted.read_bytes()
            wanted.unlink()
            self.assertEqual(recover(c), (RECOVERY, NEEDS_RECOVERY))
            self.assertEqual(root_state(c), PENDING)
            wanted.write_bytes(held)
            self.settled(root, c)
            self.assertEqual(tree(root), EXPECTED)
            lib.apkg_close(c)

    def test_install_recovers_too(self):
        for point, skip in ((201, 0), (202, 1), (203, 0), (204, 0), (5, 0)):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                root, c = system_root(d, populate={'C/Mine': 'user', 'C/Copy': 'in the way'})
                code, p, subject = plan(c, system(BASE), takeover=True)
                self.assertEqual(code, 0, subject)
                code, a = approve(c, p)
                self.interrupted(root, c, p, a, lambda: lib.apkg_test_fault_after(point, 2, skip))
                self.settled(root, c)
                self.assertEqual(tree(root)['C/Copy'], 'copy-1')
                self.assertEqual(tree(root)['C/Mine'], 'user')
                self.assertIn('\nok 10\nchanged 0\nedited 0\nmissing 0\n', verify(root)[1])
                lib.apkg_close(c)


def install_plan_refused(c, s):
    data = descriptor(name='late')
    code, pkg = package(c, data, sign(data))
    if code:
        return code
    code, p = install_plan(c, s, pkg)
    lib.apkg_package_free(pkg)
    lib.apkg_plan_free(p)
    return code


class SystemScale(unittest.TestCase):
    def test_four_thousand_files_within_default_budgets(self):
        count, per = 4000, 50
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, populate={'D000/user-file': 'mine'})

            def listing(generation, drop=0):
                files = [folder(f'D{i:03d}') for i in range(count // per)]
                for i in range(drop, count):
                    text = f'file {i} generation {generation if i % 10 == 0 else 1}\n'
                    files.append(payload(f'D{i // per:03d}/file-{i:05d}.library', text, config=i % 400 == 1))
                return files
            started = time.monotonic()
            data = system(listing(1))
            self.assertGreater(len(data), 700000)
            record = apply(self, c, data)
            self.assertEqual(len(record['write']), count)
            installed = time.monotonic() - started
            code, report = verify(root)
            self.assertEqual(code, 0)
            self.assertIn(f'\nok {count + count // per}\nchanged 0\nedited 0\nmissing 0\n', report)
            (root / 'D003/file-00151.library').write_text('damaged')
            started = time.monotonic()
            record = apply(self, c, system(listing(2, drop=per), version='2.0'), 'upgrade')
            upgraded = time.monotonic() - started
            self.assertEqual(len(record['delete']), per)
            self.assertEqual(len(record['write']), (count - per) // 10)
            self.assertEqual(record['overwrite'], ['D003/file-00151.library'])
            self.assertEqual(record['rmdir'], [])                   # D000 holds the user's file
            self.assertEqual((root / 'D000/user-file').read_text(), 'mine')
            self.assertEqual(sum(1 for _ in root.rglob('*.library')), count - per)
            self.assertIn('\nchanged 0\nedited 0\nmissing 0\n', verify(root)[1])
            # Plans and state stay inside the default 2 MiB record reserve.
            self.assertLess((root / '.apkg/state').stat().st_size, 2 * 1024 * 1024)
            # Linear work: generous bounds that a quadratic walk would miss.
            self.assertLess(installed, 60, installed)
            self.assertLess(upgraded, 60, upgraded)
            lib.apkg_close(c)


def toml(name='aros-system', version='1.0', configs=()):
    return (f'id = "{name}"\nversion = "{version}"\nrevision = 1\nsummary = "Base system"\n'
            f'category = "System"\nlicense = "APL"\nkind = "system"\narch = "x86_64"\nabi = "v11"\n'
            f'depends_checked = true\nconfig = {json.dumps(list(configs))}\n')


def drawer(base, files, version='1.0', configs=('S/Startup',), name='Sys'):
    source = base / name
    source.mkdir()
    for path, text in files.items():
        (source / path).parent.mkdir(parents=True, exist_ok=True)
        (source / path).write_text(text)
    (base / (name + '.arospkg.toml')).write_text(toml(version=version, configs=configs))
    return source


def confirmed(test, *args):
    preview = clients.cli(*args)
    test.assertEqual(preview.returncode, CANCELLED, preview.stdout + preview.stderr)
    digest = re.search(r'Approved plan: ([a-f0-9]{64})', preview.stdout).group(1)
    return clients.cli(*args, '--confirm', digest), preview.stdout


class SystemClients(unittest.TestCase):
    """The builder, the CLI and the unattended client call."""

    def initialized(self, d, pinned):
        root = d / 'root'
        root.mkdir()
        seed = d / 'policy.json'
        seed.write_bytes(pinned)
        done = clients.cli('init', root, TARGET, seed, '--yes')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return root, seed

    def test_builder_maps_the_drawer_to_the_root(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            source = drawer(d, {'C/Copy': 'copy', 'S/Startup': 'startup', 'Devs/Keymaps/pc105': 'map'})
            (source / 'Empty').mkdir()
            key = key_fixture(d / 'key')
            out = d / 'system.pkg'
            done = build(source, out, key)
            self.assertEqual(done.returncode, 0, done.stderr)
            from test_author import members
            body = json.loads(members(out)['descriptor.json'])['body']
            self.assertEqual(body['kind'], 'system')
            self.assertEqual([f['path'] for f in body['files']],
                             ['C', 'C/Copy', 'Devs', 'Devs/Keymaps', 'Devs/Keymaps/pc105', 'Empty', 'S', 'S/Startup'])
            self.assertEqual([f['path'] for f in body['files'] if f.get('config')], ['S/Startup'])
            self.assertFalse(any('Package.arospkg.toml' in f['path'] for f in body['files']))

    def test_builder_refuses_what_a_system_package_cannot_carry(self):
        cases = {
            'subdir': lambda d, s: (d / 'Sys.arospkg.toml').write_text(toml() + 'subdir = "Sys"\n'),
            'reserved': lambda d, s: ((s / '.apkg').mkdir(), (s / '.apkg/state').write_text('x')),
            'placing': lambda d, s: (s / 'C/.apkg-placing').write_text('x'),
            'legacy database': lambda d, s: ((s / 'db').mkdir(), (s / 'db/x').write_text('x')),
            'metadata': lambda d, s: (s / 'C/.ameta').write_text('ameta 1\nfile Copy\nprot 0x40\n'),
            'missing config': lambda d, s: (s / 'S/Startup').unlink(),
        }
        for label, spoil in cases.items():
            with self.subTest(label), tempfile.TemporaryDirectory() as d:
                d = Path(d)
                source = drawer(d, {'C/Copy': 'copy', 'S/Startup': 'startup'})
                spoil(d, source)
                out = d / 'system.pkg'
                done = build(source, out, key_fixture(d / 'key'))
                self.assertNotEqual(done.returncode, 0, label)
                self.assertFalse(out.exists())

    def test_cli_install_upgrade_repair_verify_with_takeover(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            root, seed = self.initialized(d, grant())
            (root / 'C').mkdir()
            (root / 'C/Copy').write_text('already here, different')
            (root / 'C/Mine').write_text('user')
            key = key_fixture(d / 'key')
            first = d / 'one.pkg'
            self.assertEqual(build(drawer(d, {'C/Copy': 'copy-1', 'S/Startup': 'startup-1', 'Libs/a.library': 'a-1'}),
                                   first, key).returncode, 0)
            refused = clients.cli('install', root, first)
            self.assertEqual(refused.returncode, CONFLICT, refused.stdout + refused.stderr)
            self.assertIn('C/Copy', refused.stderr)
            done, text = confirmed(self, 'install', root, first, '--takeover')
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn('Take over existing file C/Copy', text)
            self.assertEqual((root / 'C/Copy').read_text(), 'copy-1')
            self.assertEqual((root / 'C/Mine').read_text(), 'user')
            (root / 'S/Startup').write_text('mine')
            (root / 'Libs/a.library').write_text('damaged')
            report = clients.cli('verify', root, 'aros-system')
            self.assertEqual(report.returncode, 0, report.stderr)
            self.assertIn('changed Libs/a.library', report.stdout)
            self.assertIn('edited S/Startup [configuration]', report.stdout)
            done, text = confirmed(self, 'repair', root, 'aros-system')
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual((root / 'Libs/a.library').read_text(), 'a-1')
            self.assertEqual((root / 'S/Startup').read_text(), 'mine')
            second = d / 'two.pkg'
            newer = d / 'v2'
            newer.mkdir()
            self.assertEqual(build(drawer(newer, {'C/Copy': 'copy-2', 'S/Startup': 'startup-2', 'C/New': 'new-2'}, version='2.0'),
                                   second, key).returncode, 0)
            done, text = confirmed(self, 'upgrade', root, second)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn('Keep edited configuration S/Startup', text)
            self.assertIn('Delete Libs/a.library', text)
            self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-2', 'C/Mine': 'user', 'C/New': 'new-2',
                                          'S': None, 'S/Startup': 'mine'})
            for command in (('remove', root, 'aros-system'), ('rollback-last', root, 'aros-system'),
                            ('rollback-last', root, '-')):
                refused = clients.cli(*command)
                self.assertEqual(refused.returncode, UNSUPPORTED, refused.stdout + refused.stderr)
            self.assertEqual(clients.cli('folder', root, 'aros-system').stdout.splitlines()[0], str(root.resolve()))
            self.assertEqual(clients.cli('info', root, 'aros-system', 'kind').stdout.splitlines()[0], 'system')

    def published(self, d, files, version, name):
        base = d / name
        base.mkdir()
        archive = d / (name + '.pkg')
        self.assertEqual(build(drawer(base, files, version=version), archive, d / 'key').returncode, 0)
        done = publish(archive, d / 'channel', d / 'channel-key')
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_unattended_channel_update(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            key_fixture(d / 'key')
            channel_key(d / 'channel-key')
            pinned = grant(channel=True)
            seed = d / 'policy.json'
            seed.write_bytes(pinned)
            self.published(d, {'C/Copy': 'copy-1', 'S/Startup': 'startup-1', 'Libs/a.library': 'a-1'}, '1.0', 'v1')
            # A volume that already holds a system and a user's files.
            root = d / 'root'
            (root / 'C').mkdir(parents=True)
            (root / 'C/Copy').write_text('an older build')
            (root / 'C/Mine').write_text('user')
            update = ('channel-update', root, d / 'channel', CHANNEL, 'aros-system', '--policy', seed)
            refused = clients.cli(*update, '--yes')
            self.assertNotEqual(refused.returncode, 0)                 # not initialized, no target given
            refused = clients.cli(*update, '--target', TARGET, '--yes')
            self.assertEqual(refused.returncode, CONFLICT, refused.stdout + refused.stderr)
            done = clients.cli(*update, '--target', TARGET, '--takeover', '--yes')
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn('outcome=1', done.stdout)
            self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-1', 'C/Mine': 'user', 'Libs': None,
                                          'Libs/a.library': 'a-1', 'S': None, 'S/Startup': 'startup-1'})
            again = clients.cli(*update, '--yes')
            self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
            self.assertIn('outcome=0', again.stdout)                   # current: nothing to do
            (root / 'S/Startup').write_text('mine')
            self.published(d, {'C/Copy': 'copy-2', 'S/Startup': 'startup-2', 'C/New': 'new-2'}, '2.0', 'v2')
            # The pinned policy is what the root must hold, byte for byte.
            other = d / 'other.json'
            other.write_bytes(grant(system=False, channel=True))
            refused = clients.cli('channel-update', root, d / 'channel', CHANNEL, 'aros-system', '--policy', other, '--yes')
            self.assertEqual(refused.returncode, POLICY, refused.stdout + refused.stderr)
            # The library call the updater makes, with its own observer.
            phases = []
            callback = EVENT_FN(lambda user, event: phases.append(event.contents.phase) or 0)
            observer = Observer(callback, None, None)
            buffer = C.create_string_buffer(pinned)
            options = Update(1, C.sizeof(Update), os.fsencode(root), os.fsencode(d / 'channel'), CHANNEL.encode(),
                             b'aros-system', Text(C.cast(buffer, C.c_void_p), len(pinned)), None, 0, 0, 0, 0, None,
                             None, C.cast(C.byref(observer), C.c_void_p))
            result = C.c_void_p()
            self.assertEqual(lib.apkg_client_channel_update(C.byref(options), C.byref(result), None), 0)
            self.assertEqual(lib.apkg_result_get(result).contents.outcome, COMMITTED)
            lib.apkg_result_free(result)
            self.assertIn(3, phases)
            self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-2', 'C/Mine': 'user', 'C/New': 'new-2',
                                          'S': None, 'S/Startup': 'mine'})
            # A struct of another size is another contract.
            options.size += 8
            self.assertEqual(lib.apkg_client_channel_update(C.byref(options), C.byref(result), None), UNSUPPORTED)

    def test_unattended_update_over_http(self):
        from test_http_channel import serve
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            key_fixture(d / 'key')
            channel_key(d / 'channel-key')
            seed = d / 'policy.json'
            seed.write_bytes(grant(channel=True))
            self.published(d, {'C/Copy': 'copy-1', 'S/Startup': 'startup-1'}, '1.0', 'v1')
            root = d / 'root'
            root.mkdir()
            with serve(d / 'channel') as (url, server):
                update = ('channel-update', root, url, CHANNEL, 'aros-system', '--policy', seed, '--target', TARGET, '--yes')
                done = clients.cli(*update)
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                self.assertTrue(any(path.startswith('/archives/') for path in server.requests))
                self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-1', 'S': None, 'S/Startup': 'startup-1'})
                self.published(d, {'C/Copy': 'copy-2', 'S/Startup': 'startup-1'}, '2.0', 'v2')
                done = clients.cli(*update)
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                self.assertEqual((root / 'C/Copy').read_text(), 'copy-2')
                again = clients.cli(*update)
                self.assertIn('outcome=0', again.stdout)

    def test_unattended_update_recovers_an_interrupted_transaction_first(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            key_fixture(d / 'key')
            channel_key(d / 'channel-key')
            pinned = grant(channel=True)
            seed = d / 'policy.json'
            seed.write_bytes(pinned)
            self.published(d, {'C/Copy': 'copy-1', 'C/List': 'list-1', 'S/Startup': 'startup-1'}, '1.0', 'v1')
            root = d / 'root'
            root.mkdir()
            update = ('channel-update', root, d / 'channel', CHANNEL, 'aros-system', '--policy', seed, '--target', TARGET, '--yes')
            buffer = C.create_string_buffer(pinned)
            options = Update(1, C.sizeof(Update), os.fsencode(root), os.fsencode(d / 'channel'), CHANNEL.encode(),
                             b'aros-system', Text(C.cast(buffer, C.c_void_p), len(pinned)), TARGET.encode(), 0, 0, 0, 0,
                             None, None, None)
            pid = os.fork()
            if pid == 0:
                lib.apkg_test_fault_after(203, 2, 0)                   # dies with one file of three placed
                result = C.c_void_p()
                lib.apkg_client_channel_update(C.byref(options), C.byref(result), None)
                os._exit(98)
            _, status = os.waitpid(pid, 0)
            self.assertEqual(os.waitstatus_to_exitcode(status), 99)
            self.assertEqual(clients.cli('inspect', root).stdout.split()[0], f'root_state={PENDING}')
            done = clients.cli(*update)                                # the next boot
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-1', 'C/List': 'list-1', 'S': None, 'S/Startup': 'startup-1'})
            self.assertEqual(clients.cli('inspect', root).stdout.split()[0], f'root_state={READY}')

    def test_a_channel_retains_several_builds_of_a_large_system_package(self):
        # A snapshot embeds each retained build's whole descriptor: three
        # builds of 4,000 files pass the former 2 MiB and 100,000-token bounds.
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            key_fixture(d / 'key')
            channel_key(d / 'channel-key')
            seed = d / 'policy.json'
            seed.write_bytes(grant(channel=True))
            for build_number in (1, 2, 3):
                files = {f'D{i // 50:02d}/file-{i:04d}.library': f'{i} of build {build_number if i < 5 else 1}' for i in range(4000)}
                files['S/Startup'] = 'startup'
                self.published(d, files, f'{build_number}.0', f'v{build_number}')
            document = (d / 'channel/snapshots' / (d / 'channel/current').read_text().strip() / 'channel.json').read_bytes()
            self.assertGreater(len(document), 2 * 1024 * 1024)
            self.assertEqual(len(json.loads(document)['body']['entries']), 3)
            root = d / 'root'
            root.mkdir()
            done = clients.cli('channel-update', root, d / 'channel', CHANNEL, 'aros-system', '--policy', seed, '--target', TARGET, '--yes')
            self.assertEqual(done.returncode, 0, done.stdout[-600:] + done.stderr)
            self.assertEqual((root / 'D00/file-0003.library').read_text(), '3 of build 3')     # the newest build
            self.assertEqual(sum(1 for _ in root.rglob('*.library')), 4000)
            report = clients.cli('verify', root, 'aros-system')
            self.assertIn('\nchanged 0\nedited 0\nmissing 0\n', report.stdout)


if __name__ == '__main__':
    unittest.main()
