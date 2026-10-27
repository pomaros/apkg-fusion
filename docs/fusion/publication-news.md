<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Publication dates and recent additions

Recent additions are channel-publisher claims, separate from author signature,
review, runtime qualification and online freshness. They do not use filesystem
modification times, installation dates or the reader's clock.

## Signed channel fields

A channel body may contain `published_on`. An entry may contain `publication`
with optional `first_added` and `app_first_added` fields. These dates are UTC
calendar days in `YYYY-MM-DD` form, with Gregorian leap-year validation and
years 0001 through 9999. Absent fields remain unknown. Empty strings, invalid
days, unknown publication fields, an application date later than its build
date, or an entry date later than the known channel publication day refuse.
The optional fields preserve reading older signed channels without dates.
Older verifiers that do not understand this extension require an update; they
must refuse unknown semantics rather than silently accepting them.

```json
"publication":{"app_first_added":"2025-01-01","first_added":"2026-02-01"}
```

`app_first_added` identifies the application under its authority/name across
targets. `first_added` identifies its target build, across subsequent versions
and packaging revisions. These are signed channel metadata, not alterations
to the author's descriptor or its signature. They cannot grant reviewer roles
or move a root's accepted channel sequence backwards.

`apkg-channel` records today's UTC day by default; `--published-on YYYY-MM-DD`
lets the publisher provide an explicit day. New versions preserve first dates
from the related entries in the previous head. A newly introduced target gets
its own first day and retains the application's known first day. Related older
entries without dates prevent automatic backfilling of the missing history.
An unchanged archive/observation publication remains idempotent and does not
rewrite dates merely because it is retried another day. A changed publication
cannot use a day earlier than the previous known head publication.

## Shared query

`apkg_channel_recent` selects current-target groups by full authority/name/target.
It chooses the newest orderable version and revision within a group. A group
requiring indeterminate version comparisons is counted and excluded, rather
than selecting by filename, input order or spelling. A single unorderable
version needs no ordering comparison and remains a valid unique build.

The default lists applications by their publisher-reported first day, newest
first, limited to 25. A target introduced later than the application is listed
only with the builds option, using its target's first day. Missing application
dates are counted as unknown and not assigned a guessed position. Equal-day
ties use complete name/authority/target text for deterministic presentation.
The inclusive since filter and explicit limit operate on these dates; zero
limit requests all results. Inputs and limits are bounded at 4,096 entries.

The result owns its entry array but borrows immutable dates and package views
from the parent channel, which must outlive it. Formatting returns owned text
released with `apkg_client_text_free`. It exposes source sequence/digest,
unknown-date and indeterminate-group counters, and explicitly unknown freshness.
Reading and formatting perform no writes, network requests or clock queries.

```text
apkg recent ROOT ENDPOINT CHANNEL-ID [--builds] [--since YYYY-MM-DD] [--limit N] [--json]
apkg source-recent ROOT PROFILE [same options]
```

The first command discovers signed metadata normally. The second revalidates
the retained source profile under current root trust and replay rules without
contacting the source. Neither command saves preferences or advances replay
floors. This syntax is the common CLI; adapting the historical CLI profile is
a separate compatibility task.

PkgManager's Recently added view uses the same selection and default limit.
The worker copies ranks, complete identities, publication days and counters
before releasing library objects. Both installed-current and available rows
can participate, with their usual distinct action eligibility. Categories and
search then filter those recent results. The UI labels dates as publisher
reports, not verification of when an application really first existed.

## Last-refresh comparison

Recently added does not establish what changed during the last refresh. That
view uses [two authenticated source observations](last-refresh.md), an explicit
missing-baseline case and retained comparison evidence. A first-observation
recent fallback stays labelled as such; it is not a fabricated refresh delta.
Implementation and test evidence belong in [status](status.md).
