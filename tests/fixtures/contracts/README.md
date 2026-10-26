<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Data-model decision corpus

These original MIT fixtures accompany
[the contract](../../../docs/fusion/data-model.md). They are synthetic decisions,
not captured distribution data or proof of a working common engine.

`versions.json` records arospkg `version_compare` results (-1, 0, 1, 2 for
unordered) and `upgrade_allowed` decisions alongside the common comparison.
The common comparison intentionally recognizes exact opaque identity, whereas
the inherited comparator reports unordered for every unparsed version. Its
upgrade predicate already accepts an identical opaque string with increased
packaging revision. Revision 0 models a missing legacy revision's effective
value; absence/provenance must still be retained by migration.

`targets.json` tests the current qualified-target parser and exact matching.
`generic` and CPU-only strings are not concrete targets. Architecture-independent
data is a separate contract declaration; it cannot use a concrete-target
parser failure to bypass executable validation.

Validation uses the existing `components/arospkg/src/libpkg/upgrade.c` and
`src/pkg_target.c` in a temporary host harness. It checks inherited decisions
and target syntax/matching, not future database serialization, the common opaque
comparison implementation, signatures, migration, or runtime compatibility.

`wire-vectors.json` fixes canonical bytes and the domain-separated signature
transcript for a framing-only synthetic object. It contains public-key bytes
but no private key or valid signature. Run `make test-contract` (or
`python3 tests/contracts/check.py --sanitize`) to reproduce the corpus checks,
framing rejection cases, executable consumer/test-double and compile-only plan
client checks. No fake is linked into the production library.
