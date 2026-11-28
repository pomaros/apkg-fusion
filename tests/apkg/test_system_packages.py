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
                ('recommendations', C.c_int), ('repair', C.c_int), ('object_max', C.c_uint64),
                ('memory_max', C.c_uint64), ('store', C.c_char_p), ('confirm', C.c_void_p),
                ('user', C.c_void_p), ('observer', C.c_void_p)]


def update_options(root, endpoint, pinned, target=None, observer=None, store=None, repair=0, takeover=0):
    buffer = C.create_string_buffer(pinned)
    options = Update(2, C.sizeof(Update), os.fsencode(root), os.fsencode(endpoint), CHANNEL.encode(),
                     b'aros-system', Text(C.cast(buffer, C.c_void_p), len(pinned)),
                     target.encode() if target else None, takeover, 0, repair, 0, 0,
                     os.fsencode(store) if store else None, None, None, observer)
    options.keep = buffer
    return options


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
lib.pkg_fs_test_free.argtypes = [C.c_ulonglong]
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


def blob(path, text, config=False):
    """A file whose bytes live in the store only, as an archive delivers them."""
    data = text.encode()
    sha = hashlib.sha256(data).hexdigest()
    entry = dict(blob=sha, mode='420', path=path, sha256=sha, size=str(len(data)), type='file')
    if config:
        entry['config'] = 'preserve'
    return entry


def stored(root, *texts):
    store = Path(str(root) + '.apkg-objects')
    store.mkdir(exist_ok=True)
    for text in texts:
        (store / hashlib.sha256(text.encode()).hexdigest()).write_bytes(text.encode())
    return store


def execute_error(c, p):
    """(status, outcome, message, subject) of one execution."""
    code, a = approve(c, p)
    assert code == 0
    r, e = C.c_void_p(), C.c_void_p()
    code = lib.apkg_execute(c, p, a, None, C.byref(r), C.byref(e))
    outcome = lib.apkg_result_get(r).contents.outcome
    view = lib.apkg_error_get(e).contents if e else None
    message = raw(view.message).decode() if view else ''
    subject = raw(view.subject).decode() if view else ''
    lib.apkg_result_free(r)
    lib.apkg_error_free(e)
    lib.apkg_approval_free(a)
    return code, outcome, message, subject


def present(root, relative):
    """The file's text under either spelling of its case."""
    path = root / relative
    return path.read_text() if path.exists() else None


