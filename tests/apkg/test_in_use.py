# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tomasz Staniak
"""A live package tree that something still uses refuses the change.

The host has no real check; fault points 1301 (before anything is written),
1302 (right before the first tree is renamed) and 1303 (right before a later
tree is renamed) stand for the answer "held"."""
import ctypes as C
import os
import tempfile
import unittest
from pathlib import Path
from support import *
from test_operations import install, op, payload
from test_relations import related, requirement, resolve

BUSY, UNCHANGED, COMMITTED, RESTORED = 7, 0, 1, 2


class ErrorView(C.Structure):
    _fields_ = [('code', C.c_int), ('message', Text), ('field', Text),
                ('subject', Text)]


lib.apkg_error_get.argtypes = [C.c_void_p]
lib.apkg_error_get.restype = C.POINTER(ErrorView)
lib.apkg_error_free.argtypes = [C.c_void_p]


def run(c, p):
    """Approve and execute; return (code, outcome, message, subject)."""
    code, a = approve(c, p)
    assert code == 0
    result, error = C.c_void_p(), C.c_void_p()
    code = lib.apkg_execute(c, p, a, None, C.byref(result), C.byref(error))
    outcome = lib.apkg_result_get(result).contents.outcome
    message = subject = ''
    if error:
        view = lib.apkg_error_get(error).contents
        message, subject = raw(view.message).decode(), raw(view.subject).decode()
        lib.apkg_error_free(error)
    lib.apkg_result_free(result)
    lib.apkg_approval_free(a)
    return code, outcome, message, subject


def newer():
    obj = json.loads(descriptor(files=[payload('hello', 'second\n')]))
    obj['body']['version'] = '2.0'
    return json.dumps(obj, sort_keys=True, separators=(',', ':')).encode()


def upgrade(c):
    data = newer()
    code, s = snapshot(c)
    assert code == 0
    code, pkg = package(c, data, sign(data))
    assert code == 0
    p = C.c_void_p()
    code = lib.apkg_plan_upgrade(c, s, pkg, C.byref(p), None)
    lib.apkg_snapshot_free(s)
    lib.apkg_package_free(pkg)
    return code, p


def archives(root):
    return sorted(root.parent.glob(root.name + '.apkg-history.*'))


def siblings(root):
    return sorted(x.name for x in root.parent.iterdir())


