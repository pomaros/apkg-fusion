# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Latest rollback selects a validated package transaction, never path ordering."""
import ctypes as C
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from support import *
import test_client as clients
import test_operations as operations
import test_relations as relations

lib.apkg_plan_rollback_last.argtypes = [C.c_void_p, C.c_void_p, Text, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_plan_rollback_last.restype = C.c_int
lib.apkg_plan_sources.argtypes = [C.c_void_p, C.c_void_p, Text, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_plan_sources.restype = C.c_int


def plan_last(ctx, name='demo', captured=None):
    if captured is None:
        code, captured = snapshot(ctx)
        assert code == 0
        owned = True
    else:
        owned = False
    data = name.encode()
    out = C.c_void_p(123)
    code = lib.apkg_plan_rollback_last(ctx, captured,
        Text(C.cast(C.c_char_p(data), C.c_void_p), len(data)), C.byref(out), None)
    if owned:
        lib.apkg_snapshot_free(captured)
    return code, out


def upgrade(ctx):
    data = json.loads(descriptor(files=[operations.payload('config', 'default', True),
                                        operations.payload('program', 'v2')]))
    data['body']['version'] = '2.0'
    data = wire('descriptor', data['body'])
    code, s = snapshot(ctx)
    assert code == 0
    code, pkg = package(ctx, data, sign(data))
    assert code == 0
    out = C.c_void_p()
    code = lib.apkg_plan_upgrade(ctx, s, pkg, C.byref(out), None)
    assert code == 0
    assert operations.run_plan(ctx, out) == (0, 1)
    lib.apkg_plan_free(out)
    lib.apkg_package_free(pkg)
    lib.apkg_snapshot_free(s)


def prepared(root):
    ctx = init(root)
    operations.install(ctx, descriptor(files=[operations.payload('config', 'default', True),
                                              operations.payload('program', 'v1')]))
    (root / 'demo/config').write_text('personal config')
    upgrade(ctx)
    return ctx


def history(root):
    current = (root / '.apkg/state').read_bytes()
    return next(path for path in root.parent.glob(root.name + '.apkg-history.*')
                if (path / 'new-state').read_bytes() == current)


class LatestRollback(unittest.TestCase):
    def tearDown(self):
        lib.apkg_test_fail_alloc(0)
        lib.apkg_test_fault(0, 0)

    def test_dependency_transaction_preview_and_undo_include_every_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'
            ctx = init(root)
            code, plan = relations.resolve(ctx,
                [relations.related('app', depends=[relations.requirement('provider')])],
                [relations.related('provider')])
            self.assertEqual(code, 0)
            self.assertEqual(operations.run_plan(ctx, plan), (0, 1))
            lib.apkg_plan_free(plan)
            lib.apkg_close(ctx)
            before = (root / '.apkg/state').read_bytes()
            preview = clients.cli('rollback-last', root, 'app')
            self.assertEqual(preview.returncode, 16, preview.stdout + preview.stderr)
            self.assertIn('undo the whole retained package transaction', preview.stdout)
            self.assertIn('app', preview.stdout)
            self.assertIn('provider', preview.stdout)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertTrue((root / 'app/hello').is_file())
            self.assertTrue((root / 'provider/hello').is_file())
            clients.OperationClients().commit('rollback-last', root, 'app')
            self.assertFalse((root / 'app').exists())
            self.assertFalse((root / 'provider').exists())
            self.assertEqual(json.loads((root / '.apkg/state').read_bytes())['body']['installed'], [])

    def test_named_undo_of_removal_restores_registration_and_managed_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'
            ctx = init(root)
            operations.install(ctx, descriptor())
            (root / 'demo/personal').write_text('keep')
            code, plan = operations.op(ctx, 'remove')
            self.assertEqual(code, 0)
            self.assertEqual(operations.run_plan(ctx, plan), (0, 1))
            lib.apkg_plan_free(plan)
            self.assertFalse((root / 'demo/hello').exists())
            code, plan = plan_last(ctx)
            self.assertEqual(code, 0)
            self.assertEqual(operations.run_plan(ctx, plan), (0, 1))
            lib.apkg_plan_free(plan)
            self.assertEqual((root / 'demo/hello').read_bytes(), b'hello\n')
            self.assertEqual((root / 'demo/personal').read_text(), 'keep')
            code, captured = snapshot(ctx)
            self.assertEqual(code, 0)
            self.assertEqual(lib.apkg_snapshot_installed_count(captured), 1)
            lib.apkg_snapshot_free(captured)
            lib.apkg_close(ctx)

    def test_cli_and_worker_restore_offline_and_preserve_personal_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            d = Path(temporary)
            roots = d / 'cli-root', d / 'worker-root'
            for root in roots:
                ctx = prepared(root)
                lib.apkg_close(ctx)
                (root / 'demo/personal').write_text('keep me')
                (root / 'demo/config').write_text('later edit')
            helper = clients.OperationClients()
            helper.commit('rollback-last', roots[0], 'demo')
            helper.worker(roots[1], 'demo', 12)
            helper.compare(*roots)
            for root in roots:
                self.assertEqual((root / 'demo/program').read_text(), 'v1')
                self.assertEqual((root / 'demo/config').read_text(), 'later edit')
                self.assertEqual((root / 'demo/personal').read_text(), 'keep me')
                self.assertFalse(root.with_name(root.name + '.apkg-transaction').exists())

    def test_unrelated_name_cannot_select_an_older_transaction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'
            ctx = init(root)
            operations.install(ctx, descriptor(name='a'))
            old = history(root)
            operations.install(ctx, descriptor(name='b'))
            latest = history(root)
            old.rename(root.with_name(root.name + '.apkg-history.' + 'f' * 32))
            latest.rename(root.with_name(root.name + '.apkg-history.' + '0' * 32))
            before = (root / '.apkg/state').read_bytes()
            code, plan = plan_last(ctx, 'a')
            self.assertEqual(code, 5)
            self.assertFalse(plan)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            code, plan = plan_last(ctx, 'b')
            self.assertEqual(code, 0)
            self.assertEqual(operations.run_plan(ctx, plan), (0, 1))
            lib.apkg_plan_free(plan)
            self.assertTrue((root / 'a/hello').is_file())
            self.assertFalse((root / 'b').exists())
            lib.apkg_close(ctx)

    def test_duplicate_applicable_archives_refuse_without_guessing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'
            ctx = prepared(root)
            shutil.copytree(history(root), root.with_name(root.name + '.apkg-history.' + 'f' * 32))
            before = (root / '.apkg/state').read_bytes()
            code, plan = plan_last(ctx)
            self.assertEqual(code, 14)
            self.assertFalse(plan)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertEqual((root / 'demo/program').read_text(), 'v2')
            lib.apkg_close(ctx)

    def test_damaged_incomplete_and_linked_current_archives_refuse(self):
        for damage, expected in [('plan', 10), ('new-state', 9), ('decision', 9), ('link', 9)]:
            with self.subTest(damage=damage), tempfile.TemporaryDirectory() as temporary:
                d = Path(temporary)
                root = d / 'root'
                ctx = prepared(root)
                retained = history(root)
                if damage == 'plan':
                    with (retained / 'plan').open('ab') as stream:
                        stream.write(b' ')
                elif damage == 'link':
                    outside = d / 'outside'
                    retained.rename(outside)
                    retained.symlink_to(outside, target_is_directory=True)
                else:
                    (retained / damage).unlink()
                before = (root / '.apkg/state').read_bytes()
                code, plan = plan_last(ctx)
                self.assertEqual(code, expected)
                self.assertFalse(plan)
                self.assertEqual((root / '.apkg/state').read_bytes(), before)
                self.assertEqual((root / 'demo/program').read_text(), 'v2')
                lib.apkg_close(ctx)

    def test_state_only_changes_are_skipped_and_current_sources_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'
            ctx = prepared(root)
            code, s = snapshot(ctx)
            self.assertEqual(code, 0)
            plan = C.c_void_p()
            sources = [{'name': 'unused', 'endpoint': '/not-contacted', 'id': 'cd' * 16}]
            raw = json.dumps(sources, sort_keys=True, separators=(',', ':')).encode()
            data = C.create_string_buffer(raw)
            code = lib.apkg_plan_sources(ctx, s, Text(C.cast(data, C.c_void_p), len(raw)), C.byref(plan), None)
            self.assertEqual(code, 0)
            self.assertEqual(operations.run_plan(ctx, plan), (0, 1))
            lib.apkg_plan_free(plan)
            lib.apkg_snapshot_free(s)
            code, plan = plan_last(ctx)
            self.assertEqual(code, 0)
            self.assertEqual(operations.run_plan(ctx, plan), (0, 1))
            lib.apkg_plan_free(plan)
            self.assertEqual((root / 'demo/program').read_text(), 'v1')
            self.assertEqual(json.loads((root / '.apkg/state').read_bytes())['body']['sources'], sources)
            lib.apkg_close(ctx)

    def test_policy_change_blocks_discovery_without_reverting_trust(self):
        with tempfile.TemporaryDirectory() as temporary:
            d = Path(temporary)
            root = d / 'root'
            ctx = prepared(root)
            lib.apkg_close(ctx)
            seed = d / 'new-policy'
            seed.write_bytes(wire('policy', {'authorities': []}))
            clients.OperationClients().commit('policy', root, seed)
            before = (root / '.apkg/state').read_bytes()
            result = clients.cli('rollback-last', root, 'demo')
            self.assertEqual(result.returncode, 5, result.stdout + result.stderr)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_changed_tree_invalidates_the_approved_plan(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'
            ctx = prepared(root)
            code, plan = plan_last(ctx)
            self.assertEqual(code, 0)
            code, approval = approve(ctx, plan)
            self.assertEqual(code, 0)
            before = (root / '.apkg/state').read_bytes()
            (root / 'demo/program').write_text('changed after preview')
            self.assertEqual(execute(ctx, plan, approval)[0], 8)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertEqual((root / 'demo/program').read_text(), 'changed after preview')
            lib.apkg_approval_free(approval)
            lib.apkg_plan_free(plan)
            lib.apkg_close(ctx)

    def test_allocation_failures_clear_outputs_and_snapshot_contexts_must_match(self):
        with tempfile.TemporaryDirectory() as temporary:
            d = Path(temporary)
            root = d / 'root'
            ctx = prepared(root)
            code, s = snapshot(ctx)
            self.assertEqual(code, 0)
            before = (root / '.apkg/state').read_bytes()
            for fault in range(1, 801):
                lib.apkg_test_fail_alloc(fault)
                try:
                    code, plan = plan_last(ctx, captured=s)
                finally:
                    lib.apkg_test_fail_alloc(0)
                self.assertIn(code, (0, 4), fault)
                if code: self.assertFalse(plan)
                lib.apkg_plan_free(plan)
            other = init(d / 'other')
            code, foreign = snapshot(other)
            self.assertEqual(code, 0)
            self.assertEqual(plan_last(ctx, captured=foreign)[0], 8)
            self.assertEqual(plan_last(ctx, '../demo')[0], 1)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            lib.apkg_snapshot_free(foreign)
            lib.apkg_snapshot_free(s)
            lib.apkg_close(other)
            lib.apkg_close(ctx)