class SystemReview(unittest.TestCase):
    """Defects found by review, one regression each."""

    def tearDown(self):
        lib.apkg_test_fault(0, 0)
        lib.pkg_fs_test_replace(0, 0)
        lib.pkg_fs_test_free(0)

    def test_a_case_only_rename_keeps_the_file_and_the_directory(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system([folder('C'), payload('C/copy', 'same'), folder('Made'), payload('Made/tool', 'tool-1')]))
            record = apply(self, c, system([folder('C'), payload('C/Copy', 'same'), folder('MADE'), payload('MADE/Tool', 'tool-2')],
                                           version='2.0'), 'upgrade')
            self.assertEqual((record['delete'], record['rmdir'], record['release'], record['clear']), ([], [], [], []))
            self.assertEqual(present(root, 'C/Copy'), 'same')
            self.assertEqual(present(root, 'MADE/Tool'), 'tool-2')
            self.assertEqual(root_state(c), READY)
            self.assertIn('\nchanged 0\nedited 0\nmissing 0\n', verify(root)[1])
            # The directory stays recorded as created under its new spelling.
            entry = json.loads((root / '.apkg/state').read_bytes())['body']['installed'][0]
            self.assertIn('MADE', entry['placement']['directories'])
            lib.apkg_close(c)

    def test_paths_that_fold_alike_are_one_path(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            # One descriptor cannot list both spellings.
            data = system([folder('C'), payload('C/Copy', 'a'), payload('C/copy', 'b')])
            self.assertNotEqual(package(c, data, sign(data))[0], 0)
            apply(self, c, system([folder('C'), payload('C/Copy', 'A-owned')], name='pkg-a'))
            # Another package cannot take the file through a case difference.
            for files in ([folder('C'), payload('C/copy', 'B-bytes')], [folder('c'), payload('c/COPY', 'B-bytes')],
                          [payload('c', 'a file')]):
                code, p, subject = plan(c, system(files, name='pkg-b'), takeover=True)
                self.assertEqual(code, CONFLICT, files)
            self.assertEqual((root / 'C/Copy').read_text(), 'A-owned')
            lib.apkg_close(c)

    def test_a_missing_or_corrupt_object_refuses_before_the_decision(self):
        for damage in ('missing', 'corrupt'):
            with self.subTest(damage), tempfile.TemporaryDirectory() as d:
                root, c = system_root(d)
                apply(self, c, system([folder('C'), payload('C/A', 'a1'), payload('C/Z', 'z1')]))
                store = stored(root, 'a2', 'z2')
                code, p, subject = plan(c, system([folder('C'), blob('C/A', 'a2'), blob('C/Z', 'z2')], version='2.0'), 'upgrade')
                self.assertEqual(code, 0, subject)
                before = tree(root)
                target = store / hashlib.sha256(b'z2').hexdigest()
                if damage == 'missing':
                    target.unlink()
                else:
                    target.write_bytes(b'ZZ')
                code, outcome, message, subject = execute_error(c, p)
                self.assertIn(code, (5, 10))                       # NOT_FOUND, INTEGRITY
                self.assertNotEqual(outcome, NEEDS_RECOVERY)
                self.assertEqual(root_state(c), READY)
                self.assertEqual(tree(root), before)                # C/A was not written either
                lib.apkg_plan_free(p)
                lib.apkg_close(c)

    def test_an_object_corrupted_after_planning_is_caught_by_the_last_check(self):
        # Present at both live checks, wrong bytes of the right length: only
        # the hash before the decision can tell.
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            store = stored(root, 'a2', 'z2')
            code, p, subject = plan(c, system([folder('C'), blob('C/A', 'a2'), blob('C/Z', 'z2')]))
            self.assertEqual(code, 0, subject)
            (store / hashlib.sha256(b'z2').hexdigest()).write_bytes(b'ZZ')
            code, outcome, message, subject = execute_error(c, p)
            self.assertEqual((code, subject), (10, 'C/Z'))
            self.assertIn('nothing was written', message)
            self.assertEqual(root_state(c), READY)
            self.assertEqual(tree(root), {})
            self.assertEqual([x for x in Path(d).iterdir() if 'transaction' in x.name], [])
            lib.apkg_close(c)

    def test_too_little_free_space_refuses_before_the_decision(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system([folder('C'), payload('C/Big', 'x' * 5000), payload('C/Small', 's1')]))
            newer = system([folder('C'), payload('C/Big', 'y' * 5000), payload('C/Small', 's2'), payload('C/New', 'n' * 3000)], version='2.0')
            code, p, subject = plan(c, newer, 'upgrade')
            before = tree(root)
            lib.pkg_fs_test_free(270000)                           # under the margin plus the peak
            code, outcome, message, subject = execute_error(c, p)
            self.assertEqual(code, 3)                              # LIMIT
            self.assertIn('free space', message)
            self.assertRegex(subject, r'^\d+ bytes needed, 270000 free$')
            needed = int(subject.split()[0])
            self.assertEqual(root_state(c), READY)
            self.assertEqual(tree(root), before)
            lib.apkg_plan_free(p)
            # The room asked for is the peak of in-place replacement, not the
            # net growth: the 5,000-byte file needs its new copy beside the old.
            for free, expected in ((needed - 1, 3), (needed, 0)):
                code, p, subject = plan(c, newer, 'upgrade')
                if expected:
                    view = lib.apkg_plan_get(p).contents
                    body = json.loads(C.string_at(view.canonical.data, view.canonical.size))['body']
                    state_size = len(json.dumps(body['new_state'], sort_keys=True, separators=(',', ':')))
                    unit = os.statvfs(root).f_frsize
                    # Peak 5,000 (C/Big beside its old copy; C/New then fits in
                    # what that freed), three files, the state twice, the margin.
                    self.assertEqual(needed, 5000 + (3 + 4) * unit + 2 * state_size + 262144)
                lib.pkg_fs_test_free(free)
                self.assertEqual(execute_error(c, p)[0], expected, free)
                lib.apkg_plan_free(p)
            self.assertEqual(present(root, 'C/New'), 'n' * 3000)
            lib.apkg_close(c)

    def test_a_directory_without_owner_write_is_filled_then_given_its_mode(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            closed = dict(path='Locked', type='directory', mode='365')      # 0555
            try:
                apply(self, c, system([closed, payload('Locked/A', 'a1'), dict(path='Locked/Sub', type='directory', mode='365'),
                                       payload('Locked/Sub/B', 'b1')]))
                self.assertEqual((root / 'Locked').stat().st_mode & 0o777, 0o555)
                self.assertEqual((root / 'Locked/Sub').stat().st_mode & 0o777, 0o555)
                self.assertEqual((root / 'Locked/Sub/B').read_text(), 'b1')
                # A later version writes into it and deletes from it.
                record = apply(self, c, system([closed, payload('Locked/A', 'a2'), payload('Locked/C', 'c2')], version='2.0'), 'upgrade')
                self.assertEqual(record['delete'], ['Locked/Sub/B'])
                self.assertEqual(tree(root), {'Locked': None, 'Locked/A': 'a2', 'Locked/C': 'c2'})
                self.assertEqual((root / 'Locked').stat().st_mode & 0o777, 0o555)
                # Recovery copes with a directory an earlier attempt created.
                code, p, subject = plan(c, system([closed, payload('Locked/A', 'a3'), payload('Locked/C', 'c2'), dict(path='Two', type='directory', mode='365'),
                                                   payload('Two/X', 'x3'), payload('Two/Y', 'y3')], version='3.0'), 'upgrade')
                lib.apkg_test_fault_after(203, 1, 1)
                self.assertEqual(run_plan(c, p), (RECOVERY, NEEDS_RECOVERY))
                lib.apkg_test_fault(0, 0)
                self.assertEqual(recover(c), (0, COMMITTED))
                self.assertEqual(tree(root), {'Locked': None, 'Locked/A': 'a3', 'Locked/C': 'c2', 'Two': None, 'Two/X': 'x3', 'Two/Y': 'y3'})
                self.assertEqual((root / 'Two').stat().st_mode & 0o777, 0o555)
                self.assertEqual(recover(c), (0, 0))
            finally:
                os.system('chmod -R u+w %s' % d)
            lib.apkg_close(c)

    def test_an_edit_made_while_pending_is_kept_by_recovery(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system([folder('S'), payload('S/A', 'a1'), payload('S/Lib', 'l1'), payload('S/Startup', 's1', True)]))
            code, p, subject = plan(c, system([folder('S'), payload('S/A', 'a2'), payload('S/Lib', 'l2'), payload('S/New', 'n2'),
                                               payload('S/Startup', 's2', True)], version='2.0'), 'upgrade')
            self.assertEqual(records(p)[0]['write'], ['S/A', 'S/Lib', 'S/New', 'S/Startup'])
            lib.apkg_test_fault_after(203, 1, 0)                    # stops with S/A placed
            self.assertEqual(run_plan(c, p), (RECOVERY, NEEDS_RECOVERY))
            lib.apkg_test_fault(0, 0)
            (root / 'S/Startup').write_text('edited while pending')
            (root / 'S/Lib').write_text('patched while pending')
            (root / 'S/New').write_text('created while pending')
            code, outcome = recover(c)
            self.assertEqual((code, outcome), (0, COMMITTED))
            self.assertEqual(tree(root), {'S': None, 'S/A': 'a2', 'S/Lib': 'patched while pending', 'S/New': 'created while pending',
                                          'S/Startup': 'edited while pending'})
            history = [h for h in Path(d).iterdir() if '.apkg-history.' in h.name and (h / 'kept').exists()]
            self.assertEqual(len(history), 1)
            self.assertEqual((history[0] / 'kept').read_text().split(), ['S/Lib', 'S/New', 'S/Startup'])
            report = verify(root)[1]
            self.assertIn('edited S/Startup [configuration]', report)
            self.assertIn('changed S/Lib', report)                 # visible, and repair restores it
            lib.apkg_close(c)

    def test_files_the_plan_said_to_replace_are_replaced_in_recovery(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, populate={'C/Mine': 'in the way'})
            apply(self, c, system([folder('C'), payload('C/A', 'a1'), payload('C/B', 'b1')]))
            (root / 'C/B').write_text('modified before the plan')
            code, p, subject = plan(c, system([folder('C'), payload('C/A', 'a2'), payload('C/B', 'b2'), payload('C/Mine', 'm2')],
                                              version='2.0'), 'upgrade', takeover=True)
            self.assertEqual((records(p)[0]['overwrite'], records(p)[0]['taken']), (['C/B'], ['C/Mine']))
            lib.apkg_test_fault(201, 1)
            self.assertEqual(run_plan(c, p), (RECOVERY, NEEDS_RECOVERY))
            lib.apkg_test_fault(0, 0)
            (root / 'C/B').write_text('modified again')
            self.assertEqual(recover(c), (0, COMMITTED))
            self.assertEqual(tree(root), {'C': None, 'C/A': 'a2', 'C/B': 'b2', 'C/Mine': 'm2'})
            lib.apkg_close(c)

    def test_takeover_adopts_an_existing_configuration_as_it_is(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d, populate={'S/Startup': 'my own startup', 'S/Shell': 'an older shell'})
            data = system([folder('S'), payload('S/Startup', 'default startup', True), payload('S/Shell', 'shell-1')])
            self.assertEqual(plan(c, data)[0], CONFLICT)           # never without the request
            record = apply(self, c, data, takeover=True)
            self.assertEqual((record['keep'], record['taken'], record['write']), (['S/Startup'], ['S/Shell'], []))
            self.assertEqual(tree(root), {'S': None, 'S/Shell': 'shell-1', 'S/Startup': 'my own startup'})
            self.assertIn('edited S/Startup [configuration]', verify(root)[1])
            # It is owned now: the next version keeps it too, without takeover.
            record = apply(self, c, system([folder('S'), payload('S/Startup', 'newer default', True), payload('S/Shell', 'shell-2')],
                                           version='2.0'), 'upgrade')
            self.assertEqual(record['keep'], ['S/Startup'])
            self.assertEqual((root / 'S/Startup').read_text(), 'my own startup')
            # The same holds for a preserved file a new version adds.
            (root / 'S/Prefs').write_text('my prefs')
            record = apply(self, c, system([folder('S'), payload('S/Startup', 'newer default', True), payload('S/Shell', 'shell-2'),
                                            payload('S/Prefs', 'default prefs', True)], version='3.0'), 'upgrade', takeover=True)
            self.assertEqual(record['keep'], ['S/Prefs', 'S/Startup'])
            self.assertEqual((root / 'S/Prefs').read_text(), 'my prefs')
            lib.apkg_close(c)

    def test_a_file_becomes_a_directory_and_back(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system([folder('C'), payload('C/Tool', 't1'), payload('C/Other', 'o1')]))
            wide = system([folder('C'), payload('C/Other', 'o1'), folder('C/Tool'), payload('C/Tool/bin', 't2'),
                           folder('C/Tool/data'), payload('C/Tool/data/x', 'x2')], version='2.0')
            code, p, subject = plan(c, wide, 'upgrade')
            self.assertEqual(code, 0, subject)
            record = records(p)[0]
            self.assertEqual((record['clear'], record['directories'], record['write']),
                             (['C/Tool'], ['C/Tool', 'C/Tool/data'], ['C/Tool/bin', 'C/Tool/data/x']))
            self.assertEqual(run_plan(c, p), (0, COMMITTED))
            self.assertEqual(tree(root), {'C': None, 'C/Other': 'o1', 'C/Tool': None, 'C/Tool/bin': 't2', 'C/Tool/data': None,
                                          'C/Tool/data/x': 'x2'})
            narrow = system([folder('C'), payload('C/Other', 'o1'), payload('C/Tool', 't3')], version='3.0')
            code, p, subject = plan(c, narrow, 'upgrade')
            self.assertEqual(code, 0, subject)
            record = records(p)[0]
            self.assertEqual((record['clear'], record['write'], record['delete'], record['rmdir']),
                             (['C/Tool', 'C/Tool/bin', 'C/Tool/data', 'C/Tool/data/x'], ['C/Tool'], [], []))
            self.assertEqual(run_plan(c, p), (0, COMMITTED))
            self.assertEqual(tree(root), {'C': None, 'C/Other': 'o1', 'C/Tool': 't3'})
            self.assertIn('\nok 3\nchanged 0\nedited 0\nmissing 0\n', verify(root)[1])
            lib.apkg_close(c)

    def test_a_type_change_refuses_when_something_else_is_in_the_way(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system([folder('C'), payload('C/Tool', 't1'), folder('C/Dir'), payload('C/Dir/a', 'a1'), payload('C/Dir/b', 'b1')]))
            to_dir = system([folder('C'), folder('C/Tool'), payload('C/Tool/bin', 't2'), folder('C/Dir'), payload('C/Dir/a', 'a1'),
                             payload('C/Dir/b', 'b1')], version='2.0')
            to_file = system([folder('C'), payload('C/Tool', 't1'), payload('C/Dir', 'now a file')], version='2.0')
            (root / 'C/Tool').write_text('edited')
            self.assertEqual(plan(c, to_dir, 'upgrade', takeover=True)[::2], (CONFLICT, 'C/Tool'))
            (root / 'C/Tool').write_text('t1')
            (root / 'C/Dir/mine').write_text('unowned')
            self.assertEqual(plan(c, to_file, 'upgrade', takeover=True)[::2], (CONFLICT, 'C/Dir/mine'))
            (root / 'C/Dir/mine').unlink()
            (root / 'C/Dir/a').write_text('edited')
            self.assertEqual(plan(c, to_file, 'upgrade', takeover=True)[::2], (CONFLICT, 'C/Dir/a'))
            (root / 'C/Dir/a').unlink()                             # a missing child is no obstacle
            apply(self, c, to_file, 'upgrade')
            self.assertEqual(tree(root), {'C': None, 'C/Dir': 'now a file', 'C/Tool': 't1'})
            lib.apkg_close(c)

    def test_a_type_change_recovers_at_each_step(self):
        for point, skip in ((207, 0), (207, 2), (201, 0), (203, 0), (204, 0), (206, 0)):
            with self.subTest(point=point, skip=skip), tempfile.TemporaryDirectory() as d:
                root, c = system_root(d)
                apply(self, c, system([folder('C'), payload('C/Tool', 't1'), folder('C/Dir'), payload('C/Dir/a', 'a1'),
                                       folder('C/Dir/sub'), payload('C/Dir/sub/b', 'b1')]))
                code, p, subject = plan(c, system([folder('C'), folder('C/Tool'), payload('C/Tool/bin', 't2'), payload('C/Dir', 'file now')],
                                                  version='2.0'), 'upgrade')
                self.assertEqual(code, 0, subject)
                code, a = approve(c, p)
                pid = os.fork()
                if pid == 0:
                    lib.apkg_test_fault_after(point, 2, skip)
                    execute(c, p, a)
                    os._exit(98)
                self.assertEqual(os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]), 99)
                self.assertEqual(root_state(c), PENDING)
                self.assertEqual(recover(c), (0, COMMITTED))
                self.assertEqual(tree(root), {'C': None, 'C/Dir': 'file now', 'C/Tool': None, 'C/Tool/bin': 't2'})
                lib.apkg_close(c)

    def test_the_token_array_is_charged_to_working_memory(self):
        from test_file_channel import open_file
        from test_channel import catalog, entry as channel_entry
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            files = [folder('D')] + [payload(f'D/f{i:05d}', str(i)) for i in range(6000)]
            data = system(files)
            document = wire('channel', json.loads(catalog([channel_entry(data, sign(data))]))['body'])
            self.assertGreater(len(document), 800000)
            signature = sign(document, CP, __import__('test_channel').CS, 'channel')
            lib.apkg_channel_read.argtypes = [C.c_void_p, C.POINTER(ID), C.c_uint64, Text, Text, C.POINTER(C.c_void_p), C.c_void_p]
            lib.apkg_channel_read.restype = C.c_int
            state(d / 'root', policy=grant(channel=True))
            identity = ID()
            identity.bytes[:] = bytes.fromhex(CHANNEL)
            results = {}
            for label, memory in (('default', 0), ('small', 2 * 1024 * 1024)):
                code, c = open_ctx(d / 'root', 0, 2 * 1024 * 1024 if memory else 0, memory)
                self.assertEqual(code, 0)
                a, b = C.create_string_buffer(document), C.create_string_buffer(signature)
                out = C.c_void_p()
                results[label] = lib.apkg_channel_read(c, C.byref(identity), 0, Text(C.cast(a, C.c_void_p), len(document)),
                                                       Text(C.cast(b, C.c_void_p), len(signature)), C.byref(out), None)
                if out:
                    lib.apkg_channel_free(out)
                lib.apkg_close(c)
            # 78,000 tokens need 2.5 MiB; half of a 2 MiB budget does not hold them.
            self.assertEqual(results, {'default': 0, 'small': 3})


