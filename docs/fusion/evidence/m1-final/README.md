<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# M1 RAM application lifecycle qualification

[Final candidate](final-candidate.json) records source `f73eec7`, its additional
fresh-root verification-report/GUI-launch trial and binary identities. The
[host CI receipt](host-ci.json) records the final source's successful Linux/macOS
integration, memory, inherited-suite, symbol/documentation and portal baseline
checks. The
[base lifecycle verdict](verdict.json) records `08e6cf8` and the corrected
confirmation rendering. The CLI is byte-identical across these revisions;
the final GUI additionally retains verification reports in ARexx jobs. Synthetic
application versions 1.0 and 2.0 wrap the same real Calculator executable; these
are package lifecycle fixtures, not upstream Calculator releases.

The run exercises signed installation, seven-element verification, actual managed
launch and interaction, moved unregistered launch, upgrade with edited settings
and personal data, offline rollback and repair, PkgManager/ARexx queries and
confirmed operations, source observations and offline profile restoration,
removal and independent registry/file/process inspection. Output records are
listed with SHA-256 digests in the verdict. [Final state](final-root.json) keeps
an empty installation set and replay sequence 2 at generation 9. Removal leaves
exactly the edited configuration and personal file, plus their parent drawers.
Retained history/cache content has a separate lifecycle.

The preceding GUI failed repeated same-size confirmation: its rollback plan was
blank and was declined. The corrected GUI displays the first rollback plan,
the declined/reopened plan at the same size, and the later distinct removal
plan. Native scanout witness hashes are retained with the verdict. A successful
job return alone was not used to accept application launch or plan visibility.

The helper sends genuine ARexx messages directly to PkgManager's port; this image
does not contain an RX interpreter. This qualifies the message interface, not
an interpreter installation or all historical scripts. INFO and SHOW correctly
refuse the ambiguous catalogue-only name after removal; the catalogue contains
two versions. Worker completion is checked separately from the independent task
inventory after client exit.

The final-source follow-up compares independent CLI and ARexx verification with
changed, missing and unknown paths. A second job has different counts; the first
job retains its original report after worker completion and replacement of the
window's report buffer. Replay the independent regression check:

ARexx job messages retain a bounded 479-byte prefix; the complete report remains
available in the window and CLI. The regression compares that prefix and the
fixture's verification counts, rather than claiming an unlimited job field.

```sh
python3 tests/aros/verify_job_report.py \
  --first-cli docs/fusion/evidence/m1-final/follow-up-verify-cli-first.txt \
  --first-job docs/fusion/evidence/m1-final/follow-up-verify-job-first.txt \
  --second-cli docs/fusion/evidence/m1-final/follow-up-verify-cli-second.txt \
  --second-job docs/fusion/evidence/m1-final/follow-up-verify-job-second.txt \
  --retained-job docs/fusion/evidence/m1-final/follow-up-verify-job-retained.txt
```

That follow-up launches the real managed application through PkgManager/ARexx,
observes interaction, closes the client/application and removes the package.
The icon manually restored for launching has changed DOS permissions: removal
preserves it as locally modified, as its preview declares. The test then
explicitly deletes this synthetic icon and independently inspects the remaining
configuration and personal note. These two inspection stages are separate
records; manual cleanup is not attributed to the package-removal transaction.

This is application evidence on a physical M1 under a qualified hypervisor and
independent debugger, in isolated RAM. The retained runtime/SDK has development
C-runtime differences. It is not bare-metal OS acceptance or OS promotion.
The guest reports failed secondary Exec startup and runs the application
workload on one active AROS core. The eight-CPU supervisor exit is not an AROS
SMP acceptance claim.
An earlier newer-runtime attempt failed before application execution; its
diagnostics were retained separately. The final supervisor exit accounts for
all eight CPUs and a fresh proxy identity observation. Installed libpkg, startup,
updater and persistent storage are unchanged.

Broader platform/filesystem/extended metadata, native HTTP/TLS, interrupted
native recovery and power-loss durability, native self-update and the complete
desktop interaction matrix remain separate release qualifications. Deferred
[legacy support](../../legacy-support.md) and portal work are not completed by
this trial. Private target-control tooling and machine identifiers are excluded
from these public synthetic records; they are observations, not hardware
attestation or qualification of third-party publications.
