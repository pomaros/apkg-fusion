<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# PkgManager combined library

The main PkgManager list combines captured installed records with candidates
from the loaded signed catalogue. It uses the common typed rows, not imported
fixed-size `pkg_entry` conversions. The separate catalogue window retains its
more detailed trust, target, observation and comparison filters.

## Rows and filters

An installed row describes the exact committed descriptor. Its summary and
category come from that descriptor, including when the catalogue contains only
a newer build with different metadata. Missing descriptive text remains
unrecorded; a recorded empty category remains distinct. No live payload check,
runtime qualification or current trust assertion follows from list membership.

Catalogue candidates retain full authority, name, target, comparison and exact
signed selection. A candidate identical to an installed build is represented
by the installed row. Older, newer, conflicting and foreign-authority candidates
remain separate. Same-name authorities are never merged into one package.
The main display retains packaging revisions, including full 64-bit values.

The main state filters have these meanings:

| Filter | Rows |
|---|---|
| All | Installed records and non-identical catalogue candidates. |
| Installed | Installed records, once per record. |
| Not installed | Candidates classified NOT_INSTALLED by the common comparison. A foreign authority colliding with an occupied root name is not silently classified as a missing installable package. |
| Updates | Candidates classified NEWER. Trust/signature/target blocks remain visible and prevent installation approval through the normal common flow. |
| Recently added | The newest 25 current-target application additions selected by the common [publication query](publication-news.md), ordered by publisher-reported first day. Unknown dates and ambiguous build groups are counted separately. |
| Last refresh | The common [observation comparison](last-refresh.md), restored from the profile or captured session observations. Missing baseline and invalid evidence remain distinct from a valid zero-change refresh. |

Search covers full names, authorities, targets, summaries and categories.
Categories use the full top-level component before `/`, with owned dynamic
choices rather than the inherited 32-category/fixed-label arrays. Choices come
from the whole combined list, so a search with no hits does not erase them.
The model bounds installed records at 4,096 and combined inputs at 8,192;
overflow refuses with LIMIT rather than truncating rows or category names.

Broader settings and remaining automation are unfinished integration work.
Recently added uses signed publisher claims and never substitutes installation
time or file modification time for missing dates. Last refresh retains its
actual pair through display filtering and offline recomparison, with explicit
comparison errors and source changes separate from installed state.

## Actions and lifetime

ARexx `SHOW name` refreshes the same combined view offline and clears its
search/category/state filters. An installed row takes precedence; without one,
the name must identify exactly one catalogue row. Multiple versions, targets
or authorities refuse with CONFLICT, directing selection to the window.
Missing names report NOTFOUND; refresh failures cannot succeed against stale
displayed rows. SHOW refuses BUSY before changing controls while a worker or
confirmation can borrow the current models. This is display navigation, not
installation or execution authorization.

Installed rows enable local Run, folder, Verify, Remove, Upgrade and Rollback
requests. Candidate rows enable review of an exact signed installation/update
when their metadata allows it. They do not masquerade as installed rows for
local file operations. Execution still prepares and approves a common plan;
the visible list is not an execution authorization.

The UI refreshes comparisons from the captured signed observation offline and
reads a fresh installed snapshot before combining rows. The pure combined-view
builder requires equal root generation and snapshot digest. An unavailable
catalogue leaves installed records usable and availability/update knowledge
explicitly unknown. A failed refresh preserves the previous displayed model
and reports that it was not refreshed.

The combined AllocVec owns wrapper rows and category labels and borrows the
two complete parent models. Those inputs must outlive it. The UI clears both
lists and borrowed detail/category pointers before replacing parents, preserves
selection by complete build identity, and disables the main search/filter
controls while a worker or confirmation may still borrow their input. A
queued refresh also refuses at that boundary. It never reuses shortened package
names to recover a selection after filtering.

Host evidence is recorded in [status](status.md). Native SDK compilation is
distinct from the final M1 interaction and lifecycle test.