class InUse(unittest.TestCase):
    def tearDown(self):
        lib.apkg_test_fault(0, 0)

    def prepared(self, d, action):
        """A root with demo installed and the plan of one operation on it."""
        root = Path(d) / 'root'
        c = init(root)
        install(c, descriptor())
        if action == 'upgrade':
            code, p = upgrade(c)
        elif action == 'repair':
            (root / 'demo/hello').write_text('damage')
            code, p = op(c, 'repair')
        elif action == 'rollback':
            code, p = upgrade(c)
            self.assertEqual(code, 0)
            self.assertEqual(run(c, p)[:2], (0, COMMITTED))
            lib.apkg_plan_free(p)
            history = next(x for x in archives(root) if (x / 'old-0').exists())
            code, p = op(c, 'rollback', str(history))
        else:
            code, p = op(c, 'remove')
        self.assertEqual(code, 0)
        return root, c, p

    def test_first_check_refuses_with_nothing_written(self):
        for action in ('upgrade', 'remove', 'repair', 'rollback'):
            with self.subTest(action=action), \
                    tempfile.TemporaryDirectory() as d:
                root, c, p = self.prepared(d, action)
                state = (root / '.apkg/state').read_bytes()
                content = (root / 'demo/hello').read_bytes()
                names = siblings(root)
                lib.apkg_test_fault(1301, 1)
                code, outcome, message, subject = run(c, p)
                lib.apkg_test_fault(0, 0)
                self.assertEqual((code, outcome), (BUSY, UNCHANGED))
                self.assertEqual(subject, str(root.resolve() / 'demo'))
                self.assertIn('in use', message)
                self.assertIn('close', message)
                self.assertIn('restarting the system', message)
                # No draft, no journal, no archive: nothing to recover.
                self.assertEqual(siblings(root), names)
                self.assertEqual((root / '.apkg/state').read_bytes(), state)
                self.assertEqual((root / 'demo/hello').read_bytes(), content)
                self.assertEqual(recover(c), (0, UNCHANGED))
                lib.apkg_plan_free(p)
                lib.apkg_close(c)

    def test_second_check_refuses_through_the_journal(self):
        for action in ('upgrade', 'remove', 'repair', 'rollback'):
            with self.subTest(action=action), \
                    tempfile.TemporaryDirectory() as d:
                root, c, p = self.prepared(d, action)
                state = (root / '.apkg/state').read_bytes()
                content = (root / 'demo/hello').read_bytes()
                before = len(archives(root))
                lib.apkg_test_fault(1302, 1)
                code, outcome, message, subject = run(c, p)
                lib.apkg_test_fault(0, 0)
                self.assertEqual((code, outcome), (BUSY, RESTORED))
                self.assertEqual(subject, str(root.resolve() / 'demo'))
                self.assertFalse(
                    Path(str(root) + '.apkg-transaction').exists())
                # The refused attempt is archived like any restored one.
                self.assertEqual(len(archives(root)), before + 1)
                self.assertEqual((root / '.apkg/state').read_bytes(), state)
                self.assertEqual((root / 'demo/hello').read_bytes(), content)
                self.assertEqual(recover(c), (0, UNCHANGED))
                lib.apkg_plan_free(p)
                lib.apkg_close(c)

    def test_repeat_after_refusal_succeeds(self):
        for point in (1301, 1302):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                root, c, p = self.prepared(d, 'upgrade')
                lib.apkg_test_fault(point, 1)
                self.assertEqual(run(c, p)[0], BUSY)
                lib.apkg_test_fault(0, 0)
                lib.apkg_plan_free(p)
                code, p = upgrade(c)
                self.assertEqual(code, 0)
                self.assertEqual(run(c, p)[:2], (0, COMMITTED))
                self.assertEqual((root / 'demo/hello').read_text(), 'second\n')
                lib.apkg_plan_free(p)
                lib.apkg_close(c)

    def two_trees(self, d):
        """Two installed packages and one plan that moves both live trees."""
        root = Path(d) / 'root'
        c = init(root)
        a = related('a', depends=[requirement('b')])
        b = related('b', depends=[requirement('a')])
        code, p = resolve(c, [a], [b])
        self.assertEqual(code, 0)
        self.assertEqual(run(c, p)[:2], (0, COMMITTED))
        lib.apkg_plan_free(p)
        history = next(x for x in archives(root) if (x / 'decision').exists()
                       and b'"name":"b"' in (x / 'plan').read_bytes())
        code, p = op(c, 'rollback', str(history))
        self.assertEqual(code, 0)
        return root, c, p

    def test_later_tree_held_puts_the_first_one_back(self):
        with tempfile.TemporaryDirectory() as d:
            root, c, p = self.two_trees(d)
            state = (root / '.apkg/state').read_bytes()
            lib.apkg_test_fault(1303, 1)
            code, outcome, message, subject = run(c, p)
            lib.apkg_test_fault(0, 0)
            self.assertEqual((code, outcome), (BUSY, RESTORED))
            self.assertEqual(subject, str(root.resolve() / 'b'))
            self.assertEqual((root / 'a/hello').read_bytes(), b'hello\n')
            self.assertEqual((root / 'b/hello').read_bytes(), b'hello\n')
            self.assertEqual((root / '.apkg/state').read_bytes(), state)
            self.assertFalse(Path(str(root) + '.apkg-transaction').exists())
            self.assertEqual(recover(c), (0, UNCHANGED))
            lib.apkg_plan_free(p)
            lib.apkg_close(c)

    def test_death_at_the_second_check_recovers_to_the_old_tree(self):
        for point in (1302, 1303):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                root, c, p = self.two_trees(d)
                state = (root / '.apkg/state').read_bytes()
                pid = os.fork()
                if pid == 0:
                    lib.apkg_test_fault(point, 2)
                    run(c, p)
                    os._exit(98)
                _, status = os.waitpid(pid, 0)
                self.assertEqual(os.waitstatus_to_exitcode(status), 99)
                self.assertEqual(recover(c), (0, RESTORED))
                self.assertEqual((root / 'a/hello').read_bytes(), b'hello\n')
                self.assertEqual((root / 'b/hello').read_bytes(), b'hello\n')
                self.assertEqual((root / '.apkg/state').read_bytes(), state)
                lib.apkg_plan_free(p)
                lib.apkg_close(c)

    def test_install_of_a_new_package_is_never_refused(self):
        # Nothing live is moved, so there is nothing to hold.
        for point in (1301, 1302, 1303):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                root = Path(d) / 'root'
                c = init(root)
                install(c, descriptor())
                lib.apkg_test_fault(point, 1)
                code, p = resolve(c, [related('fresh')])
                self.assertEqual(code, 0)
                self.assertEqual(run(c, p)[:2], (0, COMMITTED))
                lib.apkg_test_fault(0, 0)
                lib.apkg_plan_free(p)
                lib.apkg_close(c)


if __name__ == '__main__':
    unittest.main()
