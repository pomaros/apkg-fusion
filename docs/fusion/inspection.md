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
with explicit `unknown` for unestablished archive size/runtime
requirements/upgrade availability/rollback applicability. `requirements` is
`not-checked`. Installed version/revision describe the committed record. Added
fields expose author, target, reason, signature/trust, declared payload counts,
full relations, independent qualifications and snapshot provenance.

[Signed descriptive metadata](presentation.md) supplies summary, category,
license and source repository when recorded. Missing fields remain `unknown`;
recorded empty fields remain empty. They are author text, not qualifications.

PkgManager installed details and fusion ARexx INFO share that inspection.
Fusion advertises **ARexx interface 2**: INFO reports explicit unknowns and
`target-match`/`target-mismatch`, without inferring runtime compatibility or
fabricating upgrade/rollback booleans. INFO prefers installed records; when
there is no installed record, it revalidates the loaded signed observation
against current policy and replay floors, then requires a unique matching
catalogue build. Multiple versions, targets or authorities refuse with CONFLICT.
No network fallback is performed.
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
ROLLBACK uses the common retained-history plan and confirmation flow. Dated
publication and last-refresh views use the common [news queries](publication-news.md).

## Catalogue information

`apkg_channel_info(CHANNEL, INDEX, FIELD, ...)` formats one immutable channel
entry with the fields advertised by `apkg_info_fields`. Its returned text owns
its bytes independently of the channel. Field names are case-insensitive;
absent or empty FIELD emits name/value lines. Invalid fields and out-of-range
indices refuse without a partial output.

Catalogue-only INFO reports `state available`, with installed version/revision
`unknown` and reason/adoption `not-applicable`. The channel formatter has no
installed-state knowledge, including when the same build happens to be installed.
Root state, root identity,
generation and installed snapshot are `unknown`; no installed record is
fabricated. Archive `size` is the channel's declared archive byte count. Payload
bytes, files, directories, relations and presentation come from the signed
descriptor. Missing presentation fields remain unknown and recorded empty
fields remain empty. Signature, captured author trust and root-target match
remain independent facts. Runtime/review are `not-asserted`, and payload/system
requirements are `not-checked`; supplied qualifications have their own
[observation view](attestations.md).

Fusion ARexx INFO caps the complete reply at its result boundary and refuses
overflow rather than truncating it. An installed record retains precedence
even when the catalogue advertises newer versions. An unavailable or invalid
catalogue cannot replace a failed installed query with an invented success.

Fusion `REQUIREMENTS name` reads the retained signed catalogue offline and
revalidates current channel/author trust, target and accepted replay floors.
Among trusted builds for the exact name and current target, it selects the
newest version/revision; different authors, incomparable versions or differing
equal builds refuse. Identical opaque versions can still be ordered by revision.
The report names the selected author, version and revision and probes its signed
system requirements with the same implementation used by install planning.
Missing requirements are reported facts, not a failed query. No declared
requirements produces an explicit empty-declaration message; a host that cannot
probe AROS reports undetermined. This is a current machine observation, not an
installation plan or a runtime qualification. No archives or root writes occur.

The CLI equivalent discovers a file/HTTP channel before querying:

```
apkg requirements ROOT ENDPOINT CHANNEL-ID NAME
```

`apkg_channel_requirements` shares selection/reporting across the two clients.
`apkg_package_requirements` queries an already validated immutable package
handle; it does not independently revalidate root policy. Owned reports use
`apkg_client_text_free`. Installed INFO retains its snapshot-only
`requirements not-checked` meaning rather than silently probing the OS.

Fusion `VERIFY name`, the Verify button and `apkg verify ROOT NAME` now share
`apkg_client_verify`. It authenticates the installed descriptor, inventories the
live package drawer using streamed hashes, and compares declared path types,
permissions, content size/digest and Amiga metadata. The report counts `ok`,
`changed`, `missing` and `unknown` paths; configuration paths are labelled.
Personal files are unknown rather than adopted or removed. A missing drawer
reports missing declarations. Withdrawn author trust remains a distinct fact;
a valid historical signature still provides the comparison baseline.

Query success means the report completed; changed or missing files are report
facts and do not trigger repair. Invalid signatures, links, special files,
unsupported metadata, pending roots, allocation failures and replies over
65,535 bytes refuse without partial output. A changed committed snapshot during
the scan refuses as stale. The live scan is an observation, not an atomic
snapshot against arbitrary concurrent external writers. Declared paths are
checked; no runtime or root-directory metadata qualification is implied.
Cancellation is checked before/after inventory and between comparisons; the
current inventory helper does not interrupt a file hash. Reports are owned and
use `apkg_client_text_free`; worker text is copied for native process lifetime.

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