class SystemRecoveryGaps(unittest.TestCase):
    """Interruptions the first recovery cases did not reach."""

    def tearDown(self):
        lib.apkg_test_fault(0, 0)
        lib.pkg_fs_test_replace(0, 0)

    BEFORE = [folder('C'), payload('C/Copy', 'copy-1'), payload('C/Keep', 'keep-1'),
              folder('Old'), payload('Old/a', 'a-1'), payload('Old/b', 'b-1'), payload('Old/c', 'c-1'),
              folder('Old/Deep'), payload('Old/Deep/d', 'd-1')]
    AFTER = [folder('C'), payload('C/Copy', 'copy-2' * 20000), payload('C/Keep', 'keep-1'), payload('C/New', 'new-2'),
             folder('Made'), payload('Made/x', 'x-2')]
    RESULT = {'C': None, 'C/Copy': 'copy-2' * 20000, 'C/Keep': 'keep-1', 'C/New': 'new-2', 'Made': None, 'Made/x': 'x-2'}
    # Every point of placement, with passes let through where a step repeats:
    # mid-write of the temporary file (second 64 KiB chunk), several
    # deletions, between the last unlink and the first rmdir.
    POINTS = [(4, 0), (201, 0), (208, 0), (208, 1), (202, 0), (202, 2), (203, 0), (203, 1), (204, 0), (205, 0), (205, 1),
              (205, 3), (209, 0), (206, 0), (5, 0)]

    def prepared(self, d):
        root, c = system_root(d)
        apply(self, c, system(self.BEFORE))
        code, p, subject = plan(c, system(self.AFTER, version='2.0'), 'upgrade')
        self.assertEqual(code, 0, subject)
        self.assertEqual(records(p)[0]['delete'], ['Old/Deep/d', 'Old/a', 'Old/b', 'Old/c'])
        self.assertEqual(records(p)[0]['rmdir'], ['Old', 'Old/Deep'])
        code, a = approve(c, p)
        return root, c, p, a

    def dies(self, action, arm):
        pid = os.fork()
        if pid == 0:
            arm()
            action()
            os._exit(98)
        self.assertEqual(os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]), 99)

    def test_every_point_then_recovery(self):
        for point, skip in self.POINTS:
            with self.subTest(point=point, skip=skip), tempfile.TemporaryDirectory() as d:
                root, c, p, a = self.prepared(d)
                self.dies(lambda: execute(c, p, a), lambda: lib.apkg_test_fault_after(point, 2, skip))
                self.assertEqual(root_state(c), PENDING)
                self.assertEqual(recover(c), (0, COMMITTED))
                self.assertEqual(tree(root), self.RESULT)
                self.assertEqual(leftovers(root), [])
                self.assertEqual(recover(c), (0, 0))
                lib.apkg_close(c)

    def test_recovery_itself_interrupted_at_every_point(self):
        # The first run stops right after the decision; recovery then dies at
        # each point in turn, and a second recovery finishes.
        for point, skip in self.POINTS[1:-1] + [(6, 0), (7, 0)]:
            with self.subTest(point=point, skip=skip), tempfile.TemporaryDirectory() as d:
                root, c, p, a = self.prepared(d)
                self.dies(lambda: execute(c, p, a), lambda: lib.apkg_test_fault(4, 2))
                self.dies(lambda: recover(c), lambda: lib.apkg_test_fault_after(point, 2, skip))
                self.assertEqual(root_state(c), PENDING)
                self.assertEqual(recover(c), (0, COMMITTED))
                self.assertEqual(tree(root), self.RESULT)
                self.assertEqual(leftovers(root), [])
                lib.apkg_close(c)

    def test_aside_steps_with_an_interrupted_recovery(self):
        # A DOS without ReplaceObject: the run dies inside one replacement,
        # recovery dies inside the next one, the last recovery finishes both.
        for first in (1, 2):
            for second in (1, 2):
                with self.subTest(first=first, second=second), tempfile.TemporaryDirectory() as d:
                    root, c, p, a = self.prepared(d)
                    (root / 'C/New').write_text('new-2')            # owned without a write
                    code, p, subject = plan(c, system(self.AFTER + [payload('C/Keep2', 'k2')], version='2.0'), 'upgrade')
                    code, a = approve(c, p)
                    self.dies(lambda: execute(c, p, a), lambda: lib.pkg_fs_test_replace(1, first))
                    self.dies(lambda: recover(c), lambda: lib.pkg_fs_test_replace(1, second))
                    self.assertEqual(root_state(c), PENDING)
                    lib.pkg_fs_test_replace(1, 0)
                    self.assertEqual(recover(c), (0, COMMITTED))
                    self.assertEqual(tree(root), dict(self.RESULT, **{'C/Keep2': 'k2'}))
                    self.assertEqual(leftovers(root), [])
                    lib.apkg_close(c)

    def test_recovery_finishes_a_replacement_stopped_between_its_renames(self):
        # The destination is absent and its old bytes wait under the aside
        # name: whatever replacement primitive recovery has, it ends with the
        # new file in place and no aside.
        with tempfile.TemporaryDirectory() as d:
            root, c, p, a = self.prepared(d)
            self.dies(lambda: execute(c, p, a), lambda: lib.pkg_fs_test_replace(1, 1))
            self.assertFalse((root / 'C/Copy').exists())
            self.assertEqual(len([x for x in leftovers(root) if '.pkgold' in x]), 1)
            lib.pkg_fs_test_replace(0, 0)                           # atomic rename from here on
            self.assertEqual(recover(c), (0, COMMITTED))
            self.assertEqual(tree(root), self.RESULT)
            self.assertEqual(leftovers(root), [])
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

    def test_builder_accepts_files_for_another_cpu(self):
        # A real system tree: GRUB's modules are i386 ELF objects and the
        # keymaps are Amiga hunk files, whatever the system's own CPU is.
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            source = drawer(d, {'C/Copy': 'copy', 'S/Startup': 'startup'})
            (source / 'boot').mkdir()
            (source / 'boot/normal.mod').write_bytes(b'\x7fELF\x01\x01\x01' + bytes(9) + b'\x01\x00\x03\x00' + bytes(44))
            (source / 'Devs').mkdir()
            (source / 'Devs/pc105_pl').write_bytes(bytes.fromhex('000003f3 00000000 00000001 00000000 00000000 000000f8 000003e9'))
            out = d / 'system.pkg'
            done = build(source, out, key_fixture(d / 'key'))
            self.assertEqual(done.returncode, 0, done.stderr)
            from test_author import members
            body = json.loads(members(out)['descriptor.json'])['body']
            self.assertIn('boot/normal.mod', [f['path'] for f in body['files']])
            self.assertIn('Devs/pc105_pl', [f['path'] for f in body['files']])

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
            options = update_options(root, d / 'channel', pinned, observer=C.cast(C.byref(observer), C.c_void_p))
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
            options = update_options(root, d / 'channel', pinned, target=TARGET)
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


