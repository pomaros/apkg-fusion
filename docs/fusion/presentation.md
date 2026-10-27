<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Signed descriptive metadata

A common descriptor may carry an optional `body.presentation` object with
`summary`, `category`, `license` and `repository` string fields. They are author
declarations covered by the descriptor signature and digest. They confer no
review, runtime-test result, channel privilege or author trust. Unknown fields,
including attempted review/test status, refuse the descriptor.

Summary, license and repository are bounded to 4,096 UTF-8 bytes each; category
is bounded to 255. Quotes, backslashes and UTF-8 text are preserved through the
canonical JSON framing. Control characters, embedded NUL, DEL, non-string values
and overlong text refuse instead of truncating. These are single-line descriptive
fields. Repository is displayed as text, not fetched or executed.

An absent object or field is unknown. An explicitly empty string is recorded
empty. The descriptor and installed/channel views preserve the original optional
object bytes. `apkg_presentation_field` decodes a named field to owned text without
I/O or authentication; NOT_FOUND represents absence and OK can return an empty
string. Free returned text with `apkg_client_text_free`.

Common authoring maps TOML `summary`, `category`, `license` and
`source.repository` into this signed object, while preserving the original TOML.
The inherited technical authoring checks remain in use. Its legacy index string
rules still apply to inherited publication; common descriptive text is validated
by the common engine's schema rather than that index reader's ASCII limitations.
All product consumers must be rebuilt together. Older fusion binaries that do
not recognize the presentation field refuse it; they do not silently discard it
or rewrite an existing published build.

`channel-open`, `channel-updates` and `channel-inspect` include the optional
object as `presentation` (null when absent). Installed INFO exposes the named
fields and reports `unknown` for absence. The compatibility JSON adapter uses
null and its `unrecorded` list for absent summary/category; recorded empty values
stay empty strings. PkgManager's owned catalogue rows copy decoded summary and
category, use them in text matching and show descriptive fields in package
details. The retained signed observation remains the complete source of facts.

This metadata path does not by itself complete the inherited category selector,
recent/last-refresh views, or other [client closure work](non-portal-checklist.md).
Host string/row tests prove byte preservation and ownership; native font rendering
and the completed GUI remain part of the candidate's M1 test. Evaluated results
are recorded in [status](status.md).
