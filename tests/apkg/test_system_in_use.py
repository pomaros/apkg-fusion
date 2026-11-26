# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tomasz Staniak
"""System packages and objects in use.

DOS does not delete a file that is open or locked, and system files are placed
after the decision. The engine therefore asks for every object it will replace
or remove before the decision and refuses with BUSY and the path. A host has no
such lock: fault points 1304 (before anything is written) and 1305 (last check
before the decision) stand for a held object, 1306 for a root that is the
running system, 210 and 211 and pkg_fs_test_hold for a holder that appears
after the decision."""
import ctypes as C
import os
import tempfile
import unittest
from support import *
from test_system_packages import (BASE, NEXT, COMMITTED, NEEDS_RECOVERY, RECOVERY, READY, PENDING,
                                  UNSUPPORTED, ErrorView, system_root, system, plan, apply, tree,
                                  root_state, leftovers, execute_error, folder)
from test_operations import payload
import test_system_packages as base

BUSY = 7
lib.pkg_fs_test_hold.argtypes = [C.c_int]
lib.apkg_error_get.restype = C.POINTER(ErrorView)


def recover_error(c):
    """(status, outcome, error code, subject) of one recovery."""
    r, e = C.c_void_p(), C.c_void_p()
    code = lib.apkg_recover(c, None, C.byref(r), C.byref(e))
    outcome = lib.apkg_result_get(r).contents.outcome if r else None
    view = lib.apkg_error_get(e).contents if e else None
    answer = (code, outcome, view.code if view else 0, raw(view.subject).decode() if view else '')
    lib.apkg_result_free(r)
    lib.apkg_error_free(e)
    return answer


def clear():
    lib.apkg_test_fault(0, 0)
    lib.pkg_fs_test_replace(0, 0)
    lib.pkg_fs_test_hold(0)


