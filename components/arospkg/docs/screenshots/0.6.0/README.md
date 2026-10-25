# arospkg 0.6 screenshots: what is new, Listed and Reviewed

From the development build of 2026-10-14 (apkg 309f3801..., PkgManager
732900c9...) on AROS One 1.3, x86_64 ABIv11, 1024x768. Terminal examples use
Limpet 0.4.1. These are working screenshots for the coming 0.6 release; the
release adds its own.

What is real and what was prepared:

- The catalogue fetched is the published one, from
  `raw.githubusercontent.com/tomaszstaniak/arospkg-index/main/index-v2.json`.
- The catalogue it replaced was prepared for the test: the published one
  without limpet, arospkg and xrick, without Folio's x86_64 build, and with
  older versions of scopagame and zgloom, recorded as fetched on
  2026-10-01. So the news shown is the difference between those two files,
  not a real week of the catalogue.

## PkgManager: after Update index

![The line under the list after Update index](news-after-update-index.png)

## Last refresh, with a build's status, runtime tests and dates

![Last refresh with xrick selected](last-refresh-details.png)

## Recently added

![Recently added, newest first](recently-added.png)

## Limpet: apkg update, apkg recent, apkg show

![apkg update with its summary of what is new](limpet-update-news.png)

![apkg recent --limit 6](limpet-recent.png)

![apkg show xrick: Review and tests](limpet-show-status.png)

## Relations: dependencies, replacement, the removal guard

From the development build of 2026-10-16 (apkg 716850a2..., PkgManager of
the same commit) on AROS One 1.3, with a local test catalogue: Bishop and
Hardcode are internal test packages, not published; rellib, relapp and the
other `rel*` packages are small test packages made for these runs.

![Install bishop: the plan replaces hardcode](replace-confirm.png)

![Bishop's details: Limpet recommended, Hardcode replaced](relations-details.png)

![Install relapp: two packages it needs come first](dependencies-confirm.png)

![Remove rellib: relapp needs it](remove-needed-refused.png)

![Limpet: the replacement asked about and done](limpet-replace-bishop.png)

