<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Installed inspection

`apkg_snapshot_inspect` owns an immutable copy of one installed record,
independent of snapshot/context lifetime. Its view retains exact descriptor,
signature and relation bytes; complete names/versions and unsigned revisions;
declared payload counts; and captured root/generation/state/snapshot identity.
Free it with `apkg_inspection_free`. Borrowed views expire with that object.

READY and PENDING common snapshots can be inspected without recovery. Absent,
legacy and invalid roots refuse without creating state. Out-of-range rows
return NOT_FOUND. Allocating outputs clear on entry and failure.

The reader checks the descriptor signature against exact captured bytes and
checks author grants against captured policy. A valid signature with a withdrawn
grant reports `rejected`; an invalid signature reports `invalid` with author
trust `unknown`. Diagnostic metadata grants no permission to install or run it.
Target comparison uses the root's declaration, independently of the running OS.
Review and runtime qualifications remain `not-asserted`.

It fetches no archives and checks or repairs no installed payload. Signature
verification cannot qualify modified/missing live files: `payloadverification`
is `not-checked`. Declared file bytes and explicit directories are counted;
archive size remains `unknown`. Byte-sum overflow returns LIMIT. All four
relation sets retain authority, target, version and revision constraints.
Absent relations in a common schema-1 descriptor mean empty sets; legacy
missing facts still need the separate import policy.

`apkg_client_info(ROOT, NAME, FIELD, ...)` performs read-only exact-name lookup
and returns owned text. Empty FIELD emits name/value lines; a case-insensitive
field emits its value. Release text with `apkg_client_text_free`.

```sh
./build/apkg info ROOT NAME
./build/apkg info ROOT NAME signature
./build/apkg info ROOT NAME relations
```

`apkg_info_fields` advertises fields. Existing information field names remain,
with explicit `unknown` for unestablished summary/category/archive size/runtime
requirements/upgrade availability/rollback applicability. `requirements` is
`not-checked`. Installed version/revision describe the committed record. Added
fields expose author, target, reason, signature/trust, declared payload counts,
full relations, independent qualifications and snapshot provenance.

PkgManager installed details and fusion ARexx INFO share that inspection.
Fusion advertises **ARexx interface 2**: INFO reports explicit unknowns and
`target-match`/`target-mismatch`, without inferring runtime compatibility or
fabricating upgrade/rollback booleans. INFO currently describes installed
records; catalogue-only INFO and remaining queries need common adapters.
The imported non-fusion client retains interface 1 and its original corpus.

Fusion `LIST [MATCH text]` now reads the retained signed catalogue after
revalidation against current root policy, target and replay floors. It returns
current-target names, collapses multiple builds of the same author's package,
and refuses a name shared by different authors. MATCH searches the complete
name. An untrusted author's name can be discovered; listing grants no install
permission. There is no network fallback and no root mutation. `LIST INSTALLED`
continues to use the installed-record view.

ARexx `INSTALL name` and `UPGRADE name` use the root's ordered source list,
resolve through the common engine and wait for plan confirmation in the window.
UPGRADE requires an installed name and keeps its `upgrade` job label. Source
setup uses the [root source controls](sources.md#root-source-list-controls);
loading a catalogue with UPDATE alone does not add it as an installation source.
Job numbers, WAIT and cancellation keep their existing asynchronous protocol.
Rollback/verification command adapters and live REQUIREMENTS queries remain
separate outstanding work, as do catalogue-only INFO and dated news.

Fusion JOB/WAIT now project the common progress events: `phase` is check,
fetch, stage, apply, commit or recover; `done`/`total` retain 64-bit counters.
An unmeasured total is `unknown`, including before the first event; known zero
is `0`. Counters describe the current phase, not an accumulated percentage.
`cancancel` follows the last engine boundary. During plan approval the job is
`confirm`, and CANCEL declines it; approval resumes the same worker-held plan.
A declined plan ends as `declined`, a cancellation as `cancelled`, and recovery
required as `failed`. A late event cannot revive a completed job. These changes
belong to fusion's interface 2; the imported interface 1 format stays unchanged.

ARexx refuses answers over 65,535 bytes with LIMIT/TOOLONG and frees them.
Callers can request smaller fields. CLI/library formatting uses the bounded
common metadata limit. Neither path truncates identities or relations.

[Status](status.md) records host/type evidence. Native ARexx/UI interaction and
script compatibility still need the scheduled M1 rehearsal and further tests.
