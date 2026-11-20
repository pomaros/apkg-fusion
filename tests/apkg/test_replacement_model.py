# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Model of the proposed state publication without ReplaceObject.

See docs/fusion/proposal-aros-replacement.md. This is a proposal, not the
engine: nothing here runs on AROS, and the engine still refuses mutation
without ReplaceObject. The model is a DOS-like volume whose rename never
overwrites. It kills the writer and the recovery at every step, runs recovery
over every combination of the scratch files, and runs readers whose every
check is its own event, across two consecutive transactions."""
import itertools
import unittest

OLD, NEW, NEWER, FOREIGN = b'old-state', b'new-state', b'newer-state', b'someone else'
TX1, TX2 = 'a1', 'b2'


class Refused(Exception):
    pass


class Volume:
    """Names to bytes. rename refuses an existing destination, as DOS does;
    an exclusive create refuses an existing name."""
    def __init__(self, files):
        self.files = dict(files)

    def apply(self, op, torn=False):
        kind, *args = op
        if kind == 'create':
            name, data = args
            if name in self.files:
                raise Refused(op)
            self.files[name] = data[:len(data) // 2] if torn else data
        elif kind == 'rename':
            src, dst = args
            if dst in self.files or src not in self.files:
                raise Refused(op)
            self.files[dst] = self.files.pop(src)
        elif kind == 'delete':
            self.files.pop(args[0])


def nxt(tx): return 'state-next.' + tx
def prev(tx): return 'state-prev.' + tx


def publish(tx, new):
    """Runs after the decision is durable. The scratch names carry the id of
    the transaction whose journal holds old-state and new-state."""
    return [('create', nxt(tx), new), ('rename', 'state', prev(tx)),
            ('rename', nxt(tx), 'state'), ('delete', prev(tx))]


def recovery_ops(v, tx, old, new, decided):
    """The recovery table of the proposal. A scratch file belongs to this
    transaction only when its name carries this transaction's id AND its
    bytes are what this transaction writes there: for state-next a prefix of
    new-state (a torn write) or all of it, for state-prev exactly old-state.
    Anything else under those names is foreign: kept, and the root refused.
    Files under any other name are never touched."""
    f = v.files
    state, p, n = f.get('state'), f.get(prev(tx)), f.get(nxt(tx))
    if p is not None and p != old:
        raise Refused('foreign ' + prev(tx))
    if n is not None and not new.startswith(n):
        raise Refused('foreign ' + nxt(tx))
    if not decided:
        # The protocol starts after the decision, so nothing of this
        # transaction carries these names yet. Whatever does is not ours.
        if state != old or p is not None or n is not None:
            raise Refused('unresolved before the decision')
        return []
    if state == new:
        return [('delete', name) for name, b in ((nxt(tx), n), (prev(tx), p)) if b is not None]
    if state == old and p is None:
        return ([('delete', nxt(tx))] if n is not None else []) + publish(tx, new)
    if state is None and p == old:
        redo = [] if n == new else ([('delete', nxt(tx))] if n is not None else []) + publish(tx, new)[:1]
        return redo + publish(tx, new)[2:]
    raise Refused('unresolved')


def recover(v, tx, old, new, decided, limit=None, torn=False):
    ops = recovery_ops(v, tx, old, new, decided)
    for i, op in enumerate(ops):
        if limit is not None and i == limit:
            if torn and op[0] == 'create':
                v.apply(op, torn=True)
            return False
        v.apply(op)
    return True


def owned(name, data, tx, old, new):
    """What recovery may delete or replace: the proof of ownership above."""
    if name == 'state':
        return data in (old, new)
    if name == nxt(tx):
        return new.startswith(data)
    if name == prev(tx):
        return data == old
    return False


class Recovery(unittest.TestCase):
    def test_writer_killed_at_every_step_recovers_to_new(self):
        for k in range(5):
            for torn in (False, True):
                v = Volume({'state': OLD})
                for i, op in enumerate(publish(TX1, NEW)):
                    if i == k:
                        if torn and op[0] == 'create':
                            v.apply(op, torn=True)
                        break
                    v.apply(op)
                self.assertTrue(recover(v, TX1, OLD, NEW, decided=True), (k, torn))
                self.assertEqual(v.files, {'state': NEW}, (k, torn))

    def test_recovery_killed_again_still_finishes(self):
        for k, j in itertools.product(range(5), range(6)):
            for torn in (False, True):
                v = Volume({'state': OLD})
                for op in publish(TX1, NEW)[:k]:
                    v.apply(op)
                recover(v, TX1, OLD, NEW, decided=True, limit=j, torn=torn)
                self.assertTrue(recover(v, TX1, OLD, NEW, decided=True))
                self.assertEqual(v.files, {'state': NEW}, (k, j, torn))

    def test_every_combination_before_and_after_the_decision(self):
        # state, and this transaction's state-next and state-prev, each with
        # nothing, our bytes, a torn write of ours or foreign bytes; strays
        # under another name and another transaction's name alongside.
        strays = {'state-next': FOREIGN, nxt('ff'): NEW}
        tried = refused = 0
        for decided, s, n, p in itertools.product((False, True), (None, OLD, NEW, FOREIGN),
                                                  (None, NEW, NEW[:4], b'', FOREIGN, OLD),
                                                  (None, OLD, NEW, FOREIGN)):
            files = dict(strays)
            for name, data in (('state', s), (nxt(TX1), n), (prev(TX1), p)):
                if data is not None:
                    files[name] = data
            v = Volume(files)
            case = (decided, s, n, p)
            tried += 1
            try:
                recover(v, TX1, OLD, NEW, decided)
            except Refused:
                refused += 1
                self.assertEqual(v.files, files, case)  # refused: nothing changed
                continue
            # Recovered: strays untouched, state old or new as the decision
            # says, and every file that changed or went was ours.
            self.assertEqual(v.files.get('state'), NEW if decided else OLD, case)
            self.assertEqual(set(v.files), set(strays) | {'state'}, case)
            for name in strays:
                self.assertEqual(v.files[name], files[name], case)
            for name, data in files.items():
                if v.files.get(name) != data:
                    self.assertTrue(owned(name, data, TX1, OLD, NEW), (case, name))
        self.assertEqual(tried, 192)
        self.assertTrue(0 < refused < tried)

    def test_foreign_scratch_files_are_kept_in_the_reviewed_cases(self):
        for decided, files in ((True, {'state': OLD, nxt(TX1): FOREIGN}),
                               (True, {prev(TX1): OLD, nxt(TX1): FOREIGN}),
                               (False, {'state': OLD, nxt(TX1): FOREIGN}),
                               (False, {'state': OLD, nxt(TX1): NEW})):
            v = Volume(files)
            with self.assertRaises(Refused):
                recover(v, TX1, OLD, NEW, decided)
            self.assertEqual(v.files, files)

    def test_a_scratch_name_without_this_transaction_is_never_ours(self):
        v = Volume({'state': OLD, 'state-next': NEW})
        self.assertTrue(recover(v, TX1, OLD, NEW, decided=True))
        self.assertEqual(v.files, {'state': NEW, 'state-next': NEW})

    def test_rename_never_overwrites(self):
        v = Volume({'state': OLD, prev(TX1): FOREIGN})
        with self.assertRaises(Refused):
            v.apply(('rename', 'state', prev(TX1)))
        self.assertEqual(v.files, {'state': OLD, prev(TX1): FOREIGN})


def world():
    """Two consecutive transactions. Each point is (writer holds the parent
    lock, journal present, state bytes or None); one reader event happens at
    one point. A writer takes the lock, publishes the journal, decides,
    publishes state, archives the journal and releases the lock."""
    v = Volume({'state': OLD})
    points = [(False, False, OLD)]
    for tx, new in ((TX1, NEW), (TX2, NEWER)):
        points += [(True, False, v.files['state']), (True, True, v.files['state'])]
        for op in publish(tx, new):
            v.apply(op)
            points.append((True, True, v.files.get('state')))
        points += [(True, False, v.files['state']), (False, False, v.files['state'])]
    return points


def read(points, times, rule):
    """The snapshot reader; every check is its own event at its own point.
    'once' is the reader of today. 'recheck' rechecks the journal and then
    state without the lock, as the first version of this proposal said.
    'lock' is the proposed rule: a missing state with no journal seen is
    decided only under the parent lock, tried once without waiting; a held
    lock means retry."""
    t = iter(times)
    journal = lambda: points[next(t)][1]
    state = lambda: points[next(t)][2]
    j1 = journal()
    s1 = state()
    if s1 is None:
        if j1:
            return 'PENDING'
        if rule == 'once':
            return 'INVALID'
        if rule == 'recheck':
            if journal():
                return 'PENDING'
            return 'STALE' if state() is not None else 'INVALID'
        held, j, s = points[next(t)]  # under the lock neither can move
        if held:
            return 'STALE'
        return 'PENDING' if j else 'STALE' if s is not None else 'INVALID'
    s2 = state()
    j2 = journal()
    if s1 != s2 or j1 != j2:
        return 'STALE'
    return 'PENDING' if j1 else s1


def schedules(points):
    return itertools.combinations_with_replacement(range(len(points)), 4)


class Readers(unittest.TestCase):
    def test_proposed_reader_never_reports_invalid_across_two_transactions(self):
        points = world()
        for times in schedules(points):
            got = read(points, times, 'lock')
            self.assertNotEqual(got, 'INVALID', times)
            if got == 'PENDING':
                self.assertTrue(any(p[1] for p in points[times[0]:times[-1] + 1]), times)
            elif got != 'STALE':
                # A state the root really had while the reader looked.
                self.assertIn(got, [p[2] for p in points[times[1]:times[-1] + 1]], times)

    def test_a_quiet_root_is_read_without_retry(self):
        points = world()
        for i, (held, journal, state) in enumerate(points):
            got = read(points, (i,) * 4, 'lock')
            if not journal:
                self.assertEqual(got, state, i)
            else:
                self.assertEqual(got, 'PENDING', i)

    def test_weaker_rules_report_invalid(self):
        # Today a missing state is PENDING only when the first journal check
        # saw a journal. Rechecking the journal and then state without the
        # lock is not enough either: the first transaction can finish, and
        # the second reach its own interval, between the two rechecks.
        points = world()
        for rule in ('once', 'recheck'):
            self.assertTrue([t for t in schedules(points) if read(points, t, rule) == 'INVALID'], rule)
        second = {i for i, p in enumerate(points) if p[2] is None and i > len(points) // 2}
        bad = [t for t in schedules(points) if read(points, t, 'recheck') == 'INVALID']
        self.assertTrue(all(t[3] in second for t in bad))
        one = world()[:len(points) // 2 + 1]  # the first transaction alone
        self.assertFalse([t for t in schedules(one) if read(one, t, 'recheck') == 'INVALID'])

    def test_a_dead_writer_is_pending_until_recovery(self):
        # Killed in the interval: the journal stays. On AROS the parent lock
        # is a public semaphore that stays held after its holder dies.
        for held in (True, False):
            self.assertEqual(read([(held, True, None)], (0,) * 4, 'lock'), 'PENDING')
        # A missing state with no journal and no writer is invalid.
        self.assertEqual(read([(False, False, None)], (0,) * 4, 'lock'), 'INVALID')


if __name__ == '__main__':
    unittest.main()
