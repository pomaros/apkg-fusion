# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Model of the proposed state publication without ReplaceObject.

See docs/fusion/proposal-aros-replacement.md. This models the protocol over a
DOS-like filesystem whose rename never overwrites; it is not the engine. It
kills the writer and the recovery at every step and runs a reader at every
interleaving."""
import itertools
import unittest

OLD, NEW, PARTIAL, FOREIGN = b'old-state', b'new-state', b'new-st', b'someone else'


class Refused(Exception):
    pass


class Volume:
    """Names to bytes. rename refuses an existing destination, as DOS does."""
    def __init__(self, files):
        self.files = dict(files)

    def apply(self, op, torn=False):
        kind, *args = op
        if kind == 'write':
            name, data = args
            self.files[name] = data[:len(data) // 2 + 1] if torn else data
        elif kind == 'rename':
            src, dst = args
            if dst in self.files or src not in self.files:
                raise Refused(op)
            self.files[dst] = self.files.pop(src)
        elif kind == 'delete':
            self.files.pop(args[0], None)


PUBLISH = [('write', 'state-next', NEW), ('rename', 'state', 'state-prev'),
           ('rename', 'state-next', 'state'), ('delete', 'state-prev')]


def recovery_ops(v, decided):
    """The recovery table of the proposal, from what is on the volume only."""
    f = v.files
    state, prev, nxt = f.get('state'), f.get('state-prev'), f.get('state-next')
    if not decided:
        # Before the decision the protocol has not started: state is old and
        # only the scratch file may exist.
        if state != OLD or prev is not None:
            raise Refused('unresolved')
        return [('delete', 'state-next')] if nxt is not None else []
    if state == NEW:
        if prev not in (None, OLD) or nxt not in (None, NEW):
            raise Refused('unresolved')
        return [('delete', 'state-next'), ('delete', 'state-prev')]
    if state == OLD and prev is None:
        return list(PUBLISH)
    if state is None and prev == OLD:
        ops = [] if nxt == NEW else [('delete', 'state-next'), ('write', 'state-next', NEW)]
        return ops + PUBLISH[2:]
    raise Refused('unresolved')


def recover(v, decided, limit=None, torn=False):
    ops = recovery_ops(v, decided)
    for i, op in enumerate(ops):
        if limit is not None and i == limit:
            if torn and op[0] == 'write':
                v.apply(op, torn=True)
            return False
        v.apply(op)
    return True


def timeline():
    """(journal active, state bytes or None) at each point a reader can look."""
    v = Volume({'state': OLD})
    points = [(False, OLD), (True, OLD)]  # before the journal; journal, decision
    for op in PUBLISH:
        v.apply(op)
        points.append((True, v.files.get('state')))
    points.append((False, v.files.get('state')))  # journal archived
    return points


def read(points, t0, t1, t2, rule):
    """The snapshot reader: journal check at t0, state read at t1 and again at
    t2, journal check at t2."""
    pend1, (_, first), (pend2, again) = points[t0][0], points[t1], points[t2]
    if first is None:
        if pend1:
            return 'PENDING'
        if rule == 'once':  # the reader today
            return 'INVALID'
        if pend2:
            return 'PENDING'
        if rule == 'journal':  # rechecking the journal alone
            return 'INVALID'
        # Proposed: a missing state is checked again together with the
        # journal. A journal now, or a state now, means a writer moved on.
        return 'STALE' if again is not None else 'INVALID'
    if first != again or pend1 != pend2:
        return 'STALE'  # the caller retries
    return 'PENDING' if pend1 else first


class ReplacementModel(unittest.TestCase):
    def test_writer_killed_at_every_step_recovers_to_new(self):
        for k in range(len(PUBLISH) + 1):
            for torn in (False, True):
                v = Volume({'state': OLD})
                for i, op in enumerate(PUBLISH):
                    if i == k:
                        if torn and op[0] == 'write':
                            v.apply(op, torn=True)
                        break
                    v.apply(op)
                self.assertTrue(recover(v, decided=True), (k, torn))
                self.assertEqual(v.files, {'state': NEW}, (k, torn))

    def test_recovery_killed_again_still_finishes(self):
        for k, j in itertools.product(range(len(PUBLISH) + 1), range(len(PUBLISH) + 2)):
            for torn in (False, True):
                v = Volume({'state': OLD})
                for op in PUBLISH[:k]:
                    v.apply(op)
                recover(v, decided=True, limit=j, torn=torn)
                self.assertTrue(recover(v, decided=True))
                self.assertEqual(v.files, {'state': NEW}, (k, j, torn))

    def test_before_the_decision_state_stays_old(self):
        for torn in (False, True):
            v = Volume({'state': OLD})
            v.apply(PUBLISH[0], torn=torn)
            self.assertTrue(recover(v, decided=False))
            self.assertEqual(v.files, {'state': OLD})

    def test_reader_never_reports_invalid_with_the_second_check(self):
        points = timeline()
        for t0, t1, t2 in itertools.combinations_with_replacement(range(len(points)), 3):
            got = read(points, t0, t1, t2, 'journal+state')
            self.assertIn(got, ('PENDING', 'STALE', OLD, NEW), (t0, t1, t2))

    def test_weaker_reader_rules_report_invalid_during_the_interval(self):
        # Today a missing state is PENDING only when the first journal check
        # saw a journal. Rechecking the journal alone misses a writer that
        # finished before the recheck.
        points = timeline()
        for rule in ('once', 'journal'):
            seen = {read(points, t0, t1, t2, rule)
                    for t0, t1, t2 in itertools.combinations_with_replacement(range(len(points)), 3)}
            self.assertIn('INVALID', seen, rule)

    def test_foreign_bytes_are_refused_and_kept(self):
        for files in ({'state-prev': FOREIGN}, {'state-prev': OLD, 'state': FOREIGN},
                      {'state': NEW, 'state-prev': FOREIGN}, {'state': FOREIGN}):
            v = Volume(files)
            with self.assertRaises(Refused):
                recover(v, decided=True)
            self.assertEqual(v.files, files)

    def test_rename_never_overwrites(self):
        v = Volume({'state': OLD, 'state-prev': FOREIGN})
        with self.assertRaises(Refused):
            v.apply(('rename', 'state', 'state-prev'))
        self.assertEqual(v.files, {'state': OLD, 'state-prev': FOREIGN})


if __name__ == '__main__':
    unittest.main()