def listed_objects(root):
    """Content digests the installed descriptors of a root reference."""
    state = json.loads((root / '.apkg/state').read_bytes())['body']
    return sorted({f['sha256'] for entry in state['installed'] for f in entry['descriptor']['body']['files'] if f['type'] == 'file'})


class SystemStore(unittest.TestCase):
    """A store directory chosen by the caller, pruning, and refetching."""

    V1 = {'C/Copy': 'copy-1', 'C/List': 'list-1', 'S/Startup': 'startup-1'}
    V2 = {'C/Copy': 'copy-2', 'C/List': 'list-1', 'C/New': 'new-2', 'S/Startup': 'startup-2'}

    def tearDown(self):
        lib.apkg_test_fault(0, 0)

    def channel(self, d):
        key_fixture(d / 'key')
        channel_key(d / 'channel-key')
        self.pinned = grant(channel=True)
        (d / 'policy.json').write_bytes(self.pinned)
        SystemClients.published(self, d, self.V1, '1.0', 'v1')
        root = d / 'root'
        root.mkdir()
        return root, ('channel-update', root, d / 'channel', CHANNEL, 'aros-system', '--policy', d / 'policy.json')

    def test_a_store_elsewhere_leaves_the_root_and_its_volume_alone(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            root, update = self.channel(d)
            store = d / 'other-volume'
            store.mkdir()
            done = clients.cli(*update, '--target', TARGET, '--yes', '--store', store)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            # Beside the root: its archived journals, nothing else.
            beside = sorted(x.name.rsplit('.', 1)[0] for x in d.iterdir() if x.name.startswith('root.'))
            self.assertEqual(beside, ['root.apkg-history', 'root.apkg-history'])
            # Inside the root: the state and the system's own files.
            self.assertEqual(sorted(x.name for x in root.iterdir()), ['.apkg', 'C', 'S'])
            self.assertEqual(sorted(x.name for x in (root / '.apkg').iterdir()), ['state'])
            # The store holds what repair of the installed version needs.
            self.assertEqual(sorted(x.name for x in store.iterdir()), ['.apkg-owner', 'archives', 'objects'])
            self.assertEqual(sorted(x.name for x in (store / 'objects').iterdir()), listed_objects(root))
            self.assertEqual(list((store / 'archives').iterdir()), [])
            SystemClients.published(self, d, self.V2, '2.0', 'v2')
            done = clients.cli('--store', store, *update, '--yes')
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual(tree(root)['C/New'], 'new-2')
            self.assertEqual(sorted(x.name for x in (store / 'objects').iterdir()), listed_objects(root))
            self.assertNotIn(hashlib.sha256(b'copy-1').hexdigest(), listed_objects(root))
            self.assertEqual(list((store / 'archives').iterdir()), [])
            self.assertFalse(any(x.name.startswith('root.apkg-objects') or x.name.startswith('root.apkg-archives') for x in d.iterdir()))
            # The store must exist: the engine does not invent a volume.
            refused = clients.cli(*update, '--yes', '--store', d / 'absent')
            self.assertEqual(refused.returncode, 5, refused.stdout + refused.stderr)

    def test_two_roots_cannot_prune_each_others_external_store(self):
        with tempfile.TemporaryDirectory() as folder:
            d = Path(folder)
            root, update = self.channel(d)
            store = d / 'store'
            store.mkdir()
            seed = d / 'policy.json'
            self.assertEqual(clients.cli(*update, '--target', TARGET, '--store', store, '--yes').returncode, 0)
            before = {p.name: p.read_bytes() for p in (store / 'objects').iterdir()}
            other = d / 'other'
            other.mkdir()
            self.assertEqual(clients.cli('init', other, TARGET, seed, '--yes').returncode, 0)
            self.assertEqual(clients.cli('prune', other, '--store', store).returncode, CONFLICT)
            self.assertEqual(before, {p.name: p.read_bytes() for p in (store / 'objects').iterdir()})
            self.assertEqual(clients.cli('verify', root, 'aros-system', '--store', store).returncode, 0)

    def test_prune_removes_only_what_the_engine_stored_and_nothing_needed(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            root, update = self.channel(d)
            store = d / 'store'
            store.mkdir()
            self.assertEqual(clients.cli(*update, '--target', TARGET, '--yes', '--store', store).returncode, 0)
            objects, archives = store / 'objects', store / 'archives'
            (objects / ('ab' * 32)).write_bytes(b'unreferenced object')
            (objects / ('.incoming.' + 'cd' * 16)).write_bytes(b'unfinished')
            (archives / ('ef' * 32 + '.pkg')).write_bytes(b'old archive')
            (objects / 'notes.txt').write_text('not the engine\'s')
            (objects / ('12' * 32 + '.bak')).write_text('not the engine\'s')
            (archives / ('34' * 32)).mkdir()
            (store / 'unrelated').write_text('beside the store directories')
            done = clients.cli('prune', root, '--store', store)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn('pruned files=3 bytes=40', done.stdout)
            self.assertEqual(sorted(x.name for x in objects.iterdir()), sorted(listed_objects(root) + ['notes.txt', '12' * 32 + '.bak']))
            self.assertEqual([x.name for x in archives.iterdir()], ['34' * 32])
            self.assertTrue((store / 'unrelated').exists())
            self.assertIn('pruned files=0 bytes=0', clients.cli('prune', root, '--store', store).stdout)
            # Repair still has everything it needs.
            (root / 'C/Copy').write_text('damaged')
            done, text = confirmed(self, 'repair', root, 'aros-system', '--store', store)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual((root / 'C/Copy').read_text(), 'copy-1')

    def test_prune_refuses_under_a_pending_transaction(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            root, update = self.channel(d)
            self.assertEqual(clients.cli(*update, '--target', TARGET, '--yes').returncode, 0)
            SystemClients.published(self, d, self.V2, '2.0', 'v2')
            options = update_options(root, d / 'channel', self.pinned)
            pid = os.fork()
            if pid == 0:
                lib.apkg_test_fault_after(203, 2, 0)
                result = C.c_void_p()
                lib.apkg_client_channel_update(C.byref(options), C.byref(result), None)
                os._exit(98)
            self.assertEqual(os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]), 99)
            before = sorted(x.name for x in Path(str(root) + '.apkg-objects').iterdir())
            refused = clients.cli('prune', root)
            self.assertEqual(refused.returncode, RECOVERY, refused.stdout + refused.stderr)
            self.assertEqual(sorted(x.name for x in Path(str(root) + '.apkg-objects').iterdir()), before)
            self.assertEqual(clients.cli('recover', root).returncode, 0)
            self.assertEqual(sorted(x.name for x in Path(str(root) + '.apkg-objects').iterdir()), listed_objects(root))

    def test_repair_says_when_the_store_lacks_the_bytes_and_the_updater_refetches(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            root, update = self.channel(d)
            store = d / 'store'
            store.mkdir()
            self.assertEqual(clients.cli(*update, '--target', TARGET, '--yes', '--store', store).returncode, 0)
            (root / 'C/Copy').write_text('damaged')
            (root / 'C/List').unlink()
            # Without the store it was installed with there is nothing to repair from.
            refused = clients.cli('repair', root, 'aros-system')
            self.assertEqual(refused.returncode, 5, refused.stdout + refused.stderr)
            self.assertIn('the store holds no bytes for this file', refused.stderr)
            self.assertEqual((root / 'C/Copy').read_text(), 'damaged')
            self.assertFalse(Path(str(root) + '.apkg-objects').exists())
            # The same when the store itself lost them.
            for held in (store / 'objects').iterdir():
                held.unlink()
            self.assertEqual(clients.cli('repair', root, 'aros-system', '--store', store).returncode, 5)
            # The updater is at the newest build: asked to repair, it fetches
            # that build's objects again and restores the files.
            self.assertIn('outcome=0', clients.cli(*update, '--yes', '--store', store).stdout)
            self.assertEqual((root / 'C/Copy').read_text(), 'damaged')
            done = clients.cli(*update, '--yes', '--repair', '--store', store)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn('outcome=1', done.stdout)
            self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-1', 'C/List': 'list-1', 'S': None, 'S/Startup': 'startup-1'})
            self.assertEqual(sorted(x.name for x in (store / 'objects').iterdir()), listed_objects(root))
            # Nothing damaged: nothing committed.
            generation = json.loads((root / '.apkg/state').read_bytes())['body']['generation']
            self.assertIn('outcome=0', clients.cli(*update, '--yes', '--repair', '--store', store).stdout)
            self.assertEqual(json.loads((root / '.apkg/state').read_bytes())['body']['generation'], generation)

    def test_the_updater_refetches_objects_a_pending_placement_lost(self):
        for damage in ('missing', 'corrupt'):
            with self.subTest(damage), tempfile.TemporaryDirectory() as d:
                d = Path(d)
                root, update = self.channel(d)
                store = d / 'store'
                store.mkdir()
                self.assertEqual(clients.cli(*update, '--target', TARGET, '--yes', '--store', store).returncode, 0)
                SystemClients.published(self, d, self.V2, '2.0', 'v2')
                options = update_options(root, d / 'channel', self.pinned, store=store)
                pid = os.fork()
                if pid == 0:
                    lib.apkg_test_fault_after(203, 2, 0)            # one of three files placed
                    result = C.c_void_p()
                    lib.apkg_client_channel_update(C.byref(options), C.byref(result), None)
                    os._exit(98)
                self.assertEqual(os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]), 99)
                for held in (store / 'objects').iterdir():
                    if damage == 'missing':
                        held.unlink()
                    else:
                        held.chmod(0o600)
                        held.write_bytes(b'not these bytes')
                # Recovery alone cannot finish, and says so without touching more.
                stuck = clients.cli('recover', root, '--store', store)
                self.assertEqual(stuck.returncode, RECOVERY, stuck.stdout + stuck.stderr)
                self.assertEqual(clients.cli('inspect', root).stdout.split()[0], f'root_state={PENDING}')
                # The next run of the updater fetches the same build again and finishes it.
                done = clients.cli(*update, '--yes', '--store', store)
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                self.assertEqual(tree(root), {'C': None, 'C/Copy': 'copy-2', 'C/List': 'list-1', 'C/New': 'new-2', 'S': None,
                                              'S/Startup': 'startup-2'})
                self.assertEqual(clients.cli('inspect', root).stdout.split()[0], f'root_state={READY}')
                self.assertEqual(leftovers(root), [])
                self.assertEqual(sorted(x.name for x in (store / 'objects').iterdir()), listed_objects(root))

    def test_library_contexts_take_the_store_and_prune(self):
        lib.apkg_store_prune.argtypes = [C.c_void_p, C.POINTER(C.c_uint64), C.POINTER(C.c_uint64), C.c_void_p]
        lib.apkg_store_prune.restype = C.c_int
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            store = d / 'store'
            store.mkdir()
            root = d / 'root'
            root.mkdir()
            code, c = open_ctx(root, 1, store=store)
            self.assertEqual(code, 0)
            code, s = snapshot(c)
            code, p = initialize_plan(c, s, seed=grant())
            code, a = approve(c, p)
            self.assertEqual(execute(c, p, a), (0, COMMITTED))
            apply(self, c, system([folder('C'), payload('C/Copy', 'copy-1')]))
            install_bundle(self, c, descriptor(name='demo'))
            self.assertFalse(Path(str(root) + '.apkg-objects').exists())
            (store / 'objects' / ('ab' * 32)).write_bytes(b'x')
            files, size = C.c_uint64(), C.c_uint64()
            self.assertEqual(lib.apkg_store_prune(c, C.byref(files), C.byref(size), None), 0)
            self.assertEqual((files.value, size.value), (1, 1))
            self.assertEqual([x.name for x in (store / 'objects').iterdir()], [hashlib.sha256(b'copy-1').hexdigest()])
            lib.apkg_close(c)
            code, reader = open_ctx(root, 0, store=store)
            self.assertEqual(lib.apkg_store_prune(reader, None, None, None), POLICY)
            lib.apkg_close(reader)
            self.assertNotEqual(open_ctx(root, 1, store=d / 'absent')[0], 0)


