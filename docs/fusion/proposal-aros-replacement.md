<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Proposal: state replacement on AROS systems without ReplaceObject

This is a proposal for review. It is not an agreed contract and does not change
the [journal](journal.md) until both maintainers accept it. **Nothing here is
implemented.** The engine is unchanged on this branch: it still refuses mutation
where `pkg_fs_atomic_replace_supported()` is false, and with this branch the
refusal says why. The tests named below run a Python model of the proposal, not
the engine.

## Where the engine needs replacement

The engine overwrites an existing regular file in two places. Everything else
it publishes uses an exclusive create, or a rename to a name that does not
exist yet. DOS `Rename` does both without replacing anything.

| Write | What it does | Needs overwrite |
|---|---|---|
| Journal draft, `ready`, `decision`, archive | Exclusive create, then rename of the draft directory to an absent `<root>.apkg-transaction`; archive by rename to an absent `<root>.apkg-history.<id>` | No |
| Payload: install, upgrade, repair, rollback, remove | Staged tree renamed to an absent package directory. The live tree is moved aside to an absent name in the journal | No |
| Initialization | Staged root or `.apkg` renamed to an absent name | No |
| Common state `.apkg/state` (`execute.c`, state publication) | `state-next` written, then `pkg_fs_replace_if_same(state)`, which uses `ReplaceObject` on AROS | **Yes** |
| Source profile, default `ENVARC:apkg-fusion.source` (`source_config.c`) | `pkg_fs_write_private`, a temporary file and then `ReplaceObject` | **Yes**, and outside any root journal |

So the question is narrower than "atomic writes": the common state and the
source profile need replacement. Payload publication and the journal do not.

## Guarantees needed

- A reader gets either the old state or the new one, or it reports PENDING. It
  never gets a mixture, and it never reports an empty or invalid root because a
  writer is part-way through.
- A process killed at any step leaves something the next writer recovers to
  exactly one of old or new, following the journal decision. Before the
  decision that is old; after it, new.
- A second writer is excluded by the existing parent lock. It never sees
  half-published state as ready.
- Power loss: no claim without evidence for each handler (see the volume
  requirements below).

The current design gets the first guarantee from `ReplaceObject`: the name
`state` always refers to a complete file. Without `ReplaceObject` that cannot
be had. **The state name is absent for a short interval, and the proposal
makes that interval recoverable and visible as PENDING rather than calling it
atomic.**

## Proposed state publication without ReplaceObject

This runs after the decision marker is durable. The journal already holds
`old-state`, `new-state` and a random `archive-id`; that id names this
transaction's scratch files, so every step can be redone from local bytes and
no file is taken to be ours from a generic name.

1. Create `.apkg/state-next.<archive-id>` exclusively with the exact
   `new-state` bytes, flush it and read it back.
2. `Rename(state, state-prev.<archive-id>)`, to an absent name. From here
   `state` is absent.
3. `Rename(state-next.<archive-id>, state)`, to an absent name.
4. Delete `state-prev.<archive-id>` once it is verified equal to `old-state`.
5. Flush `.apkg`.

These are two renames and a delete: a multi-step operation that can be
recovered, not an atomic one.

### Which scratch file is ours

A file belongs to this transaction only when both hold:

- its name carries this transaction's `archive-id`, and
- its bytes are what this transaction writes there: for `state-next.<id>` all
  of `new-state` or a prefix of it (a write cut short, including an empty
  file); for `state-prev.<id>` exactly `old-state`.

Anything else under those two names is foreign: recovery keeps it and the root
is NEEDS_RECOVERY. Files under any other name, such as a bare `state-next` or
another id, are never deleted, renamed or overwritten by this protocol. Today's
engine does write a bare `state-next` and overwrites one that exists
(`execute.c`, `state_publish`); the proposal would replace that, and it is noted
here because it is the same question.

### Recovery

Before the decision the protocol has not started, so nothing of this
transaction can carry these names. A file that does is foreign: preserved,
NEEDS_RECOVERY. Otherwise the journal's existing pre-decision rules apply and
`state` is still old.

After the decision, with ownership checked first as above:

| Observed | Action |
|---|---|
| `state` = new | Delete our `state-next.<id>` and `state-prev.<id>` where present; done |
| `state` = old, no `state-prev.<id>` | Delete our `state-next.<id>` if present, then steps 1 to 5 |
| `state` absent, `state-prev.<id>` = old, `state-next.<id>` = new | Steps 3 to 5 |
| `state` absent, `state-prev.<id>` = old, our `state-next.<id>` cut short or missing | Delete it if present, step 1, then steps 3 to 5 |
| A foreign `state-next.<id>` or `state-prev.<id>`; `state` neither old nor new; `state` absent without our `state-prev.<id>`; `state` = old beside our `state-prev.<id>` | Unresolved: preserve everything, NEEDS_RECOVERY |

