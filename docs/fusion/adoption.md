<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Existing bundle adoption

Adoption registers an existing unmanaged bundle through the common context,
immutable plan and transaction journal. It preserves the entire observed tree,
including hidden files, empty directories, modes and supported AROS attributes.
It requires an initialized common root, a supplied trusted signed artifact and
an explicit ownership-evidence object. It grants no publisher key or review.

```text
apkg adopt ROOT ARTIFACT --evidence FILE
apkg adopt ROOT ARTIFACT --evidence FILE --confirm APPROVED-DIGEST
apkg info ROOT PACKAGE adoption
```

The CLI and PkgManager worker call `apkg_plan_adopt` through the same client
entry point. The desktop action uses the signed-package input for the artifact
and the auxiliary input for the evidence file. Preview includes exact local
ownership evidence and distinguishes the supplied artifact's valid signature
from unknown authentication of the original installation.

## Evidence and ownership

The canonical `apkg-json-1` envelope has type `adoption`, schema `1`, empty
`requires`, and a body with exactly these fields:

| Field | Meaning |
|---|---|
| `source` | `manual`, `arospkg-v1` or `pkg-v1`; descriptive provenance, never authentication |
| `record` | Original record bytes encoded as lowercase hex; inert retained evidence |
| `reason` | Recorded `explicit`, `dependency`, or `unrecorded`; unknown is not inferred |
| `baseline` | Sorted unique file objects containing `path`, `sha256`, and decimal-string `size` |

Every recorded file baseline must match the supplied signed artifact's file hash
and size. Paths obey common payload constraints. Signature and current author
trust authenticate the supplied artifact; they do not retrospectively validate
the legacy record or establish its original publisher identity. A migration
adapter must separately validate the legacy record, identity/version/revision,
target uncertainty and explicit artifact mapping before requesting adoption.

The local installed entry retains an `adoption` object containing this envelope
as `evidence` plus sorted `unowned` and `preserve` path arrays. Publisher
manifests cannot add these local fields. Files absent from the recorded baseline
remain unowned even if their current bytes equal the artifact. Directories are
unowned because this baseline format records files only. Owned files whose live
bytes, type, mode or supported AROS attributes differ from the signed artifact
are protected as personal modifications. Missing recorded files remain missing
at adoption; an explicit repair may restore the supplied signed baseline.

Repair, direct upgrades, dependency-resolved upgrades and removal retain these
ownership guards. They preserve existing protected paths and refuse conflicting
type changes. Removal preserves the unowned bundle drawer, including an empty drawer, and
can leave personal files within it.
New paths delivered by an approved later upgrade follow ordinary ownership
rules. Adoption never resets delivered hashes to the live edited bytes or signs
those bytes as if they came from the original publisher.

## Transaction and migration boundary

Preparation compares the tree used to classify ownership with the immutable
plan's before-tree. Execution rechecks physical/root/snapshot and live-tree
preconditions under the common lock. The journal stages the complete preserved
tree, records the same commit decision as other operations, and recovers
repeatedly without remote access. Rolling back adoption removes management and
restores the retained unmanaged tree while preserving later personal edits.

This primitive does not convert a legacy root in place. Common contexts continue
to refuse mixed or pending legacy state. The [migration mapping](migration.md)
requires a validated importer, original-preserving copy, history/settings/launch
handling and compatibility rehearsals. Native runtime and power-loss behavior
require target evidence. Implementation checks are recorded in [status](status.md).