class SystemInUse(unittest.TestCase):
    def tearDown(self):
        clear()

    def upgrade(self, d):
        root, c = system_root(d)
        apply(self, c, system(BASE))
        code, p, subject = plan(c, system(NEXT, version='2.0'), 'upgrade')
        self.assertEqual(code, 0, subject)
        return root, c, p

    def test_a_held_object_refuses_before_anything_is_written(self):
        for point in (1304, 1305):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                root, c, p = self.upgrade(d)
                before = tree(root)
                state = (root / '.apkg/state').read_bytes()
                lib.apkg_test_fault(point, 1)
                code, outcome, message, subject = execute_error(c, p)
                lib.apkg_test_fault(0, 0)
                self.assertEqual(code, BUSY)
                self.assertNotEqual(outcome, NEEDS_RECOVERY)
                self.assertEqual(subject, 'C/Copy')            # the first object asked for
                self.assertIn('in use', message)
                self.assertIn('Nothing was written', message)
                self.assertEqual(root_state(c), READY)
                self.assertEqual(tree(root), before)
                self.assertEqual((root / '.apkg/state').read_bytes(), state)
                self.assertEqual(leftovers(root), [])
                # Released: the same request goes through.
                code, again, subject = plan(c, system(NEXT, version='2.0'), 'upgrade')
                self.assertEqual(code, 0, subject)
                self.assertEqual(run(c, again), (0, COMMITTED))
                self.assertEqual((root / 'C/Copy').read_text(), 'copy-2')
                lib.apkg_plan_free(again)
                lib.apkg_plan_free(p)
                lib.apkg_close(c)

    def test_an_install_onto_an_empty_root_asks_for_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            code, p, subject = plan(c, system(BASE))
            self.assertEqual(code, 0, subject)
            lib.apkg_test_fault(1304, 1)                        # reached only with an object to ask for
            self.assertEqual(run(c, p), (0, COMMITTED))
            lib.apkg_plan_free(p)
            lib.apkg_close(c)

    def test_the_running_system_is_refused_where_replacement_is_not_one_step(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system(BASE))
            code, p, subject = plan(c, system(NEXT, version='2.0'), 'upgrade')
            self.assertEqual(code, 0, subject)
            before = tree(root)
            lib.apkg_test_fault(1306, 1)                        # this root is the running system
            lib.pkg_fs_test_replace(1, 0)                       # a DOS without ReplaceObject
            code, outcome, message, subject = execute_error(c, p)
            self.assertEqual(code, UNSUPPORTED)
            self.assertIn('running system', message)
            self.assertEqual(root_state(c), READY)
            self.assertEqual(tree(root), before)
            clear()
            self.assertEqual(run(c, p), (0, COMMITTED))
            lib.apkg_plan_free(p)
            lib.apkg_close(c)

    @unittest.skipIf(os.environ.get('APKG_TEST_REPLACE_ASIDE'), 'the whole suite models a DOS without ReplaceObject')
    def test_the_running_system_is_not_refused_where_replacement_is_one_step(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            code, p, subject = plan(c, system(BASE))
            self.assertEqual(code, 0, subject)
            lib.apkg_test_fault(1306, 1)
            self.assertEqual(run(c, p), (0, COMMITTED))
            lib.apkg_plan_free(p)
            lib.apkg_close(c)

    def test_a_holder_after_the_decision_leaves_a_pending_root_that_recovers(self):
        # The old file was opened after the last check: on the aside path the
        # new file takes the name and the old bytes cannot be deleted.
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system(BASE))
            code, p, subject = plan(c, system(NEXT, version='2.0'), 'upgrade')
            self.assertEqual(code, 0, subject)
            lib.pkg_fs_test_replace(1, 0)
            lib.pkg_fs_test_hold(1)
            code, outcome, message, subject = execute_error(c, p)
            self.assertEqual((code, outcome), (RECOVERY, NEEDS_RECOVERY))
            self.assertEqual(subject, 'C/Copy')
            self.assertIn('run recovery', message)
            self.assertEqual(root_state(c), PENDING)
            self.assertEqual((root / 'C/Copy').read_text(), 'copy-2')    # the new file is in place
            self.assertEqual(len(leftovers(root)), 1)                    # the old bytes, still held
            # Still held: recovery says which file and changes nothing more.
            lib.pkg_fs_test_hold(0)
            lib.apkg_test_fault(210, 1)
            self.assertEqual(recover_error(c), (RECOVERY, NEEDS_RECOVERY, BUSY, 'C/Copy'))
            self.assertEqual(root_state(c), PENDING)
            # Released.
            clear()
            self.assertEqual(recover(c), (0, COMMITTED))
            self.assertEqual(root_state(c), READY)
            self.assertEqual(leftovers(root), [])
            self.assertEqual((root / 'C/New').read_text(), 'new-2')
            self.assertFalse((root / 'Libs/Extras/gone.library').exists())
            lib.apkg_plan_free(p)
            lib.apkg_close(c)

    def test_a_held_file_to_delete_after_the_decision_recovers(self):
        with tempfile.TemporaryDirectory() as d:
            root, c = system_root(d)
            apply(self, c, system(BASE))
            code, p, subject = plan(c, system(NEXT, version='2.0'), 'upgrade')
            self.assertEqual(code, 0, subject)
            lib.apkg_test_fault(211, 1)
            code, outcome, message, subject = execute_error(c, p)
            self.assertEqual((code, outcome), (RECOVERY, NEEDS_RECOVERY))
            self.assertEqual(subject, 'Libs/Extras/gone.library')
            self.assertEqual(root_state(c), PENDING)
            self.assertEqual(recover_error(c), (RECOVERY, NEEDS_RECOVERY, BUSY, 'Libs/Extras/gone.library'))
            clear()
            self.assertEqual(recover(c), (0, COMMITTED))
            self.assertEqual(root_state(c), READY)
            self.assertFalse((root / 'Libs/Extras').exists())
            lib.apkg_plan_free(p)
            lib.apkg_close(c)

    def test_process_death_at_the_new_checks_changes_nothing(self):
        helper = base.SystemRecovery()
        for point in (1304, 1305):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as d:
                root, c, p, a = helper.prepared(d)
                before = tree(root)
                helper.interrupted(root, c, p, a, lambda: lib.apkg_test_fault(point, 2))
                if root_state(c) == PENDING:
                    self.assertEqual(recover(c)[0], 0)
                self.assertEqual(root_state(c), READY)
                self.assertEqual(tree(root), before)
                lib.apkg_close(c)


def run(c, p):
    code, a = approve(c, p)
    assert code == 0
    result = execute(c, p, a)
    lib.apkg_approval_free(a)
    return result


if __name__ == '__main__':
    unittest.main()
