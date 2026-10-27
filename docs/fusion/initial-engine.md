<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Initial common engine implementation

This records the first CONTEXT → SIGNED-READ → PLAN-INSTALL → JOURNAL →
INSTALL → GUI-INSTALL path. It is a development subset, not a merged release.
The subsequent [operations profile](operations.md) and
[streaming payload transport](payload.md) extend this initial subset.
[Architecture](architecture.md), [data model](data-model.md) and
[acceptance](acceptance.md) retain the broader product obligations.

The context/snapshot/plan structure follows the selected arospkg skeleton;
`src/apkg/` owns the new common implementation. Existing pkg filesystem,
SHA-256, Ed25519 and target/application inspection services are linked directly,
without the global `pkg_lib_*` request backend. `PKG_FS_LOCAL_ONLY` excludes
the inherited filesystem service’s network/global request section from this
local library; the regression baseline keeps its existing service profile. PkgManager keeps its existing
DOS Process worker, message port, confirmation window and UI loop.

## Build and use

`make` builds common `build/apkg` and `build/libapkg.a`. `build/apkg-core` is a
byte-identical development alias; inherited/provisional targets are explicit
regression baselines, excluded from default installation. Public headers are `include/apkg.h` and
`include/apkg_client.h`; the contract header path forwards to the real header.
The common library and CLI contain no test fault-injection entry point.

Commands:

```text
apkg-core inspect ROOT
apkg-core init ROOT TARGET POLICY --yes
apkg-core install ROOT PACKAGE-DIRECTORY
apkg-core install ROOT PACKAGE-DIRECTORY --confirm PLAN-DIGEST
apkg-core recover ROOT
```

Initialization displays its plan and, in a terminal, retains that exact plan
while asking for its digest. A nonterminal invocation only previews. `--yes`
explicitly authorizes the displayed initial target/seed grants in that
invocation for automation. Random root ids are never reconstructed from a
cross-process confirmation token. The public C API also retains exact plans.
Unconfirmed package installation leaves installed state unchanged; explicit
archive preparation may retain verified private cache content. Installation
previews can be repeated with their displayed digest while inputs are unchanged.
No command silently grants trust or recovers at open. Inspect creates no files.
Root parents must exist; automatic creation of missing ancestor directories is
not supported by this first profile.

A local signed package folder contains `descriptor.json` and `signature.json`.
The new descriptor is not an existing pkg manifest, archive or channel index;
no compatibility conversion is guessed. Authoring/channel adapters remain
AUTHORING/QUALIFY work. The tests generate genuine Ed25519-signed synthetic
packages through the same existing cryptographic implementation.

## Implemented schema subset

All objects use the canonical framing and detached signature transcript in
[serialization](serialization.md). This profile accepts no required extensions;
unknown execution metadata is refused. It does not discard unsupported fields.

