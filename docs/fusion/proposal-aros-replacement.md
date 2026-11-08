<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Proposal: state replacement on AROS systems without ReplaceObject

This is a proposal for review. It is not an agreed contract and does not change
the [journal](journal.md) until both maintainers accept it. The engine is
unchanged on this branch: it still refuses mutation where
`pkg_fs_atomic_replace_supported()` is false. With this branch the refusal says
why.

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
`old-state` and `new-state`, so every step can be redone from local bytes.

1. Write `.apkg/state-next` with the exact `new-state` bytes and flush it. This
   is what happens today.
2. `Rename(state, state-prev)`, to an absent name. From here `state` is absent.
3. `Rename(state-next, state)`, to an absent name.
4. Delete `state-prev` once it is verified equal to `old-state`.
5. Flush `.apkg`.

These are two renames and a delete: a multi-step operation that can be
recovered, not an atomic one.

Recovery, with the decision present (otherwise the journal's existing
pre-decision rules apply and `state` is still old):

| Observed | Action |
|---|---|
| `state` = new | Delete `state-next` and `state-prev` if each equals new or old respectively; done |
| `state` = old, `state-prev` absent | Steps 1 to 5 again |
| `state` absent, `state-prev` = old, `state-next` = new | Steps 3 to 5 |
| `state` absent, `state-prev` = old, `state-next` missing or not new | Write `state-next` from the journal, then steps 3 to 5 |
| `state` absent and `state-prev` not old, or any file foreign | Unresolved: preserve everything, NEEDS_RECOVERY |

Before the decision nothing in this table runs: `state` is untouched.

### Readers and a second process

The snapshot reader already checks the journal before and after reading
`state`, and reads `state` twice. One change is needed. When `state` is
missing, the reader currently reports PENDING only if the first journal check
saw a journal, and INVALID otherwise. A writer can publish its journal and reach
step 2 between the reader's first check and its read, and it can even finish
and archive the journal before the reader looks again. So a missing `state` has
to be checked again together with the journal: a journal now means PENDING, a
`state` now means the snapshot is stale and is retried, and only a `state` still
missing with no journal at either check is INVALID.

During the interval readers see PENDING, not the old state. A read-only client,
such as PkgManager browsing, shows "recovery required" until a writer
finishes. If the writer dies in the interval, this lasts until a writer runs
recovery, because readers never recover. This availability difference against
`ReplaceObject` systems needs a decision.

Writers serialize on the parent lock as today. Recovery takes the same lock.

## Source profile

The profile is outside any root and has no journal. Proposed, for the same
handlers:

1. Write `<profile>.next` exclusively. Its contents are already self-validating
   (a signed channel observation and the configuration digest).
2. `Rename(<profile>, <profile>.prev)`, then `Rename(<profile>.next, <profile>)`,
   then delete `.prev`.
3. A reader that finds the profile missing but a valid `.next` or `.prev` uses
   neither silently. It reports "source profile update interrupted". The next
   save, or an explicit restore, finishes the update from a valid `.next`, or
   restores `.prev` when `.next` is missing or invalid.

Profile writers should take a lock, so that two PkgManagers saving at once
cannot interleave steps 2 and 3. Today the profile has no lock.

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

`tests/apkg/test_replacement_model.py` models the protocol over a DOS-like
filesystem with no-overwrite rename. It kills the writer before and after every
step, and also kills recovery part-way, then checks that:

- recovery ends in exactly new after the decision, and old before it;
- a reader running at every interleaving gets old, new, PENDING or a stale
  snapshot to retry, never INVALID;
- the reader rule of today, which checks the journal once, does report INVALID
  at step 2. Rechecking only the journal is not enough either, because the
  writer can finish before the recheck. This is the regression the reader
  change prevents;
- foreign bytes in `state-prev` or `state-next` are refused and kept.

It is a model, not the engine. The engine change would carry the same cases as
fault points next to the existing 1 to 7, plus a DOS run on each qualifying
handler: kill at each step, recover, then verify on mainline v1, ABIv11 and the
Pi.

## Open points for agreement

1. Whether to adopt the protocol above, keep the refusal until `ReplaceObject`
   is in mainline and ABIv11, or both: the protocol now, `ReplaceObject` where
   present.
2. Whether PENDING during the interval, rather than the old state, is acceptable
   for read-only clients.
3. Which handlers qualify for a first release, and the evidence each needs.
4. The vector check: proposing `ReplaceObject` to mainline AROS with a version
   that marks it, and to the ABIv11 line.
5. The source-profile lock and its recovery messages.