A second transaction cannot mistake the first one's files for its own: the id
differs. Leftovers of a finished transaction are therefore never cleaned by a
later one; whether `doctor` should report them is an open point.

### Readers and a second process

The snapshot reader checks the journal before and after reading `state`, and
reads `state` twice. Today a missing `state` is PENDING if the first journal
check saw a journal, and INVALID otherwise. Under this protocol that INVALID is
wrong: a writer can publish its journal and reach step 2 between the reader's
first check and its read.

The first version of this proposal rechecked the journal and then `state`
without a lock. That is not enough: between those two rechecks the first
transaction can finish and archive its journal, and a second transaction can
reach its own step 2. The model finds exactly that schedule, and only with two
transactions.

Proposed rule: when `state` is missing and no journal was seen, the reader
tries the parent lock once without waiting.

- Held: a writer is running; the snapshot is stale and retried (after three
  tries the caller gets BUSY, as today).
- Taken: no writer can move anything while it is held. A journal now is PENDING,
  a `state` now means retry, and only a `state` still missing with no journal is
  INVALID. The lock is then released.

On AROS the parent lock is a public semaphore (`pkg_fs_lock_root`), so trying it
writes nothing to the volume, and a read-only volume is not a problem. The
[journal](journal.md) says that readers never lock; this rule changes that for
one branch and is part of what needs agreement. Readers still never recover.

During the interval readers see PENDING, not the old state. A read-only client,
such as PkgManager browsing, shows "recovery required" until a writer
finishes. If the writer dies in the interval, this lasts until a writer runs
recovery. On AROS a semaphore whose holder died stays held, so until a restart
other writers are BUSY, as they are today after such a death. This availability
difference against `ReplaceObject` systems needs a decision.

## Source profile

The profile, by default `ENVARC:apkg-fusion.source`, is outside any root and
has no journal. It is self-validating: a signed channel observation and the
configuration digest. Today it has no lock, and two PkgManagers saving at once
are not serialized. Proposed:

**Lock.** A public semaphore named after the profile's full path, tried without
waiting, as the root lock is. Held means BUSY. Readers do not take it.

**Intent.** Under the lock, create `<profile>.intent.<id>` exclusively, with a
random `<id>`, the SHA-256 of the current profile (or `absent`) and the SHA-256
of the new one, and flush it. This is the only record of what the update owns.

**Publication.**

1. Create `<profile>.next.<id>` exclusively with the new bytes and flush it.
2. If a profile exists, `Rename(<profile>, <profile>.prev.<id>)`.
3. `Rename(<profile>.next.<id>, <profile>)`.
4. Delete `<profile>.prev.<id>` when its digest is the recorded old one, then
   the intent.

On first creation the intent records `absent` and steps 2 and 4 have nothing to
move or delete.

**Ownership.** `.next.<id>` and `.prev.<id>` are ours only when an intent with
that id exists and their digests are the ones it records. The profile has no
copy of the new bytes outside `.next.<id>`, so a `.next.<id>` that was cut short
cannot be proven ours. It is renamed aside to `<profile>.unresolved.<id>`, not
deleted. An intent that does not parse is renamed aside the same way. Nothing is
deleted by name alone.

**Recovery**, by the next profile writer under the lock or by an explicit
restore, with one intent present (more than one is unresolved):

| Observed | Action |
|---|---|
| Profile = new | Delete `.prev.<id>` if = old, then the intent |
| Profile = old (or absent when old is `absent`), `.next.<id>` = new | Steps 2 to 4 |
| Profile = old (or absent when old is `absent`), `.next.<id>` missing or not new | Set `.next.<id>` aside if present, delete the intent: the save did not happen and is reported as not saved |
| Profile absent, `.prev.<id>` = old, `.next.<id>` = new | Steps 3 and 4: finish |
| Profile absent, `.prev.<id>` = old, `.next.<id>` missing or not new | Rename `.prev.<id>` back to the profile, set `.next.<id>` aside: restored |
| Profile neither old nor new; profile absent without `.prev.<id>` = old when old is not `absent` | Unresolved: preserve everything and say so |

Finishing is preferred to restoring when the new profile is complete: its
observation is never older than the saved one, because a refresh refuses an
older channel (`apkg_channel_news` returns STALE), so finishing never lowers the
replay floor. Restoring keeps the old floor; that is still safe and loses only
the advance.

**Readers** never choose between `.next` and `.prev`. A present profile is read
as today: old or new, each complete. A missing profile beside an intent reports
"source profile update interrupted", not "no profile".

## Browsing without saving the profile

Today a catalogue refresh with a profile set (`common_worker.c`,
`apkg_source_config_refresh`) verifies the channel and then saves the profile.
Where saving is refused, the catalogue is not shown, although nothing was
written to a root. Browsing does not need the save to be safe; it needs the
checks before it. Proposed read-only semantics, when the profile cannot be
saved (no replacement on this volume, a read-only volume, or the profile lock
held):