class Mirror(C.Structure):
    _fields_ = [('version', C.c_uint32), ('size', C.c_size_t), ('root', C.c_char_p), ('endpoint', C.c_char_p),
                ('identity', C.c_char_p), ('name', C.c_char_p), ('directory', C.c_char_p), ('store', C.c_char_p),
                ('observer', C.c_void_p)]


class MirrorResult(C.Structure):
    _fields_ = [('snapshot', Digest), ('descriptor', Digest), ('archive', Digest), ('sequence', C.c_uint64),
                ('revision', C.c_uint64), ('archive_bytes', C.c_uint64), ('version', C.c_char * 256),
                ('newer', C.c_int), ('installed', C.c_int), ('changed', C.c_int)]


lib.apkg_client_channel_mirror.argtypes = [C.POINTER(Mirror), C.POINTER(MirrorResult), C.c_void_p]
lib.apkg_client_channel_mirror.restype = C.c_int


def mirror_call(root, endpoint, directory, observer=None):
    options = Mirror(2, C.sizeof(Mirror), os.fsencode(root), os.fsencode(endpoint), CHANNEL.encode(), b'aros-system',
                     os.fsencode(directory), None, observer)
    result = MirrorResult()
    return lib.apkg_client_channel_mirror(C.byref(options), C.byref(result), None), result


