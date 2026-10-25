<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Acceptance matrix

These rows define preserved capabilities and evidence needed to accept them.
They are obligations, not passes. [Implementation status](status.md) records
completed and outstanding work against the [architecture](architecture.md).

| Capability | Common behavior | Acceptance evidence |
|---|---|---|
| One library | Clients share contexts, resolution, ownership and transactions. | Link and integration checks exclude a second mutation engine; CLI and GUI produce equivalent state. |
| Read-only access | Browsing and diagnostics create no root state. | Filesystem comparisons around reads of existing, absent and interrupted roots. |
| PkgManager browsing | Preserve search, filtering, details, recent additions, updates and settings. | Native UI/ARexx scenarios on supported distributions. |
| PkgManager operations | Preserve plans, cancellation, Run and folder opening; expose repair, system writes and signer facts. | Native scenarios inspect resulting state, close-during-work and allocation-failure outcomes. |
| Exact plans | Execute approved digests, providers, destinations and trust changes. | Change channel, root, content, signer or provider after preview; refuse before mutation. |
| CLI workflows | Preserve installation, diagnostics, environments, authoring, signing, publishing and automation. | Scripted commands cover success, refusals, structured output, arguments and exit codes. |
| Existing APKG scripts | Provide a working command and behavior upgrade path. | Representative scripts and output/exit-code contracts from both existing distributions. |
| Authoring | One TOML description drives validation, bundles and signed publication. | End-to-end round trip preserves metadata, files, attributes and launch description with untouched inputs. |
| Open publishing | Authors publish without curator approval or a central account. | Signed directory/share/web channels consumed directly; portal publication succeeds without review. |
| Curated catalogue | Review is independent of publication and tied to exact content. | Authors cannot grant reviewer status; changed content/target cannot inherit a qualification. |
| Runtime qualifications | Results identify content, target and environment. | Record validation and UI display; rejection of misleading or stale claims. |
| Trust | Separate author/channel signatures, key trust, review and testing. | Tampering, unknown key, replacement key, bootstrap and rotation; no silent trust expansion. |
| Targets | Select CPU/OS/ABI/runtime independently of publishing host. | Cross-host publication and consumption; CPU, ABI and runtime mismatches refused. |
| Versions | Preserve upstream versions, revisions and prerelease ordering. | Shared corpus from both projects, indeterminate comparisons and explicit downgrades. |
| Relationships | Preserve depends, recommends, conflicts, replaces and installation reasons. | Both suites plus combined plans and replacement interruption cases. |
| Providers | Prefer compatible installed providers and show changes. | Multiple providers, incompatible ABIs, package/capability conflicts and actual loader resolution. |
| Bundles | Launch applications without registry membership. | Manual unpack and launch, moved bundles, managed adoption/movement and hostile paths. |
| Payload fidelity | Preserve empty directories, hidden files, icons and AROS attributes. | Independent installed-tree comparison after host packaging and AROS installation. |
| Ownership/configuration | Preserve edits and unknown files; refuse unauthorized takeover. | Upgrade/remove/repair with edited, missing, foreign and shared files. |
| Repair | Restore verified content under trust and configuration policy. | Corrupt/missing files, edited configuration and withdrawn/unavailable sources. |
| Cancellation | Expose cancellable phases and accurate final outcomes. | Cancel during transfer, verification, preparation and mutation; verify restoration or explicit partial state. |
| Recovery | Repeat recovery after operation or recovery interruption. | Inject failures between mutation and recording; disk-full, termination and power-loss cases. |
| Rollback | Retained content supports declared offline recovery. | Disconnect channels, remove remote archives, withdraw versions and exercise retention policy. |
| Migration | Preserve original APKG records, roots, revisions, edits, settings, launch data and relations. | Fixtures and distribution runs cover validation failure, interruption, retry and original-install usability. |
| APKG history | Define import/preservation of old rollback history. | Migration reports retained/unavailable history; rollback matches the report. |
| Portal | Preserve discovery, package/version/dependency/file views, provenance, accounts, resumable push and administration. | API tests and rendered pages with open and reviewed publications. |
| Host workflows | Preserve macOS, Linux and Windows packaging, signing, publishing and automation. | Shared command/channel contracts executed on each host. |
| AROS targets | Cover x86_64 ABIv11, i386 ABIv0, x86_64 ABIv1, native Pi aarch64 and hosted aarch64. | Target-specific builds and runtime evidence; experimental builds state qualification. |
| Filesystems | Supported targets meet atomicity and durability requirements. | DOS/handler capability checks, failure and interruption tests. |
| Fresh installation | Install distribution package sets onto an explicit target volume. | Empty-volume installation, ownership verification and boot. |
| System upgrades | Preserve configuration, resident-library handling, deferred activation and boot fallback. | Interrupted updates and failed-boot recovery on representative targets. |
| Installer integration | Use an agreed interface with recorded effects. | File install and configuration edit, cancellation and failure using an implemented upstream interface. |
| Consumer adaptations | Existing pkg consumers adopt intentional common-model changes. | Each consumer's checks pass against the pinned common API and command build. |

The first merged release requires an explicit subset agreed by both maintainers.
Deferred system-installation and desktop work retains its obligations here.
Each experimental target identifies its exact test coverage. Both inherited
suites run alongside integration cases on final source and build identities.