- The channel is verified against the trusted keys as now.
- It is checked against the saved profile's observation as now: an older
  sequence is STALE and the same sequence with another digest is CONFLICT, and
  both are still refused. The check against the root's snapshot
  (`ak_channel_snapshot_check`) stays as it is.
- The catalogue is shown marked as browsing only, with the reason the profile
  was not saved. The saved observation does not advance.
- Nothing is offered that would write: install, update and remove still go
  through the normal write path, which refuses with its own reason.

The replay protection that remains is the saved floor. What is lost while
browsing only is advancing it: until a save succeeds, a replay of a channel
newer than the saved one but older than the newest seen would be shown. Without
any saved profile, browsing behaves as the no-profile path does today: the
observation is kept for the session only.

## Capability, not version

Today the capability is "the SDK declares `ReplaceObject` and `dos.library` is
at least 50.84". The SDK test is right. The version test is not enough:

- mainline `dos.library` 50.86 and 50.87 pass `>= 50.84` without having
  `ReplaceObject`. A binary built with an SDK that declares it would call
  whatever mainline has at that vector offset;
- a present vector does not show that the handler of the root's volume
  implements the replacement packet.

Proposed: decide per volume at run time, and record the outcome in the
operation result, not in the root.

1. With `ReplaceObject` declared: check that the library vector is the expected
   function. That needs an agreed mainline version or a vector marker that the
   function's presence can be read from. A version number alone does not
   qualify. Then probe once per operation in the root's `.apkg`: create two
   scratch files and replace one with the other. On success use `ReplaceObject`;
   on refusal use the protocol above when the volume qualifies, otherwise
   refuse.
2. Without it, use the protocol above when the volume qualifies.

## Volume requirements

The protocol is only as good as the handler of the volume holding the root.
A volume qualifies when all of these hold, each shown by a test on that
handler rather than inferred from the AROS version or the CPU:

- **No-overwrite rename.** `Rename` to an existing name fails and changes
  neither object, for files and directories.
- **Rename all-or-nothing at process death.** After a kill, the object is under
  exactly one of the two names.
- **Exclusive create.** `O_EXCL` (or `MODE_NEWFILE` with an existence check
  that cannot race) fails on an existing name.
- **Flush.** `fsync` and the directory flush either reach the handler's
  write-back or fail. This is needed for any power-loss statement and is not
  claimed by this proposal.
- **One volume.** The root, `.apkg`, the journal sibling and the history
  siblings are on one volume. Assigns that span volumes refuse.

To be measured on SFS, FFS/OFS, FAT, `RAM:` and any network handler that
distributions put packages on. Until a handler is measured, it does not
qualify.

## Tests proposed for the engine change

`tests/apkg/test_replacement_model.py` models the state protocol over a
DOS-like volume with no-overwrite rename and exclusive create. It is a model,
not the engine. It checks that:

- a writer killed before or after every step, with or without a write cut
  short, recovers to exactly new, and so does a recovery killed part-way;
- over all 192 combinations of `state`, this transaction's `state-next.<id>`
  and `state-prev.<id>` (absent, ours, cut short, foreign), before and after
  the decision, with strays under other names alongside, recovery either
  refuses and changes nothing, or ends in old or new as the decision says;
  every file it changed or removed was ours by the rule above, and the strays
  are untouched;
- the two cases found in review are refused and kept: a foreign
  `state-next.<id>` beside `state` = old, and beside `state-prev.<id>` = old
  with `state` absent; and before the decision any `state-next.<id>` is kept;
- a reader whose every journal check, `state` read and lock attempt is its own
  event, over every schedule across two consecutive transactions, never reports
  INVALID, reports PENDING only when a journal existed while it read, and
  otherwise returns a state the root had while it read; a quiet root is read
  without a retry;
- the reader of today, and the unlocked recheck of the first version of this
  proposal, both report INVALID; the recheck only with the second transaction.

The source-profile protocol is not modelled yet. The engine change would carry
these cases as fault points next to the existing 1 to 7, plus a DOS run on each
qualifying handler: kill at each step, recover, then verify on mainline v1,
ABIv11 and the Pi.

## Open points for agreement

1. Whether to adopt the protocol above, keep the refusal until `ReplaceObject`
   is in mainline and ABIv11, or both: the protocol now, `ReplaceObject` where
   present.
2. Whether PENDING during the interval, rather than the old state, is acceptable
   for read-only clients.
3. Which handlers qualify for a first release, and the evidence each needs.
4. The vector check: proposing `ReplaceObject` to mainline AROS with a version
   that marks it, and to the ABIv11 line.
5. The reader trying the parent lock in one branch, which changes "readers
   never lock" in the journal.
6. The source-profile lock, intent file and recovery messages.
7. Browsing only when the profile cannot be saved, and the replay window it
   leaves.