def fields(text):
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)


class SystemMirror(unittest.TestCase):
    """Fetch while the network is up, apply from the mirror without it."""

    BIG = 'x' * 200000                                             # an archive of several chunks
    V1 = {'C/Copy': 'copy-1', 'C/Big': BIG, 'S/Startup': 'startup-1'}
    V2 = {'C/Copy': 'copy-2', 'C/Big': BIG, 'C/New': 'new-2', 'S/Startup': 'startup-2'}

    def tearDown(self):
        lib.apkg_test_fault(0, 0)

    def prepared(self, d):
        key_fixture(d / 'key')
        channel_key(d / 'channel-key')
        seed = d / 'policy.json'
        seed.write_bytes(grant(channel=True))
        SystemClients.published(self, d, self.V1, '1.0', 'v1')
        root = d / 'root'
        root.mkdir()
        self.assertEqual(clients.cli('init', root, TARGET, seed, '--yes').returncode, 0)
        return root, seed, d / 'channel', d / 'mirror'

    def everything(self, d):
        """Every file under d outside the mirror and the channel, with its bytes."""
        return {str(x.relative_to(d)): x.read_bytes() for x in sorted(d.rglob('*'))
                if x.is_file() and x.relative_to(d).parts[0] in ('root', 'policy.json')
                or x.name.startswith('root.') and x.is_file()}

    def mirrored(self, mirror):
        return sorted(str(x.relative_to(mirror)) for x in mirror.rglob('*') if x.is_file())

    def test_mirror_over_http_then_update_with_the_server_stopped(self):
        from test_http_channel import serve
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            root, seed, channel, mirror = self.prepared(d)
            before = self.everything(d)
            beside = sorted(x.name for x in d.iterdir())
            with serve(channel) as (url, server):
                done = clients.cli('channel-mirror', root, url, CHANNEL, 'aros-system', mirror)
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                first = fields(done.stdout)
                self.assertEqual((first['version'], first['revision'], first['update'], first['mirror']),
                                 ('1.0', '1', 'ready', 'updated'))
                head = (channel / 'current').read_text().strip()
                self.assertEqual(first['snapshot'], head)
                # The snapshot exactly as served, and the one archive it needs.
                self.assertEqual(self.mirrored(mirror), ['archives/' + first['archive'] + '.pkg', 'current',
                                                         f'snapshots/{head}/channel.json', f'snapshots/{head}/channel.signature.json'])
                for name in self.mirrored(mirror):
                    self.assertEqual((mirror / name).read_bytes(), (channel / name).read_bytes(), name)
                # The root was only read.
                self.assertEqual(self.everything(d), before)
                self.assertEqual(sorted(x.name for x in d.iterdir()), sorted(beside + ['mirror']))
                # Complete already: nothing is fetched again.
                archives = len([r for r in server.requests if r.startswith('/archives/')])
                again = fields(clients.cli('channel-mirror', root, url, CHANNEL, 'aros-system', mirror).stdout)
                self.assertEqual((again['mirror'], again['update'], again['snapshot']), ('unchanged', 'ready', head))
                self.assertEqual(len([r for r in server.requests if r.startswith('/archives/')]), archives)
            # No network from here on.
            update = ('channel-update', root, mirror, CHANNEL, 'aros-system', '--policy', seed, '--yes')
            done = clients.cli(*update)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual(tree(root), {'C': None, 'C/Big': self.BIG, 'C/Copy': 'copy-1', 'S': None, 'S/Startup': 'startup-1'})
            SystemClients.published(self, d, self.V2, '2.0', 'v2')
            with serve(channel) as (url, server):
                done = fields(clients.cli('channel-mirror', root, url, CHANNEL, 'aros-system', mirror).stdout)
                self.assertEqual((done['version'], done['update'], done['mirror']), ('2.0', 'ready', 'updated'))
                head = (channel / 'current').read_text().strip()
                # What the mirror no longer references is gone.
                self.assertEqual(self.mirrored(mirror), ['archives/' + done['archive'] + '.pkg', 'current',
                                                         f'snapshots/{head}/channel.json', f'snapshots/{head}/channel.signature.json'])
                self.assertEqual([x.name for x in (mirror / 'snapshots').iterdir()], [head])
            done = clients.cli(*update)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual(tree(root), {'C': None, 'C/Big': self.BIG, 'C/Copy': 'copy-2', 'C/New': 'new-2', 'S': None,
                                          'S/Startup': 'startup-2'})
            self.assertIn('outcome=0', clients.cli(*update).stdout)
            # Installed now: the mirror says there is nothing to do.
            code, result = mirror_call(root, channel, mirror)
            self.assertEqual((code, result.newer, result.installed, result.changed, result.version, result.revision),
                             (0, 0, 1, 0, b'2.0', 1))

    def test_mirror_preserves_a_repository_and_foreign_directories(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            root, _, channel, mirror = self.prepared(d)
            alias = d / 'alias'
            alias.symlink_to(channel, target_is_directory=True)
            before = {p.relative_to(channel): p.read_bytes() for p in channel.rglob('*') if p.is_file()}
            for destination in (channel, alias):
                self.assertNotEqual(mirror_call(root, channel, destination)[0], 0)
            self.assertEqual(before, {p.relative_to(channel): p.read_bytes() for p in channel.rglob('*') if p.is_file()})
            mirror.mkdir()
            (mirror / 'notes').write_text('keep')
            self.assertNotEqual(mirror_call(root, channel, mirror)[0], 0)
            self.assertEqual(list(mirror.iterdir()), [mirror / 'notes'])

    def test_an_interrupted_mirror_is_never_offered_as_complete(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            root, seed, channel, mirror = self.prepared(d)
            # Killed in the middle of the archive, with no earlier mirror.
            pid = os.fork()
            if pid == 0:
                lib.apkg_test_fault_after(1301, 2, 1)
                mirror_call(root, channel, mirror)
                os._exit(98)
            self.assertEqual(os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]), 99)
            self.assertFalse((mirror / 'current').exists())
            partial = [x for x in (mirror / 'archives').iterdir()]
            self.assertEqual(len(partial), 1)
            self.assertTrue(partial[0].name.startswith('.incoming.'))
            self.assertLess(partial[0].stat().st_size, (channel / 'archives' / next((channel / 'archives').iterdir()).name).stat().st_size)
            refused = clients.cli('channel-update', root, mirror, CHANNEL, 'aros-system', '--policy', seed, '--yes')
            self.assertNotEqual(refused.returncode, 0)
            self.assertEqual(tree(root), {})
            code, result = mirror_call(root, channel, mirror)
            self.assertEqual((code, result.changed, result.newer), (0, 1, 1))
            self.assertEqual([x.name for x in (mirror / 'archives').iterdir()], [bytes(result.archive.bytes).hex() + '.pkg'])
            first = (mirror / 'current').read_text()
            # A newer snapshot, killed after its archive arrived and before
            # the reference moved: the mirror is still the previous, whole one.
            SystemClients.published(self, d, self.V2, '2.0', 'v2')
            for point in (1302, 1303):
                pid = os.fork()
                if pid == 0:
                    lib.apkg_test_fault(point, 2)
                    mirror_call(root, channel, mirror)
                    os._exit(98)
                self.assertEqual(os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]), 99)
                self.assertEqual((mirror / 'current').read_text(), first)
            done = clients.cli('channel-update', root, mirror, CHANNEL, 'aros-system', '--policy', seed, '--yes')
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual((root / 'C/Copy').read_text(), 'copy-1')       # the previous mirror's build
            code, result = mirror_call(root, channel, mirror)
            self.assertEqual((code, result.changed, result.newer, result.version), (0, 1, 1, b'2.0'))
            self.assertEqual([x.name for x in (mirror / 'archives').iterdir()], [bytes(result.archive.bytes).hex() + '.pkg'])
            done = clients.cli('channel-update', root, mirror, CHANNEL, 'aros-system', '--policy', seed, '--yes')
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual((root / 'C/Copy').read_text(), 'copy-2')

    def test_damaged_and_stale_mirror_content_is_replaced(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            root, seed, channel, mirror = self.prepared(d)
            code, result = mirror_call(root, channel, mirror)
            self.assertEqual(code, 0)
            archive = mirror / 'archives' / (bytes(result.archive.bytes).hex() + '.pkg')
            good = archive.read_bytes()
            head = (mirror / 'current').read_text()
            # A damaged archive under its final name, and a leftover partial file.
            archive.write_bytes(good[:len(good) // 2])
            (mirror / 'archives' / ('.incoming.' + 'ab' * 16)).write_bytes(b'partial')
            code, result = mirror_call(root, channel, mirror)
            self.assertEqual((code, result.changed), (0, 1))
            self.assertEqual(archive.read_bytes(), good)
            self.assertEqual([x.name for x in (mirror / 'archives').iterdir()], [archive.name])
            # A reference to a snapshot the mirror does not hold.
            (mirror / 'current').write_text('ab' * 32 + '\n')
            self.assertNotEqual(clients.cli('channel-open', root, mirror, CHANNEL).returncode, 0)
            code, result = mirror_call(root, channel, mirror)
            self.assertEqual((code, result.changed), (0, 1))
            self.assertEqual((mirror / 'current').read_text(), head)
            # A snapshot file that was altered.
            snapshot = mirror / 'snapshots' / head.strip() / 'channel.json'
            snapshot.write_bytes(snapshot.read_bytes()[:-1] + b' ')
            code, result = mirror_call(root, channel, mirror)
            self.assertEqual((code, result.changed), (0, 1))
            self.assertEqual(snapshot.read_bytes(), (channel / 'snapshots' / head.strip() / 'channel.json').read_bytes())
            self.assertEqual(mirror_call(root, channel, mirror)[1].changed, 0)
            done = clients.cli('channel-update', root, mirror, CHANNEL, 'aros-system', '--policy', seed, '--yes')
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_a_tampered_or_cancelled_transfer_publishes_nothing(self):
        from test_http_channel import serve
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            root, seed, channel, mirror = self.prepared(d)
            with serve(channel) as (url, server):
                server.mode = 'tamper'
                refused = clients.cli('channel-mirror', root, url, CHANNEL, 'aros-system', mirror)
                self.assertEqual(refused.returncode, 10, refused.stdout + refused.stderr)   # INTEGRITY
                self.assertFalse((mirror / 'current').exists())
                self.assertEqual(list((mirror / 'archives').iterdir()), [])
                server.mode = 'normal'
            # Cancelled from the observer while the archive is copied.
            seen = []
            callback = EVENT_FN(lambda user, event: seen.append(event.contents.phase) or (1 if event.contents.phase == 1 else 0))
            observer = Observer(callback, None, None)
            code, result = mirror_call(root, channel, mirror, C.cast(C.byref(observer), C.c_void_p))
            self.assertEqual(code, CANCELLED)
            self.assertIn(1, seen)                                  # FETCH
            self.assertFalse((mirror / 'current').exists())
            self.assertEqual(list((mirror / 'archives').iterdir()), [])
            # Refusals that write nothing at all.
            self.assertEqual(mirror_call(d / 'no-root', channel, d / 'm2')[0], POLICY)
            self.assertFalse((d / 'm2').exists())
            options = Mirror(2, C.sizeof(Mirror), os.fsencode(root), os.fsencode(channel), CHANNEL.encode(), b'unknown-name',
                             os.fsencode(d / 'm3'), None, None)
            self.assertEqual(lib.apkg_client_channel_mirror(C.byref(options), C.byref(MirrorResult()), None), 5)
            self.assertFalse((d / 'm3').exists())


if __name__ == '__main__':
    unittest.main()
