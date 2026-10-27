<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common architecture

The product combines native package browsing and launching, scriptable system
management, portable publishing, open author channels and a curated catalogue.
The architecture follows the behavior it must guarantee. Source layout and
original authorship confer no architectural priority. Implementation evidence
is recorded in [status](status.md).

## Implementation skeleton

The engineering direction is to extend arospkg's context-based libpkg as the
product skeleton and port pkg's portable filesystem, signing, ownership,
system-package and publishing services into it as modules. This preserves the
client-facing structure already used by PkgManager. Its journal still requires
the durability and recovery work specified below.

The root pkg backend and development CLI are a baseline and service testbed.
BASE may correct their integration regressions. Further product features belong
to the common context skeleton. The root layout does not turn the provisional
global-operation backend into the final engine.

This is the project's engineering direction, not a claim of agreement by both
upstream maintainers. Development proceeds without waiting for that agreement
or missing distribution information. Review and alignment are tracked in the
[maintainer handoff](maintainer-agreement.md); resulting changes remain possible.

The [common data model](data-model.md) defines logical identities, preservation
rules and plan inputs. The [API contract](api.md) and
[serialization framing](serialization.md) define the first implementation
tranche. Their implementation status is tracked separately
from the imported source layout.

The [initial common engine](initial-engine.md) now implements the first signed
install path; the full acceptance obligations below remain in force.

## One engine, explicit contexts

CLI, PkgManager, installers and embedded consumers share one library. A context
owns a target root, CPU/OS/ABI/runtime identity, channels, trust policy and
operation state. Read-only contexts observe without creating files or recovering
transactions. Mutation contexts lock the root before examining mutable
preconditions or performing recovery.

The API draws on arospkg's explicit contexts, typed client results and recovery
model. Portable filesystems, signatures, ownership, shared system destinations,
diagnostics and publishing draw on pkg. Those services operate through the
context; the default CLI/library now exclude the provisional global request backend. Wrapping two
independent mutation engines does not satisfy this contract.

PkgManager retains its native UI and DOS worker process. The library returns
typed facts and events; each client presents them. Events identify operations
and phases, carry 64-bit counters and expose cancellation boundaries and
recovery outcomes. UI message queues own copies of event data. Completion
reflects the final outcome after required recording and durability steps.
The [shared channel workflow](channels.md#shared-dependency-aware-client-workflow)
resolves metadata before fetching, prepares only selected archives and commits
package changes plus replay floor in one approved transaction. CLI and the
actual PkgManager worker call the same client entry point; confirmation never
starts another resolution.

## Plan, approve, execute, recover

A plan identifies exact versions, packaging revisions, targets, content digests,
signers, providers, destinations and authorized modifications. It exposes
replacements, takeovers, system writes and changes to trust. Installing an
application cannot implicitly authorize unrelated system modifications.

Execution uses that plan under lock. Changed inputs or preconditions invalidate
it and require a fresh plan. Re-resolving silently after confirmation is
forbidden. Package-name/version lists alone are insufficient plan identities.
File/HTTP selections bind their source facts into canonical plan bytes, including
selected dependency origins. The [source-bound plan contract](channels.md#source-bound-approved-plans)
requires rechecking before the commit decision; recovery and retained offline
operations never require remote revalidation after that decision.
Explicit channel acceptance records per-root sequence/digest floors in the same
journal. Catalogue-only generations preserve a separate package revision, so
package rollback remains applicable and cannot lower the latest replay floors.

Transaction preparation stays private until its complete ready journal is
published without overwrite. A killed private preparation cannot create an
unrecoverable active coordinator or block the unchanged root. Private cleanup
must preserve replaced drafts and unrelated evidence.

The journal records intent before mutation and distinguishes the commit decision
from observations of completed actions. Recovery examines both the journal and
actual filesystem state. Interrupted recovery is repeatable; unresolved state
remains available for diagnosis. Atomic replacement, write ordering and
durability belong to the filesystem contract. Closing and rereading a file
establishes readability, not power-loss durability.

Cancellation, process termination, write failure and power loss have separate
acceptance cases. Successful installation, recovered failure and partial
operation have distinct results. Offline rollback uses retained local content
and records; a channel entry cannot recreate a missing archive.

## Targets, versions and relationships

CPU, OS, ABI and runtime are explicit fields, independent of the publishing
host. Canonical `CPU-aros-ABI-runtime` text fits the channel's target field.
Executable inspection verifies CPU only; it cannot infer ABI, runtime
compatibility or hardware qualification.

Version ordering preserves upstream versions and packaging revisions, including
APKG's prerelease and rebuild semantics. Indeterminate comparisons require an
explicit decision. Package identity, capability identity and channel ownership
remain separate concepts.

Resolution prefers a suitable installed provider before proposing another.
Plans identify provider changes. Package and capability conflicts remain
distinguishable; replacement does not imply provision. Recommendations remain
optional. A shared-library dependency is satisfied only by a compatible library
the target application's loader can actually use.

## Applications and systems

An autonomous bundle carries `application.json`. Its launch paths are relative
to the bundle, and launching requires no registry entry. Managed updates,
adoption and movement add management without making it a prerequisite for
execution. Launching does not implicitly install missing system components.

System packages declare shared destinations and ownership. The engine serves
fresh target-volume installation and upgrades, with distribution package sets,
configuration preservation and recovery. Resident components, deferred
activation and boot fallback have explicit handling. Installer integration uses
an agreed interface with recorded effects; rollback covers only those effects.

## Publication and qualifications

One author description feeds packaging and publication. Authors can publish
signed packages through a directory, share, web channel or portal account.
Discovery reads bounded metadata without creating root state. Explicit transfer
prepares private retained content, verifies the whole archive and its exact
selected descriptor/signature, and rechecks source and policy before retention.
Transfer does not commit installed state. The [channel profile](channels.md)
defines current host transport limits and remaining plan-origin obligations.

Catalogue review is an independent workflow. A reviewed catalogue can select
packages from open channels without making review a prerequisite for publication.

| Fact | Authority and scope |
|---|---|
| Signature | The signer's key authenticates exact signed content; trust is evaluated separately. |
| Review | An independent reviewer attests to checks on an exact manifest digest and package identity. |
| Runtime test | A tester identifies exact content, target and environment exercised. |

Author metadata cannot self-award review or runtime qualifications. Content
changes invalidate inherited qualifications. Channel and author signatures
have separate roles. Official key bootstrap, custody, rotation and recovery
require an explicit publishing policy.

The portal is a service over this model. Its unavailability cannot prevent
local publication, installation from another channel or offline recovery.
Stable/testing/internal selection is independent of channel ownership and
review status.

## Migration and implementation boundaries

The original pkg side permits breaking command, API and storage changes;
consumers are adapted with the implementation. Released APKG installations
require a working upgrade path. Import preserves original records, custom roots,
versions, revisions, edits, settings, launch metadata, installation reasons and
relations. It commits only after validation. Interruption leaves the original
installation usable or recoverable.

Compatibility adapters protect established APKG users and scripts while the
common model owns the resulting installation. Old index URLs remain available
through the upgrade path; this workspace does not unilaterally retire upstream
endpoints. The [acceptance matrix](acceptance.md) defines evidence for these
contracts; [provenance](provenance.md) records source ancestry and licences.