| Object | `body` fields |
|---|---|
| Root | `generation`, `installed`, `policy`, `root_id`, `target`; optional `channel_heads` and `package_generation` from the [replay profile](channels.md#journaled-replay-floors) |
| Policy | `authorities`, each with `id` and sorted Ed25519 `keys` |
| Descriptor | `authority`, `files`, `kind`, `name`, `revision`, `target`, `version` |
| Installed entry | `descriptor` and `signature` objects, retained byte-for-byte in canonical form; local optional `reason` and [adoption evidence](adoption.md) remain separate from signed content |
| Plan | `action`, `anchor_dev`, `anchor_ino`, `before`, `generation`, `new_state`, `root_path`, `target_path` |

Root state is `.apkg/state`. Generation starts at 1. Installed entries preserve
commit order. Dependency resolution and [adoption](adoption.md) retain explicit,
dependency, recommendation or unrecorded installation reasons. Packaging revisions
and common dependency floors are unsigned and may be zero; the root generation
starts at one independently.
Legacy `db/`/`.pkg/` roots are inspectable as LEGACY and require migration.
Legacy transaction entries report PENDING and refuse common recovery until
validated migration exists. Coexisting common/legacy databases report INVALID,
preventing two writers from being treated as one authoritative root.
Common snapshots own immutable record/view storage and survive context close.
Missing filesystem object identity refuses mutation. On POSIX hosts the
identity is the device and inode. AROS posixc derives its inode from a hash of
the path and its device from a DOS list address, so neither survives a rename
or a reboot; there the identity is the object key `Examine()` reports, which a
rename within the volume keeps, with the volume's name and creation date. A
handler that reports no object key gives no identity, so mutation on it is
refused rather than guessed. Live filesystem bindings are checked independently from root ids/generations;
there is no global clone registry or automatic clone-id reassignment in this
profile. Cross-root plan/approval transfer is refused.

Descriptor files are sorted by exact relative path. A regular entry has exactly
`data` (lowercase hex bytes), `mode` (originally decimal `420` or `493`;
expanded by the [host payload profile](payload.md#host-semantic-metadata-profile)),
`path`, `sha256`, `size` (uint64 decimal string), `type` (`file`). A directory has
exactly `path` and `type` (`directory`). All parents must be explicit directory
entries. Hidden files and empty directories are preserved. Case-fold collisions,
links, traversal and unrepresented metadata are refused. ELF/Hunk CPU evidence
cannot contradict the declared target. It does not establish ABI/runtime testing.

This deliberately bounded initial transport keeps payload bytes in the signed
descriptor and retained records. It is not a replacement for large-package
container/archive authoring. Package kinds accepted here are data, tool and
application; dependency/recommendation/provider changes, system placement,
image semantics and richer attributes were outside this initial tranche.
Later operation and ownership work, including [existing-bundle adoption](adoption.md),
is tracked in the implementation status.
The old bridge restrictions are not removed by this implementation.

The parser enforces canonical encoding, Unicode scalar validity, sorted unique
keys, uint64 overflow checks, depth 32 and 100,000 tokens. This backend additionally
limits keys to 255 bytes, payload paths to printable ASCII/4095 bytes and 32
components. Default object/working-memory budgets remain 16/64 MiB, with an
initial conservative per-envelope reserve of 32 times serialized size during
materialization. Thus a default-context operation can reach LIMIT below the
nominal object ceiling. Larger explicit working budgets can raise that bound,
up to the 16 MiB framing ceiling; memory budgets below 1 MiB are refused.
No field is truncated to satisfy a budget.

## Transactions and results

The [journal protocol](journal.md) is implemented for initialization and a fresh
single-package install. The nonblocking physical-parent lock is intentionally
coarser than per-root locking. It also serializes first-root initialization.
Exact state/policy, destination absence and physical bindings are rechecked,
including after progress callbacks. Publication refuses to overwrite a foreign
package directory. Approval is bound to its context session and is consumed
on an execution attempt; execution does not replan.

Committed and restored journals are archived by an atomic rename to a sibling
history directory. Approved descriptors, signatures, before/after states and
restored payload remain local, so recovery needs no network. Changed published
files, foreign entries, corrupt intent and unexpected state retain evidence and
report NEEDS_RECOVERY. New transactions prepare privately and publish only a
complete ready journal. A killed preparation can leave a private orphan without
blocking the root or explicit retry. Preexisting incomplete active journals
remain refused and preserved. This profile garbage-collects neither orphaned
preparation nor retained history; host process tests do not establish power-loss
durability.

Cancellation before mutation reports UNCHANGED; after publication it can report
RESTORED. A request at commit can finish COMMITTED while still recording that
cancellation was requested. Error codes and filesystem outcomes are separate.
The result is allocated before managed writes, and a final UI message is
allocated before its worker starts. Process-death tests are not power-loss tests.
Uncooperative filesystem writers remain a platform/concurrency boundary; these
checks do not claim to freeze arbitrary external processes' file handles.

Atomic existing-state replacement is a required capability. AROS needs both an
SDK exposing `ReplaceObject` and dos.library >= 50.84. Unsupported SDK/runtime
combinations refuse the signed install plan before publication. Handler flush
and file/directory sync errors are reported; no delete-then-rename fallback is
used. Self-update and cross-volume participants remain disabled until their
operation-specific protocol and legacy import are implemented.

## PkgManager path

`APKG_FUSION=1` in the imported ABIv11 GUI build enables an advanced operation
panel for local install/remove/repair/upgrade/rollback/resolve/recovery and exact
file/HTTP channel selection, through the **Review operation** button. The worker verifies, plans and
sends a complete owned presentation with root, target, signer, exact plan digest,
all destinations, file sizes/modes/hashes and independent qualification status.
It retains the context/plan while the window asks for confirmation. Confirm,
decline, cancellation and close requests return to that same worker; no plan is
reconstructed after approval. Progress messages copy full operation identities,
64-bit event sequences/counters and phase. Final
results distinguish refusal, cancellation, success and required recovery.

Legacy mutations are disabled in this fusion development UI, preventing the
old worker from mutating a common root through a second engine. The
non-fusion build retains its inherited workflows. General catalogue browsing,
settings and the remaining operation families still require common-API wiring.
The [channel client checkpoint](channels.md#shared-dependency-aware-client-workflow)
adds shared complete discovery facts and dependency-aware worker installation;
interactive native catalogue selection remains open.
Initialize a separate common root explicitly before using this first GUI path.

For an aarch64 hosted cross-build, configure `APKG_AROS_SDK` and
`APKG_AROS_TOOLS`, then run `APKG_AROS_GUI=1 sh tools/build-apkg-aros.sh`.
`APKG_AROS_RUNTIME=native` builds for native AROS (no x18 reservation); a
native SDK keeps `collect-aros` with its build's host tools, named by
`APKG_AROS_HOST_TOOLS`.
It writes only under this checkout's build directory. It does not launch or
modify an operating system. `tools/check-apkg-aros.sh` provides a separate
header/type check with a configured SDK. Cross-builds prove compilation/linking,
not a GUI/runtime acceptance pass on either distribution.

## Test and review evidence

Run `make test-apkg` for the real common engine and actual PkgManager bridge
with host DOS message adapters. The adapters replace OS message primitives,
not verification/planning/mutation. Cases cover independent snapshots, absent
and inaccessible roots, malformed objects, trust/target/signature refusals,
exact plans, foreign destinations, stale state and approval reuse, hidden/empty
payloads, I/O failures, process termination, interrupted recovery, changed live
files, parent-lock contention, cancellation and late cancellation. Test-only
nth-allocation injection covers 293 planner/executor/reader/presentation
positions, requiring typed failures and consistent recoverable outcomes. CLI/worker
outputs are compared for identical installed descriptor/state semantics.

`sh tests/apkg/sanitize.sh` runs actual common-library lifetime/event-copy
checks and CLI/worker client scenarios under ASan/UBSan. The normal fusion CI
runs both suites, plus the inherited tests. Fault controls exist only in the
test shared library. Test-only DOS adapters are never product dependencies.
The [status](status.md) records measured results and the review gate for each lot.

The SDK check can also audit optimized PkgManager window/worker objects by
setting `AROS_NM` to the target `llvm-nm`. It refuses references to inherited
mutation and self-update writers. The host `build/test_pm_dispatch` compiles the
actual dispatcher without linking that engine, and verifies job routing and
completion with DOS/Exec adapters. Neither check establishes native execution.
