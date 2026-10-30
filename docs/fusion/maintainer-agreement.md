<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Maintainer alignment handoff

The implementation follows the
[selected skeleton](architecture.md#implementation-skeleton) as an engineering
proposal. Joint decisions are still open; reviewing them together is the next
collaboration step before assigning further implementation. This file records
the feedback to obtain; it grants no authority to contact anyone. A joint release requires the release agreement
defined in [PLAN](../../PLAN.md#release-gate).

| Topic | Proposed direction | Feedback to record |
|---|---|---|
| Skeleton | New common engine on arospkg's context/client model, linking pkg services; both original engines kept as regression baselines |  Maintainers' comments, accepted changes and unresolved objections |
| Recovery | One context journal for packages and self-update | Durable boundaries, recovery cases and migration of old journals |
| Publication | Open author channels with independent review/runtime evidence | Official channel/key ownership and reviewer administration |
| Existing users | Preserve released APKG installations and script behavior | Distribution names, releases, custom-root cases and representative records |
| First release | Explicit subset of the acceptance matrix | Included operations, clients, targets and measured qualification |

The next collaboration step is to review these proposals together, record
agreement or changes, and assign distinct implementation tasks. The
[selective 0.7.2 maintenance overlay](upstream-0.7.2.md) does not resolve any of
these joint decisions. In particular, no implementation choice in this checkout
is an approval on behalf of the other maintainer.

AROS One 1.3 x86_64 ABIv11 is named in the pinned arospkg release notes and
screenshots. The second distribution described in the merger proposal is not
identified in the examined public sources. Neither distribution's installed
dos.library version/revision is established by those sources. MAP records these
as unverified fields and collects them when evidence becomes available.

The pkg backend requires DOS 50.84 for its `ReplaceObject` path. A target's CPU
and ABI do not prove that requirement is met. Compatibility and recovery tests
must exercise the actual distribution/handler combination before claiming its
support; development of the common contracts can proceed meanwhile.

No contact, maintainer approval or distribution runtime result is recorded by
this document. Record actual feedback and evidence when obtained.
